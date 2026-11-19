// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "capture.h"
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace live {

template <typename T>
static void safeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

bool ScreenCapture::init(CaptureMethod method)
{
    if (m_initialized) return true;
    m_requested = method;

    if (method == CaptureMethod::Auto || method == CaptureMethod::DXGI) {
        if (initDXGI()) {
            m_method = CaptureMethod::DXGI;
            m_initialized = true;
            return true;
        }
    }

    // GDI is always available on Windows
    if (method == CaptureMethod::Auto || method == CaptureMethod::GDI) {
        m_method = CaptureMethod::GDI;
        m_initialized = true;
        return true;
    }

    // DXGI explicitly requested but unavailable: keep m_error from initDXGI
    return false;
}

void ScreenCapture::shutdown()
{
    shutdownDXGI();
    m_initialized = false;
}

bool ScreenCapture::captureRegion(void* windowHandle, CaptureRegion region, CapturedFrame& out)
{
    if (!m_initialized) { m_error = "Capture not initialized"; return false; }
    if (region.width <= 0 || region.height <= 0) { m_error = "Invalid region dimensions"; return false; }

    // Desktop Duplication works on screen coordinates only; window-relative capture uses GDI.
    if (m_method == CaptureMethod::DXGI && !windowHandle)
        return captureDXGI(region, out);
    return captureGDI(windowHandle, region, out);
}

bool ScreenCapture::captureFullWindow(void* windowHandle, CapturedFrame& out)
{
#ifdef _WIN32
    HWND hWnd = static_cast<HWND>(windowHandle);
    if (!hWnd) {
        return captureRegion(nullptr, {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                       GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)}, out);
    }
    RECT rect;
    if (!GetClientRect(hWnd, &rect))
        return false;
    return captureRegion(hWnd, {0, 0, rect.right, rect.bottom}, out);
#else
    (void)windowHandle; (void)out;
    return false;
#endif
}

// -- DXGI Desktop Duplication --

bool ScreenCapture::initDXGI()
{
#ifdef _WIN32
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_d3dDevice, &featureLevel, &m_d3dContext);
    if (FAILED(hr)) {
        m_error = "D3D11CreateDevice failed";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) {
        m_error = "QueryInterface IDXGIDevice failed";
        shutdownDXGI();
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
    dxgiDevice->Release();
    if (FAILED(hr)) {
        m_error = "GetParent IDXGIAdapter failed";
        shutdownDXGI();
        return false;
    }

    // Primary output only; regions outside it are captured with GDI.
    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) {
        m_error = "EnumOutputs(0) failed";
        shutdownDXGI();
        return false;
    }

    DXGI_OUTPUT_DESC outDesc;
    output->GetDesc(&outDesc);
    m_outLeft = outDesc.DesktopCoordinates.left;
    m_outTop = outDesc.DesktopCoordinates.top;
    m_outRight = outDesc.DesktopCoordinates.right;
    m_outBottom = outDesc.DesktopCoordinates.bottom;

    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&m_output));
    output->Release();
    if (FAILED(hr)) {
        m_error = "QueryInterface IDXGIOutput1 failed";
        shutdownDXGI();
        return false;
    }

    if (!createDuplication()) {
        shutdownDXGI();
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool ScreenCapture::createDuplication()
{
#ifdef _WIN32
    HRESULT hr = m_output->DuplicateOutput(m_d3dDevice, &m_dxgiDup);
    if (FAILED(hr)) {
        m_dxgiDup = nullptr;
        m_error = "DuplicateOutput failed (hr=" + std::to_string(hr) + ")";
        return false;
    }
    m_hasLastFrame = false;
    return true;
#else
    return false;
#endif
}

void ScreenCapture::releaseDuplication()
{
#ifdef _WIN32
    safeRelease(m_dxgiDup);
    m_hasLastFrame = false;
#endif
}

bool ScreenCapture::captureDXGI(CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    // Region must lie entirely on the duplicated (primary) output.
    if (region.x < m_outLeft || region.y < m_outTop
        || region.x + region.width > m_outRight || region.y + region.height > m_outBottom)
        return captureGDI(nullptr, region, out);

    // Duplication is lost on mode changes, fullscreen switches, UAC/secure desktop.
    // Recreate it, throttled so a persistent failure doesn't cost time every frame.
    if (!m_dxgiDup) {
        uint64_t now = GetTickCount64();
        if (now < m_nextDupRetryMs || !createDuplication()) {
            m_nextDupRetryMs = now + 500;
            return captureGDI(nullptr, region, out);
        }
    }

    // Don't block when we already have an image: a timeout just means "nothing changed".
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    HRESULT hr = m_dxgiDup->AcquireNextFrame(m_hasLastFrame ? 0 : 100, &frameInfo, &resource);

    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        releaseDuplication();
        m_nextDupRetryMs = 0;
        return captureGDI(nullptr, region, out);
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!m_hasLastFrame)
            return captureGDI(nullptr, region, out);
    } else if (FAILED(hr)) {
        m_error = "AcquireNextFrame failed (hr=" + std::to_string(hr) + ")";
        return captureGDI(nullptr, region, out);
    } else {
        // New frame (or pointer-only update). Keep a GPU copy of the desktop image.
        ID3D11Texture2D* desktopTex = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopTex));
        resource->Release();
        if (SUCCEEDED(hr)) {
            bool imageUpdated = frameInfo.LastPresentTime.QuadPart != 0 || !m_hasLastFrame;
            if (imageUpdated) {
                D3D11_TEXTURE2D_DESC desc;
                desktopTex->GetDesc(&desc);
                if (m_lastDesktop) {
                    D3D11_TEXTURE2D_DESC have;
                    m_lastDesktop->GetDesc(&have);
                    if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
                        safeRelease(m_lastDesktop);
                }
                if (!m_lastDesktop) {
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_lastDesktop)))
                        m_lastDesktop = nullptr;
                }
                if (m_lastDesktop) {
                    m_d3dContext->CopyResource(m_lastDesktop, desktopTex);
                    m_hasLastFrame = true;
                }
            }
            desktopTex->Release();
        }
        m_dxgiDup->ReleaseFrame();
        if (!m_hasLastFrame) {
            m_error = "Could not cache desktop frame";
            return captureGDI(nullptr, region, out);
        }
    }

    // Region in output-local coordinates
    const int rx = region.x - m_outLeft;
    const int ry = region.y - m_outTop;
    const int rw = region.width;
    const int rh = region.height;

    D3D11_TEXTURE2D_DESC deskDesc;
    m_lastDesktop->GetDesc(&deskDesc);

    if (!m_staging || m_stagingW != rw || m_stagingH != rh) {
        safeRelease(m_staging);
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = static_cast<UINT>(rw);
        stagingDesc.Height = static_cast<UINT>(rh);
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = deskDesc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &m_staging))) {
            m_staging = nullptr;
            m_error = "CreateTexture2D staging failed";
            return false;
        }
        m_stagingW = rw;
        m_stagingH = rh;
    }

    D3D11_BOX box;
    box.left = static_cast<UINT>(rx);
    box.top = static_cast<UINT>(ry);
    box.front = 0;
    box.right = static_cast<UINT>(rx + rw);
    box.bottom = static_cast<UINT>(ry + rh);
    box.back = 1;
    m_d3dContext->CopySubresourceRegion(m_staging, 0, 0, 0, 0, m_lastDesktop, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        m_error = "Map staging texture failed";
        return false;
    }

    out.width = rw;
    out.height = rh;
    out.pixels.resize(static_cast<size_t>(rw) * rh * 4);
    out.timestamp = GetTickCount64();

    // Copy row-by-row (RowPitch may differ from rw*4)
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out.pixels.data();
    const size_t rowBytes = static_cast<size_t>(rw) * 4;
    for (int y = 0; y < rh; ++y)
        std::memcpy(dst + y * rowBytes, src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);

    m_d3dContext->Unmap(m_staging, 0);
    return true;
#else
    (void)region; (void)out;
    return false;
#endif
}

void ScreenCapture::shutdownDXGI()
{
#ifdef _WIN32
    releaseDuplication();
    safeRelease(m_staging);
    safeRelease(m_lastDesktop);
    safeRelease(m_output);
    safeRelease(m_d3dContext);
    safeRelease(m_d3dDevice);
    m_stagingW = m_stagingH = 0;
#endif
}

// -- Window content (PrintWindow) --

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

bool ScreenCapture::captureWindow(void* hwnd, CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    // The window renders itself into our bitmap, so covering windows never appear in the image and
    // DirectComposition / flip-model content (video, games, Remote Play) is included.
    HWND hWnd = static_cast<HWND>(hwnd);
    RECT rc{};
    if (!GetClientRect(hWnd, &rc) || IsIconic(hWnd))
        return false;
    const int cw = rc.right, ch = rc.bottom;
    const int w = region.width, h = region.height;
    if (cw <= 0 || ch <= 0 || region.x < 0 || region.y < 0 || region.x + w > cw || region.y + h > ch)
        return false;

    HDC wndDC = GetDC(hWnd);
    if (!wndDC)
        return false;
    HDC memDC = CreateCompatibleDC(wndDC);
    HBITMAP bitmap = memDC ? CreateCompatibleBitmap(wndDC, cw, ch) : nullptr;
    if (!bitmap) {
        if (memDC) DeleteDC(memDC);
        ReleaseDC(hWnd, wndDC);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);
    const BOOL printed = PrintWindow(hWnd, memDC, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    SelectObject(memDC, oldBitmap);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = cw;
    bi.biHeight = -ch;  // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    std::vector<uint8_t> full(static_cast<size_t>(cw) * ch * 4);
    const int lines = printed ? GetDIBits(memDC, bitmap, 0, static_cast<UINT>(ch), full.data(),
                                          reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS)
                              : 0;
    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(hWnd, wndDC);
    if (lines != ch)
        return false;

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    out.timestamp = GetTickCount64();
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    bool anyLight = false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = full.data() + (static_cast<size_t>(region.y + y) * cw + region.x) * 4;
        uint8_t* dst = out.pixels.data() + y * rowBytes;
        std::memcpy(dst, src, rowBytes);
        if (!anyLight)
            for (size_t i = 0; i < rowBytes; i += 4)
                if (src[i] | src[i + 1] | src[i + 2]) { anyLight = true; break; }
    }
    // Entirely black: the window refused to render (protected content / old app). Let the caller fall back.
    return anyLight;
#else
    (void)hwnd; (void)region; (void)out;
    return false;
#endif
}

// -- GDI BitBlt --

bool ScreenCapture::captureGDI(void* hwnd, CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    if (hwnd && captureWindow(hwnd, region, out))
        return true;
    // null → whole virtual screen DC (screen coordinates, all monitors)
    HWND hWnd = static_cast<HWND>(hwnd);
    HDC srcDC = GetDC(hWnd);
    if (!srcDC) {
        m_error = "GetDC failed";
        return false;
    }

    const int w = region.width;
    const int h = region.height;
    HDC memDC = CreateCompatibleDC(srcDC);
    HBITMAP bitmap = memDC ? CreateCompatibleBitmap(srcDC, w, h) : nullptr;
    if (!bitmap) {
        if (memDC) DeleteDC(memDC);
        ReleaseDC(hWnd, srcDC);
        m_error = "CreateCompatibleBitmap failed";
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);

    BOOL blitOk = BitBlt(memDC, 0, 0, w, h, srcDC, region.x, region.y, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;  // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    out.timestamp = GetTickCount64();

    SelectObject(memDC, oldBitmap);
    int lines = blitOk ? GetDIBits(memDC, bitmap, 0, static_cast<UINT>(h),
                                   out.pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS)
                       : 0;

    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(hWnd, srcDC);

    if (lines != h) {
        m_error = "GDI capture failed";
        return false;
    }
    return true;
#else
    (void)hwnd; (void)region; (void)out;
    return false;
#endif
}

} // namespace live
