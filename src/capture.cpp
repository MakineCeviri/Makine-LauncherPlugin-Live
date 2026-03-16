#include "capture.h"
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace live {

bool ScreenCapture::init(CaptureMethod method)
{
    if (m_initialized) return true;

    if (method == CaptureMethod::Auto || method == CaptureMethod::DXGI) {
        if (initDXGI()) {
            m_method = CaptureMethod::DXGI;
            m_initialized = true;
            return true;
        }
    }

    if (method == CaptureMethod::Auto || method == CaptureMethod::GDI) {
        if (initGDI()) {
            m_method = CaptureMethod::GDI;
            m_initialized = true;
            return true;
        }
    }

    m_error = "No capture method available";
    return false;
}

void ScreenCapture::shutdown()
{
    shutdownDXGI();
    m_initialized = false;
}

bool ScreenCapture::captureRegion(void* windowHandle, CaptureRegion region, CapturedFrame& out)
{
    if (!m_initialized) return false;

    switch (m_method) {
        case CaptureMethod::DXGI:  return captureDXGI(windowHandle, region, out);
        case CaptureMethod::GDI:   return captureGDI(windowHandle, region, out);
        default: return false;
    }
}

bool ScreenCapture::captureFullWindow(void* windowHandle, CapturedFrame& out)
{
#ifdef _WIN32
    HWND hWnd = static_cast<HWND>(windowHandle);
    if (!hWnd) hWnd = GetDesktopWindow();
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
    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &m_d3dDevice, &featureLevel, &m_d3dContext);

    if (FAILED(hr)) {
        m_error = "D3D11CreateDevice failed";
        return false;
    }

    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) {
        m_error = "QueryInterface IDXGIDevice failed";
        shutdownDXGI();
        return false;
    }

    // Get adapter
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
    dxgiDevice->Release();
    if (FAILED(hr)) {
        m_error = "GetParent IDXGIAdapter failed";
        shutdownDXGI();
        return false;
    }

    // Get primary output
    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) {
        m_error = "EnumOutputs(0) failed";
        shutdownDXGI();
        return false;
    }

    // Get IDXGIOutput1 for DuplicateOutput
    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    output->Release();
    if (FAILED(hr)) {
        m_error = "QueryInterface IDXGIOutput1 failed";
        shutdownDXGI();
        return false;
    }

    // Create desktop duplication
    hr = output1->DuplicateOutput(m_d3dDevice, &m_dxgiDup);
    output1->Release();
    if (FAILED(hr)) {
        m_error = "DuplicateOutput failed (hr=" + std::to_string(hr) + ")";
        shutdownDXGI();
        return false;
    }

    return true;
#else
    return false;
#endif
}

bool ScreenCapture::captureDXGI(void* /*hwnd*/, CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    if (!m_dxgiDup || !m_d3dDevice || !m_d3dContext) return false;

    // Acquire next frame
    IDXGIResource* resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = m_dxgiDup->AcquireNextFrame(100, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available
        return false;
    }
    if (FAILED(hr)) {
        m_error = "AcquireNextFrame failed (hr=" + std::to_string(hr) + ")";
        return false;
    }

    // Get the desktop texture
    ID3D11Texture2D* desktopTex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopTex));
    resource->Release();
    if (FAILED(hr)) {
        m_dxgiDup->ReleaseFrame();
        m_error = "QueryInterface ID3D11Texture2D failed";
        return false;
    }

    // Get desktop texture description for bounds checking
    D3D11_TEXTURE2D_DESC deskDesc;
    desktopTex->GetDesc(&deskDesc);

    // Clamp region to desktop bounds
    int rx = region.x;
    int ry = region.y;
    int rw = region.width;
    int rh = region.height;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > static_cast<int>(deskDesc.Width))
        rw = static_cast<int>(deskDesc.Width) - rx;
    if (ry + rh > static_cast<int>(deskDesc.Height))
        rh = static_cast<int>(deskDesc.Height) - ry;

    if (rw <= 0 || rh <= 0) {
        desktopTex->Release();
        m_dxgiDup->ReleaseFrame();
        m_error = "Invalid capture region after clamping";
        return false;
    }

    // Create staging texture for CPU read
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = static_cast<UINT>(rw);
    stagingDesc.Height = static_cast<UINT>(rh);
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = deskDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTex = nullptr;
    hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr)) {
        desktopTex->Release();
        m_dxgiDup->ReleaseFrame();
        m_error = "CreateTexture2D staging failed";
        return false;
    }

    // Copy the requested region
    D3D11_BOX box;
    box.left = static_cast<UINT>(rx);
    box.top = static_cast<UINT>(ry);
    box.front = 0;
    box.right = static_cast<UINT>(rx + rw);
    box.bottom = static_cast<UINT>(ry + rh);
    box.back = 1;

    m_d3dContext->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, desktopTex, 0, &box);
    desktopTex->Release();

    // Map staging texture and copy pixels
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stagingTex->Release();
        m_dxgiDup->ReleaseFrame();
        m_error = "Map staging texture failed";
        return false;
    }

    out.width = rw;
    out.height = rh;
    out.pixels.resize(static_cast<size_t>(rw * rh * 4));
    out.timestamp = GetTickCount64();

    // Copy row-by-row (RowPitch may differ from rw*4)
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out.pixels.data();
    size_t rowBytes = static_cast<size_t>(rw) * 4;
    for (int y = 0; y < rh; ++y) {
        std::memcpy(dst + y * rowBytes, src + y * mapped.RowPitch, rowBytes);
    }

    m_d3dContext->Unmap(stagingTex, 0);
    stagingTex->Release();
    m_dxgiDup->ReleaseFrame();

    return true;
#else
    (void)region; (void)out;
    return false;
#endif
}

void ScreenCapture::shutdownDXGI()
{
#ifdef _WIN32
    if (m_dxgiDup) { m_dxgiDup->Release(); m_dxgiDup = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
#endif
}

// -- GDI BitBlt --

bool ScreenCapture::initGDI()
{
    // GDI always available on Windows
    return true;
}

bool ScreenCapture::captureGDI(void* hwnd, CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    HWND hWnd = static_cast<HWND>(hwnd);
    if (!hWnd) hWnd = GetDesktopWindow();

    HDC hWndDC = GetDC(hWnd);
    if (!hWndDC) {
        m_error = "GetDC failed";
        return false;
    }

    // DPI-aware dimensions
    int w = region.width;
    int h = region.height;
    if (w <= 0 || h <= 0) {
        ReleaseDC(hWnd, hWndDC);
        m_error = "Invalid region dimensions";
        return false;
    }

    HDC hMemDC = CreateCompatibleDC(hWndDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hWndDC, w, h);
    SelectObject(hMemDC, hBitmap);

    BitBlt(hMemDC, 0, 0, w, h, hWndDC, region.x, region.y, SRCCOPY);

    // Extract pixel data (BGRA)
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;  // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w * h * 4));
    out.timestamp = GetTickCount64();

    GetDIBits(hMemDC, hBitmap, 0, static_cast<UINT>(h),
              out.pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(hWnd, hWndDC);
    return true;
#else
    (void)hwnd; (void)region; (void)out;
    return false;
#endif
}

} // namespace live
