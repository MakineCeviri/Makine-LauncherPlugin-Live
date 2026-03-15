#include "capture.h"

#ifdef _WIN32
#include <windows.h>
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
    RECT rect;
    if (!GetClientRect(static_cast<HWND>(windowHandle), &rect))
        return false;
    return captureRegion(windowHandle, {0, 0, rect.right, rect.bottom}, out);
#else
    (void)windowHandle; (void)out;
    return false;
#endif
}

// ── DXGI Desktop Duplication ──

bool ScreenCapture::initDXGI()
{
    // TODO: Initialize DXGI Desktop Duplication
    // Requires: IDXGIOutputDuplication, ID3D11Device
    // For now, fall through to GDI
    return false;
}

bool ScreenCapture::captureDXGI(void* /*hwnd*/, CaptureRegion /*region*/, CapturedFrame& /*out*/)
{
    // TODO: Implement DXGI capture
    return false;
}

// ── GDI BitBlt ──

bool ScreenCapture::initGDI()
{
    // GDI always available on Windows
    return true;
}

bool ScreenCapture::captureGDI(void* hwnd, CaptureRegion region, CapturedFrame& out)
{
#ifdef _WIN32
    HWND hWnd = static_cast<HWND>(hwnd);
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
