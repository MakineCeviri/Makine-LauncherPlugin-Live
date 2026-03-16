#pragma once

#include <cstdint>
#include <vector>
#include <string>

#ifdef _WIN32
// Forward declarations for DXGI Desktop Duplication
struct ID3D11Device;
struct ID3D11DeviceContext;
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

// Screen capture interface
class ScreenCapture {
public:
    bool init(CaptureMethod method = CaptureMethod::Auto);
    void shutdown();

    bool captureRegion(void* windowHandle, CaptureRegion region, CapturedFrame& out);
    bool captureFullWindow(void* windowHandle, CapturedFrame& out);

    CaptureMethod activeMethod() const { return m_method; }
    const std::string& lastError() const { return m_error; }

private:
    bool initDXGI();
    bool initGDI();
    bool captureDXGI(void* hwnd, CaptureRegion region, CapturedFrame& out);
    bool captureGDI(void* hwnd, CaptureRegion region, CapturedFrame& out);
    void shutdownDXGI();

    CaptureMethod m_method = CaptureMethod::Auto;
    bool m_initialized = false;
    std::string m_error;

#ifdef _WIN32
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutputDuplication* m_dxgiDup = nullptr;
#endif
};

} // namespace live
