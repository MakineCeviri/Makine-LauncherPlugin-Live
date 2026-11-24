// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "common.h"

#include <windows.h>
#include <wincodec.h>

#include <string>

namespace bench {

double nowMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// PNG / JPEG / BMP via the Windows Imaging Component (no third-party image library).
bool loadImage(const std::wstring& path, Image& out, std::string& err)
{
    static IWICImagingFactory* factory = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    if (!factory) { err = "WIC unavailable"; return false; }
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv))
        && SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom))
        && SUCCEEDED(conv->GetSize(&w, &h))) {
        out.w = static_cast<int>(w);
        out.h = static_cast<int>(h);
        out.bgra.resize(static_cast<size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (!ok) err = "cannot decode " + narrow(path);
    return ok;
}

} // namespace bench
