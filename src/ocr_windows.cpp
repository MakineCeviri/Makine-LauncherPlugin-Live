#include "ocr_windows.h"

#include <fstream>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace live {

// PowerShell script for Windows OCR — embedded as string
// Python OCR script using winocr (Windows OCR via WinRT Python bindings)
static const char* kOcrScript = R"py(
import sys, asyncio
from winocr import recognize_pil
from PIL import Image

async def main():
    img = Image.open(sys.argv[1])
    lang = sys.argv[2] if len(sys.argv) > 2 else 'tr'
    result = await recognize_pil(img, lang)
    for line in result.lines:
        print(line.text)

asyncio.run(main())
)py";

bool WindowsOcrEngine::init(const std::string& dataPath)
{
    m_dataPath = dataPath;

    // Write the Python OCR script to data directory
    fs::create_directories(dataPath);
    m_scriptPath = dataPath + "/ocr_helper.py";

    std::ofstream ofs(m_scriptPath);
    if (!ofs) {
        m_error = "Cannot write OCR helper script";
        return false;
    }
    ofs << kOcrScript;
    ofs.close();

    m_ready = true;
    return true;
}

void WindowsOcrEngine::shutdown()
{
    m_ready = false;
}

std::vector<OcrBox> WindowsOcrEngine::recognize(
    const uint8_t* pixels, int width, int height)
{
    if (!m_ready) return {};

    // Save pixels as BMP to temp file
    std::string bmpPath = m_dataPath + "/capture.bmp";
    if (!saveBmp(pixels, width, height, bmpPath)) {
        m_error = "Failed to save capture as BMP";
        return {};
    }

    // Run PowerShell OCR
    std::string text = runOcrScript(bmpPath);

    if (text.empty())
        return {};

    // Parse result — one OcrBox per line
    std::vector<OcrBox> results;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();

        std::string line = text.substr(pos, end - pos);
        // Remove \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!line.empty()) {
            OcrBox box;
            box.text = std::move(line);
            box.confidence = 0.9f; // Windows OCR doesn't expose confidence
            box.x = 0; box.y = 0;
            box.width = width; box.height = 0;
            results.push_back(std::move(box));
        }
        pos = end + 1;
    }

    return results;
}

bool WindowsOcrEngine::saveBmp(const uint8_t* pixels, int w, int h, const std::string& path)
{
    // BGRA pixels → BMP file (uncompressed, 32-bit)
    const int rowSize = w * 4;
    const int imageSize = rowSize * h;
    const int fileSize = 54 + imageSize;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    // BMP Header (14 bytes)
    uint8_t bmpHeader[14] = {};
    bmpHeader[0] = 'B'; bmpHeader[1] = 'M';
    std::memcpy(bmpHeader + 2, &fileSize, 4);
    int dataOffset = 54;
    std::memcpy(bmpHeader + 10, &dataOffset, 4);
    ofs.write(reinterpret_cast<char*>(bmpHeader), 14);

    // DIB Header (40 bytes)
    uint8_t dibHeader[40] = {};
    int dibSize = 40;
    std::memcpy(dibHeader, &dibSize, 4);
    std::memcpy(dibHeader + 4, &w, 4);
    // Negative height = top-down (our pixel data is already top-down)
    int negH = -h;
    std::memcpy(dibHeader + 8, &negH, 4);
    uint16_t planes = 1;
    std::memcpy(dibHeader + 12, &planes, 2);
    uint16_t bpp = 32;
    std::memcpy(dibHeader + 14, &bpp, 2);
    std::memcpy(dibHeader + 20, &imageSize, 4);
    ofs.write(reinterpret_cast<char*>(dibHeader), 40);

    // Pixel data (BGRA, already in correct BMP order)
    ofs.write(reinterpret_cast<const char*>(pixels), imageSize);
    return ofs.good();
}

std::string WindowsOcrEngine::runOcrScript(const std::string& imagePath)
{
#ifdef _WIN32
    // Normalize paths to backslash for Windows
    auto toWinPath = [](std::string p) {
        for (auto& c : p) if (c == '/') c = '\\';
        return p;
    };

    std::string cmd = "cmd.exe /c python -X utf8 \"" +
                      toWinPath(m_scriptPath) + "\" \"" +
                      toWinPath(imagePath) + "\" tr";

    // Create process and capture stdout
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return "";

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        m_error = "Failed to start PowerShell";
        return "";
    }

    CloseHandle(hWritePipe); // Close write end in parent

    // Read stdout
    std::string output;
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        output += buf;
    }

    CloseHandle(hReadPipe);

    // Wait for process (max 10 seconds)
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        m_error = "OCR script failed (exit code " + std::to_string(exitCode) + ")";
        return "";
    }

    return output;
#else
    (void)imagePath;
    return "";
#endif
}

} // namespace live
