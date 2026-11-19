// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <cstdint>
#include <vector>
#include <string>

#ifdef _WIN32
// Forward declarations for DXGI Desktop Duplication
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutput1;
struct IDXGIOutputDuplication;
#endif

namespace live {

struct CapturedFrame {
    std::vector<uint8_t> pixels;  // BGRA format
    int width = 0;
    int height = 0;
    uint64_t timestamp = 0;
};

struct CaptureRegion {
    int x = 0, y = 0, width = 0, height = 0;
};

enum class CaptureMethod {
    Auto,
    DXGI,
    GDI,
};

// Screen capture interface.
// Region coordinates are virtual-desktop (screen) coordinates when windowHandle is null,
// otherwise client coordinates of that window. With a window handle the window's own content is read
// (PrintWindow + PW_RENDERFULLCONTENT): it works while other windows cover it and for DirectX / video windows
// such as PS Remote Play or a capture card preview (console mode). Falls back to a GDI BitBlt of the window DC.
class ScreenCapture {
public:
    ~ScreenCapture() { shutdown(); }

    bool init(CaptureMethod method = CaptureMethod::Auto);
    void shutdown();

    bool captureRegion(void* windowHandle, CaptureRegion region, CapturedFrame& out);
    bool captureFullWindow(void* windowHandle, CapturedFrame& out);

    // What the caller asked for (Auto/DXGI/GDI) vs. what is actually running (DXGI/GDI).
    CaptureMethod requestedMethod() const { return m_requested; }
    CaptureMethod activeMethod() const { return m_method; }
    const std::string& lastError() const { return m_error; }

private:
    bool initDXGI();
    bool createDuplication();
    bool captureDXGI(CaptureRegion region, CapturedFrame& out);
    bool captureGDI(void* hwnd, CaptureRegion region, CapturedFrame& out);
    bool captureWindow(void* hwnd, CaptureRegion region, CapturedFrame& out);
    void shutdownDXGI();
    void releaseDuplication();

    CaptureMethod m_requested = CaptureMethod::Auto;
    CaptureMethod m_method = CaptureMethod::Auto;
    bool m_initialized = false;
    std::string m_error;

#ifdef _WIN32
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutput1* m_output = nullptr;
    IDXGIOutputDuplication* m_dxgiDup = nullptr;

    // Last full desktop image kept on the GPU: AcquireNextFrame only reports *changes*,
    // so a static scene would otherwise have no image to read.
    ID3D11Texture2D* m_lastDesktop = nullptr;
    bool m_hasLastFrame = false;

    // Reused CPU-readable copy of the requested region.
    ID3D11Texture2D* m_staging = nullptr;
    int m_stagingW = 0, m_stagingH = 0;

    // Primary output bounds in virtual-desktop coordinates.
    int m_outLeft = 0, m_outTop = 0, m_outRight = 0, m_outBottom = 0;

    uint64_t m_nextDupRetryMs = 0;
#endif
};

} // namespace live
