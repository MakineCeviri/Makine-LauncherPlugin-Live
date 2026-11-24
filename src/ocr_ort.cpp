// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// The arithmetic below is the benchmark's (tools/ocr_bench/src/ort_ocr.cpp, "ORT: det v3 + rec ch v3")
// line for line: tests/cpp/ocr_gate.cpp checks that this file gives the bench's text on every frame.
// Parameters mirror the 0.3.x RapidOcrOnnx call (padding 50, maxSideLen 1024, boxScore 0.6, box 0.3,
// unclip 2.0); the angle classifier is not used (subtitles are horizontal).

#include "ocr_ort.h"

#include "winutil.h"

#include <windows.h>

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace live {

namespace {

constexpr int kPadding = 50;
constexpr int kMaxSideLen = 1024;
constexpr float kBoxScoreThresh = 0.6f;
constexpr float kBoxThresh = 0.3f;
constexpr float kUnclipRatio = 2.0f;
constexpr int kRecMinWidth = 320;   // PaddleOCR rec_image_shape [3,48,320]: short lines are padded to 320

struct Rect {
    float x0, y0, x1, y1;
};

struct View {   // a BGRA frame in memory, top-down, tightly packed
    const uint8_t* bgra;
    int w, h;
};

// Bilinear sample of a BGRA rect into a normalised CHW float tensor (B, G, R as PaddleOCR feeds cv2
// images). Pixels outside the image read as `outside` (the detector's padding is white).
void resizeNormalize(const View& img, const Rect& src, int dw, int dh, int tensorW, float* out, const float mean[3],
                     const float scale[3], float outside = -1.f, int padX0 = 0, int padY0 = 0)
{
    const size_t plane = static_cast<size_t>(dh) * tensorW;
    const float sx = (src.x1 - src.x0) / dw, sy = (src.y1 - src.y0) / dh;
    auto px = [&](int x, int y, int c) -> float {
        x -= padX0;
        y -= padY0;
        if (x < 0 || y < 0 || x >= img.w || y >= img.h) return outside;
        return img.bgra[(static_cast<size_t>(y) * img.w + x) * 4 + c];
    };
    for (int y = 0; y < dh; ++y) {
        float fy = src.y0 + (y + 0.5f) * sy - 0.5f;
        const float lo = src.y0, hi = src.y1 - 1;
        fy = std::clamp(fy, lo, std::max(lo, hi));
        const int y0 = static_cast<int>(std::floor(fy));
        const int y1 = std::min(y0 + 1, static_cast<int>(std::max(lo, hi)));
        const float wy = fy - y0;
        for (int x = 0; x < dw; ++x) {
            float fx = src.x0 + (x + 0.5f) * sx - 0.5f;
            const float lx = src.x0, hx = src.x1 - 1;
            fx = std::clamp(fx, lx, std::max(lx, hx));
            const int x0 = static_cast<int>(std::floor(fx));
            const int x1 = std::min(x0 + 1, static_cast<int>(std::max(lx, hx)));
            const float wx = fx - x0;
            for (int c = 0; c < 3; ++c) {
                const float v = (px(x0, y0, c) * (1 - wx) + px(x1, y0, c) * wx) * (1 - wy)
                                + (px(x0, y1, c) * (1 - wx) + px(x1, y1, c) * wx) * wy;
                out[c * plane + static_cast<size_t>(y) * tensorW + x] = (v - mean[c]) * scale[c];
            }
        }
    }
}

} // namespace

struct OcrOrt::Impl {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtMemoryInfo* mem = nullptr;
    OrtAllocator* alloc = nullptr;
    std::string version, loadedFrom;

    struct Net {
        OrtSession* session = nullptr;
        std::string inName, outName;
        std::vector<int64_t> inShape;
    };
    Net det, rec;
    int recH = 48;
    std::vector<std::string> keys;   // index 0 = CTC blank
    OcrOrtConfig cfg;

    ~Impl()
    {
        if (!api) return;
        for (Net* n : {&det, &rec})
            if (n->session) api->ReleaseSession(n->session);
        if (mem) api->ReleaseMemoryInfo(mem);
        if (env) api->ReleaseEnv(env);
        // No FreeLibrary: ONNX Runtime keeps per-process state, and the host ends right after shutdown.
    }

    void check(OrtStatus* s)
    {
        if (!s) return;
        std::string m = api->GetErrorMessage(s);
        api->ReleaseStatus(s);
        throw std::runtime_error(m);
    }

    void open(Net& n, const std::wstring& path)
    {
        OrtSessionOptions* so = nullptr;
        check(api->CreateSessionOptions(&so));
        check(api->SetIntraOpNumThreads(so, cfg.threads));
        check(api->SetInterOpNumThreads(so, 1));
        check(api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        check(api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
        // K8: no spinning — idle ORT threads must not burn the game's cores between frames
        check(api->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0"));
        check(api->CreateSession(env, path.c_str(), so, &n.session));
        api->ReleaseSessionOptions(so);

        char* name = nullptr;
        check(api->SessionGetInputName(n.session, 0, alloc, &name));
        n.inName = name;
        check(api->AllocatorFree(alloc, name));
        check(api->SessionGetOutputName(n.session, 0, alloc, &name));
        n.outName = name;
        check(api->AllocatorFree(alloc, name));

        OrtTypeInfo* ti = nullptr;
        check(api->SessionGetInputTypeInfo(n.session, 0, &ti));
        const OrtTensorTypeAndShapeInfo* tsi = nullptr;
        check(api->CastTypeInfoToTensorInfo(ti, &tsi));
        size_t dims = 0;
        check(api->GetDimensionsCount(tsi, &dims));
        n.inShape.resize(dims);
        check(api->GetDimensions(tsi, n.inShape.data(), dims));
        api->ReleaseTypeInfo(ti);
    }

    std::vector<float> infer(Net& n, std::vector<float>& input, const std::vector<int64_t>& shape,
                             std::vector<int64_t>& outShape)
    {
        OrtValue* in = nullptr;
        check(api->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape.data(),
                                                  shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
        OrtValue* out = nullptr;
        const char* inName = n.inName.c_str();
        const char* outName = n.outName.c_str();
        OrtStatus* st = api->Run(n.session, nullptr, &inName, &in, 1, &outName, 1, &out);
        api->ReleaseValue(in);
        check(st);
        OrtTensorTypeAndShapeInfo* info = nullptr;
        check(api->GetTensorTypeAndShape(out, &info));
        size_t dims = 0, count = 0;
        check(api->GetDimensionsCount(info, &dims));
        outShape.resize(dims);
        check(api->GetDimensions(info, outShape.data(), dims));
        check(api->GetTensorShapeElementCount(info, &count));
        api->ReleaseTensorTypeAndShapeInfo(info);
        float* data = nullptr;
        check(api->GetTensorMutableData(out, reinterpret_cast<void**>(&data)));
        std::vector<float> result(data, data + count);
        api->ReleaseValue(out);
        return result;
    }

    void loadKeys()
    {
        std::vector<std::string> chars;
        OrtModelMetadata* meta = nullptr;
        check(api->SessionGetModelMetadata(rec.session, &meta));
        char* value = nullptr;
        check(api->ModelMetadataLookupCustomMetadataMap(meta, alloc, "character", &value));
        if (value) {
            std::istringstream ss(value);
            for (std::string line; std::getline(ss, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                chars.push_back(line);
            }
            check(api->AllocatorFree(alloc, value));
        }
        api->ReleaseModelMetadata(meta);
        if (chars.empty()) {
            const auto text = win::readFile(cfg.dictFile);
            if (!text) throw std::runtime_error("OCR sözlüğü bulunamadı (ppocr_keys_v1.txt)");
            std::istringstream ss(*text);
            for (std::string line; std::getline(ss, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                chars.push_back(line);
            }
        }
        keys.clear();
        keys.push_back("");   // blank
        keys.insert(keys.end(), chars.begin(), chars.end());
    }

    // ---- detector (DB) ----------------------------------------------------------------------------
    std::vector<Rect> detect(const View& img)
    {
        const int pw = img.w + 2 * kPadding, ph = img.h + 2 * kPadding;
        const int target = std::min(kMaxSideLen, std::max(img.w, img.h)) + 2 * kPadding;
        int dw, dh;
        if (pw > ph) {
            dw = target;
            dh = static_cast<int>(ph * (float(target) / pw));
        } else {
            dh = target;
            dw = static_cast<int>(pw * (float(target) / ph));
        }
        // RapidOcrOnnx getScaleParam rounding (down one extra step), kept for parity
        if (dw % 32) dw = std::max((dw / 32 - 1) * 32, 32);
        if (dh % 32) dh = std::max((dh / 32 - 1) * 32, 32);
        const float rw = float(dw) / pw, rh = float(dh) / ph;

        static const float mean[3] = {0.485f * 255, 0.456f * 255, 0.406f * 255};
        static const float scale[3] = {1 / (0.229f * 255), 1 / (0.224f * 255), 1 / (0.225f * 255)};
        std::vector<float> input(static_cast<size_t>(3) * dw * dh);
        resizeNormalize(img, Rect{0, 0, float(pw), float(ph)}, dw, dh, dw, input.data(), mean, scale, 255.f, kPadding,
                        kPadding);
        std::vector<int64_t> outShape;
        std::vector<float> prob = infer(det, input, {1, 3, dh, dw}, outShape);

        // threshold, then 8-connected components → axis-aligned boxes. No dilation: a 1-px grown box drops
        // the mean score of thin (vertically squashed) subtitle lines under 0.6.
        std::vector<uint8_t> on(static_cast<size_t>(dw) * dh, 0);
        for (size_t i = 0; i < on.size(); ++i) on[i] = prob[i] > kBoxThresh;
        std::vector<int> label(on.size(), 0);
        std::vector<int> stack;
        std::vector<Rect> boxes;
        int next = 0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const size_t i = static_cast<size_t>(y) * dw + x;
                if (!on[i] || label[i]) continue;
                ++next;
                int x0 = x, x1 = x, y0 = y, y1 = y, n = 0;
                stack.assign(1, static_cast<int>(i));
                label[i] = next;
                while (!stack.empty()) {
                    const int p = stack.back();
                    stack.pop_back();
                    const int qx0 = p % dw, qy0 = p / dw;
                    x0 = std::min(x0, qx0);
                    x1 = std::max(x1, qx0);
                    y0 = std::min(y0, qy0);
                    y1 = std::max(y1, qy0);
                    ++n;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int qx = qx0 + dx, qy = qy0 + dy;
                            if (qx < 0 || qy < 0 || qx >= dw || qy >= dh) continue;
                            const size_t q = static_cast<size_t>(qy) * dw + qx;
                            if (on[q] && !label[q]) {
                                label[q] = next;
                                stack.push_back(static_cast<int>(q));
                            }
                        }
                }
                if (n <= 2) continue;
                const float w = float(x1 - x0), h = float(y1 - y0);
                if (std::max(w, h) < 3) continue;
                double sum = 0;
                for (int yy = y0; yy <= y1; ++yy)
                    for (int xx = x0; xx <= x1; ++xx) sum += prob[static_cast<size_t>(yy) * dw + xx];
                if (sum / ((x1 - x0 + 1) * (y1 - y0 + 1)) < kBoxScoreThresh) continue;
                const float dist = (w * h) * kUnclipRatio / (2 * (w + h));
                Rect r{x0 - dist, y0 - dist, x1 + dist, y1 + dist};
                if (std::min(r.x1 - r.x0, r.y1 - r.y0) < 5) continue;
                // map → padded image → original image
                r.x0 = std::clamp(r.x0 / rw, 0.f, float(pw)) - kPadding;
                r.x1 = std::clamp(r.x1 / rw, 0.f, float(pw)) - kPadding;
                r.y0 = std::clamp(r.y0 / rh, 0.f, float(ph)) - kPadding;
                r.y1 = std::clamp(r.y1 / rh, 0.f, float(ph)) - kPadding;
                r.x0 = std::max(0.f, std::floor(r.x0));
                r.y0 = std::max(0.f, std::floor(r.y0));
                r.x1 = std::min(float(img.w), std::ceil(r.x1));
                r.y1 = std::min(float(img.h), std::ceil(r.y1));
                if (r.x1 - r.x0 < 2 || r.y1 - r.y0 < 2) continue;
                boxes.push_back(r);
            }
        return boxes;
    }

    // ---- recognizer (CTC) -------------------------------------------------------------------------
    std::string recognize(const View& img, const Rect& r)
    {
        static const float mean[3] = {127.5f, 127.5f, 127.5f};
        static const float scale[3] = {1 / 127.5f, 1 / 127.5f, 1 / 127.5f};
        const float ratio = (r.x1 - r.x0) / (r.y1 - r.y0);
        const int rw = std::max(1, static_cast<int>(std::ceil(recH * ratio)));
        const int tw = std::max(kRecMinWidth, rw);
        std::vector<float> input(static_cast<size_t>(3) * tw * recH, 0.f);
        resizeNormalize(img, r, rw, recH, tw, input.data(), mean, scale);
        std::vector<int64_t> os;
        std::vector<float> p = infer(rec, input, {1, 3, recH, tw}, os);
        if (os.size() != 3) throw std::runtime_error("unexpected rec output rank");
        const int64_t T = os[1], C = os[2];
        if (static_cast<int64_t>(keys.size()) + 1 == C) keys.push_back(" ");   // use_space_char
        else if (static_cast<int64_t>(keys.size()) != C)
            throw std::runtime_error("dictionary size " + std::to_string(keys.size()) + " != classes " + std::to_string(C));
        std::string text;
        int64_t last = 0;
        for (int64_t i = 0; i < T; ++i) {
            const float* row = &p[static_cast<size_t>(i * C)];
            const int64_t k = std::max_element(row, row + C) - row;
            if (k != 0 && k != last) text += keys[static_cast<size_t>(k)];
            last = k;
        }
        return text;
    }

    std::vector<OcrLine> run(const View& img)
    {
        std::vector<Rect> boxes = detect(img);
        // Line grouping (OCR_MIMARI G1/K4a): boxes whose vertical centre falls inside a line's first box
        // belong to that line and are read left to right.
        std::vector<std::vector<Rect>> rows;
        std::sort(boxes.begin(), boxes.end(), [](const Rect& a, const Rect& b) { return a.y0 + a.y1 < b.y0 + b.y1; });
        for (const Rect& r : boxes) {
            const float cy = (r.y0 + r.y1) * 0.5f;
            if (!rows.empty() && cy >= rows.back()[0].y0 && cy <= rows.back()[0].y1) rows.back().push_back(r);
            else rows.push_back({r});
        }
        std::vector<OcrLine> lines;
        for (auto& row : rows) {
            std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.x0 < b.x0; });
            OcrLine line;
            Rect box{1e9f, 1e9f, -1e9f, -1e9f};
            for (const Rect& r : row) {
                const std::string s = recognize(img, r);
                if (s.empty()) continue;
                line.text += (line.text.empty() ? "" : " ") + s;
                box = {std::min(box.x0, r.x0), std::min(box.y0, r.y0), std::max(box.x1, r.x1), std::max(box.y1, r.y1)};
            }
            if (line.text.empty()) continue;
            line.x0 = static_cast<int>(box.x0);
            line.y0 = static_cast<int>(box.y0);
            line.x1 = static_cast<int>(box.x1);
            line.y1 = static_cast<int>(box.y1);
            lines.push_back(std::move(line));
        }
        return lines;
    }
};

OcrOrt::OcrOrt() : d(std::make_unique<Impl>()) {}

OcrOrt::~OcrOrt() = default;

bool OcrOrt::load(const OcrOrtConfig& cfg, std::string& error)
{
    d->cfg = cfg;
    // G-ORT: the plugin's own Microsoft-signed copy by full path; its imports (the VC++ runtime shipped
    // next to it) only from that folder and System32 — never PATH, the current directory or an older
    // onnxruntime.dll in System32.
    wchar_t full[MAX_PATH * 2] = {};   // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs an absolute path
    GetFullPathNameW(cfg.ortDll.c_str(), MAX_PATH * 2, full, nullptr);
    d->dll = LoadLibraryExW(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d->dll) {
        error = "OCR çalışma dosyası yüklenemedi (onnxruntime.dll, hata " + std::to_string(GetLastError())
                + "). Makine'yi onarın ya da yeniden kurun.";
        return false;
    }
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(d->dll, path, MAX_PATH * 2);
    d->loadedFrom = win::wideToUtf8(path);
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto base = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(d->dll, "OrtGetApiBase")));
    if (!base || !(d->api = base()->GetApi(ORT_API_VERSION))) {
        error = "onnxruntime.dll beklenen sürüm değil. Makine'yi onarın ya da yeniden kurun.";
        return false;
    }
    d->version = base()->GetVersionString();
    try {
        d->check(d->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "makine-live", &d->env));
        d->check(d->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d->mem));
        d->check(d->api->GetAllocatorWithDefaultOptions(&d->alloc));
        d->open(d->det, cfg.detModel);
        d->open(d->rec, cfg.recModel);
        if (d->rec.inShape.size() == 4 && d->rec.inShape[2] > 0) d->recH = static_cast<int>(d->rec.inShape[2]);
        d->loadKeys();
        // Warm-up: the first inference of each model allocates and plans; the first subtitle must not wait for it
        std::vector<uint8_t> blank(static_cast<size_t>(320) * 64 * 4, 255);
        const View warm{blank.data(), 320, 64};
        d->detect(warm);
        d->recognize(warm, Rect{0, 0, 320, 48});
    } catch (const std::exception& e) {
        error = std::string("OCR başlatılamadı: ") + e.what();
        return false;
    }
    return true;
}

std::vector<OcrLine> OcrOrt::run(const uint8_t* bgra, int width, int height, std::string& error)
{
    if (!d->det.session || !d->rec.session || !bgra || width <= 0 || height <= 0) return {};
    try {
        return d->run(View{bgra, width, height});
    } catch (const std::exception& e) {
        error = std::string("OCR: ") + e.what();
        return {};
    }
}

int OcrOrt::threads() const { return d->cfg.threads; }

std::string OcrOrt::describe() const
{
    return "onnxruntime " + d->version + " (" + d->loadedFrom + "), " + std::to_string(d->cfg.threads) + " iş parçacığı, "
           + std::to_string(d->keys.size()) + " sınıf";
}

} // namespace live
