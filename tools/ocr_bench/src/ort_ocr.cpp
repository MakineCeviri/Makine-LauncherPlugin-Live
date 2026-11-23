// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "ort_ocr.h"

#include <windows.h>

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {

namespace {

constexpr int kPadding = 50;          // ocr_rapid.cpp: padding
constexpr int kMaxSideLen = 1024;     // maxSideLen
constexpr float kBoxScoreThresh = 0.6f;
constexpr float kBoxThresh = 0.3f;
constexpr float kUnclipRatio = 2.0f;
constexpr int kRecMinWidth = 320;     // PaddleOCR rec_image_shape [3,48,320]: short lines are padded to 320
constexpr int kClsW = 192, kClsH = 48;

struct Rect { float x0, y0, x1, y1; };

// Bilinear sample of a BGRA rect into a normalised CHW float tensor (channel order B,G,R as PaddleOCR feeds
// cv2 images). Pixels outside the source image read as `outside` (det padding is white).
void resizeNormalize(const Image& img, const Rect& src, int dw, int dh, int tensorW, float* out,
                     const float mean[3], const float scale[3], float outside = -1.f, int padX0 = 0, int padY0 = 0)
{
    const size_t plane = static_cast<size_t>(dh) * tensorW;
    const float sx = (src.x1 - src.x0) / dw, sy = (src.y1 - src.y0) / dh;
    auto px = [&](int x, int y, int c) -> float {
        x -= padX0; y -= padY0;
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

struct OrtOcr::Impl {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtMemoryInfo* mem = nullptr;
    OrtAllocator* alloc = nullptr;
    std::string version;

    struct Net {
        OrtSession* session = nullptr;
        std::string inName, outName;
        std::vector<int64_t> inShape;
    };
    Net det, cls, rec;
    int recH = 48;
    std::vector<std::string> keys;  // index 0 = CTC blank
    std::string keysSource;
    OrtConfig cfg;

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
        check(api->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", cfg.spin ? "1" : "0"));
        if (cfg.dml) {
            // dml_provider_factory.h: DirectML needs memory patterns off and sequential execution.
            check(api->DisableMemPattern(so));
            using AppendDml = OrtStatus* (ORT_API_CALL*)(OrtSessionOptions*, int);
            auto fn = reinterpret_cast<AppendDml>(reinterpret_cast<void*>(
                GetProcAddress(dll, "OrtSessionOptionsAppendExecutionProvider_DML")));
            if (!fn) throw std::runtime_error("onnxruntime.dll has no DirectML provider");
            check(fn(so, 0));
        }
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

    // Runs one NCHW float input; returns the output tensor data + shape.
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
            keysSource = "model metadata";
        }
        api->ReleaseModelMetadata(meta);
        if (chars.empty()) {
            std::ifstream f(std::filesystem::path(cfg.dictFile), std::ios::binary);
            if (!f) throw std::runtime_error("dictionary not found: " + narrow(cfg.dictFile));
            for (std::string line; std::getline(f, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                chars.push_back(line);
            }
            keysSource = narrow(cfg.dictFile.substr(cfg.dictFile.find_last_of(L"\\/") + 1));
        }
        keys.clear();
        keys.push_back("");  // blank
        keys.insert(keys.end(), chars.begin(), chars.end());
    }

    // ---- detector (DB) -------------------------------------------------------------------------------------
    std::vector<Rect> detect(const Image& img, Times& t)
    {
        double t0 = nowMs();
        const int pw = img.w + 2 * kPadding, ph = img.h + 2 * kPadding;
        int target = std::min(kMaxSideLen, std::max(img.w, img.h)) + 2 * kPadding;
        int dw, dh;
        if (pw > ph) { dw = target; dh = static_cast<int>(ph * (float(target) / pw)); }
        else { dh = target; dw = static_cast<int>(pw * (float(target) / ph)); }
        // RapidOcrOnnx getScaleParam rounding (down one extra step), kept for parity
        if (dw % 32) dw = std::max((dw / 32 - 1) * 32, 32);
        if (dh % 32) dh = std::max((dh / 32 - 1) * 32, 32);
        const float rw = float(dw) / pw, rh = float(dh) / ph;

        static const float mean[3] = {0.485f * 255, 0.456f * 255, 0.406f * 255};
        static const float scale[3] = {1 / (0.229f * 255), 1 / (0.224f * 255), 1 / (0.225f * 255)};
        std::vector<float> input(static_cast<size_t>(3) * dw * dh);
        resizeNormalize(img, Rect{0, 0, float(pw), float(ph)}, dw, dh, dw, input.data(), mean, scale, 255.f,
                        kPadding, kPadding);
        double t1 = nowMs();
        t.pre += t1 - t0;

        std::vector<int64_t> outShape;
        std::vector<float> prob = infer(det, input, {1, 3, dh, dw}, outShape);

        // threshold, then 8-connected components → axis-aligned boxes. No dilation: a 1-px grown box drops the
        // mean score of thin (vertically squashed) subtitle lines under 0.6, which the DLL does not do.
        std::vector<uint8_t> dil(static_cast<size_t>(dw) * dh, 0);
        for (size_t i = 0; i < dil.size(); ++i) dil[i] = prob[i] > kBoxThresh;
        std::vector<int> label(dil.size(), 0);
        std::vector<int> stack;
        std::vector<Rect> boxes;
        int next = 0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                size_t i = static_cast<size_t>(y) * dw + x;
                if (!dil[i] || label[i]) continue;
                ++next;
                int x0 = x, x1 = x, y0 = y, y1 = y, n = 0;
                stack.assign(1, static_cast<int>(i));
                label[i] = next;
                while (!stack.empty()) {
                    int p = stack.back();
                    stack.pop_back();
                    int px = p % dw, py = p / dw;
                    x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
                    ++n;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int qx = px + dx, qy = py + dy;
                            if (qx < 0 || qy < 0 || qx >= dw || qy >= dh) continue;
                            size_t q = static_cast<size_t>(qy) * dw + qx;
                            if (dil[q] && !label[q]) { label[q] = next; stack.push_back(static_cast<int>(q)); }
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
                r.x0 = std::max(0.f, std::floor(r.x0)); r.y0 = std::max(0.f, std::floor(r.y0));
                r.x1 = std::min(float(img.w), std::ceil(r.x1)); r.y1 = std::min(float(img.h), std::ceil(r.y1));
                if (r.x1 - r.x0 < 2 || r.y1 - r.y0 < 2) continue;
                boxes.push_back(r);
            }
        t.det += nowMs() - t1;
        return boxes;
    }

    // ---- detector-free: text mask + horizontal projection (OCR_MIMARI K3/K4) -------------------------------
    std::vector<Rect> splitLines(const Image& img, Times& t)
    {
        double t0 = nowMs();
        const int w = img.w, h = img.h;
        const int r = std::max(2, h / 60);  // outline search radius, scales with the strip
        std::vector<uint8_t> luma(static_cast<size_t>(w) * h), bright(luma.size());
        for (size_t i = 0; i < luma.size(); ++i) {
            const uint8_t* p = &img.bgra[i * 4];
            luma[i] = static_cast<uint8_t>((p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000);
            bright[i] = p[2] >= 180 && p[1] >= 150;  // white or yellow fill
        }
        // separable min filter of luma: is there a dark (outline / shadow) pixel within r?
        std::vector<uint8_t> tmp(luma.size()), mn(luma.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t m = 255;
                for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k)
                    m = std::min(m, luma[static_cast<size_t>(y) * w + k]);
                tmp[static_cast<size_t>(y) * w + x] = m;
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t m = 255;
                for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k)
                    m = std::min(m, tmp[static_cast<size_t>(k) * w + x]);
                mn[static_cast<size_t>(y) * w + x] = m;
            }
        std::vector<uint8_t> mask(luma.size());
        std::vector<int> rows(h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = static_cast<size_t>(y) * w + x;
                mask[i] = bright[i] && mn[i] <= 70;
                rows[y] += mask[i];
            }
        // smooth, threshold relative to the background floor (median) and the strongest row
        std::vector<int> sm(h, 0);
        for (int y = 0; y < h; ++y)
            for (int k = std::max(0, y - 1); k <= std::min(h - 1, y + 1); ++k) sm[y] += rows[k];
        std::vector<int> sorted = sm;
        std::nth_element(sorted.begin(), sorted.begin() + h / 2, sorted.end());
        const int floor = sorted[h / 2];
        const int peak = *std::max_element(sm.begin(), sm.end());
        std::vector<Rect> out;
        if (peak < 12) { t.split += nowMs() - t0; return out; }
        // 1/12 of the peak keeps a short second line ("a nuclear winter.") that has a third of the first's mass
        const int thr = floor + std::max(6, (peak - floor) / 12);

        struct Run { int y0, y1; };
        std::vector<Run> found, runs;
        for (int y = 0; y < h; ++y) {
            if (sm[y] < thr) continue;
            if (!found.empty() && y - found.back().y1 <= 2) found.back().y1 = y;
            else found.push_back({y, y});
        }
        // two lines touching through descenders/ascenders: split at a deep valley in the middle of the run
        while (!found.empty()) {
            Run rn = found.front();
            found.erase(found.begin());
            const int rh = rn.y1 - rn.y0 + 1;
            if (rh >= 12) {
                const int lo = rn.y0 + rh / 5, hi = rn.y1 - rh / 5;
                const int ym = static_cast<int>(std::min_element(sm.begin() + lo, sm.begin() + hi + 1) - sm.begin());
                const int top = *std::max_element(sm.begin() + rn.y0, sm.begin() + rn.y1 + 1);
                if (sm[ym] * 4 < top) {
                    found.insert(found.begin(), {Run{rn.y0, ym - 1}, Run{ym + 1, rn.y1}});
                    continue;
                }
            }
            runs.push_back(rn);
        }
        int maxH = 0;
        for (auto& rn : runs) maxH = std::max(maxH, rn.y1 - rn.y0 + 1);
        for (auto& rn : runs) {
            const int lh = rn.y1 - rn.y0 + 1;
            if (lh < std::max(6, maxH * 2 / 5)) continue;
            // column clusters inside the run; word gaps are under ~1 line height, noise sits further out
            std::vector<int> cols(w, 0);
            for (int y = rn.y0; y <= rn.y1; ++y)
                for (int x = 0; x < w; ++x) cols[x] += mask[static_cast<size_t>(y) * w + x];
            int best0 = -1, best1 = -1, bestSum = 0, c0 = -1, c1 = -1, sum = 0, gap = 0;
            const int maxGap = lh * 3 / 2;
            for (int x = 0; x <= w; ++x) {
                const bool on = x < w && cols[x] > 0;
                if (on) {
                    if (c0 < 0) { c0 = x; sum = 0; }
                    c1 = x; sum += cols[x]; gap = 0;
                } else if (c0 >= 0 && (++gap > maxGap || x == w)) {
                    if (sum > bestSum) { bestSum = sum; best0 = c0; best1 = c1; }
                    c0 = -1;
                }
            }
            if (best0 < 0 || best1 - best0 < lh) continue;
            const float py = lh * 0.25f, px = lh * 0.4f;
            out.push_back(Rect{std::max(0.f, best0 - px), std::max(0.f, rn.y0 - py),
                               std::min(float(w), best1 + 1 + px), std::min(float(h), rn.y1 + 1 + py)});
        }
        t.split += nowMs() - t0;
        return out;
    }

    // ---- classifier + recognizer ---------------------------------------------------------------------------
    static Image rotate180(const Image& img, const Rect& r)
    {
        Image o;
        o.w = static_cast<int>(r.x1 - r.x0);
        o.h = static_cast<int>(r.y1 - r.y0);
        o.bgra.resize(static_cast<size_t>(o.w) * o.h * 4);
        for (int y = 0; y < o.h; ++y)
            for (int x = 0; x < o.w; ++x)
                std::memcpy(&o.bgra[(static_cast<size_t>(o.h - 1 - y) * o.w + (o.w - 1 - x)) * 4],
                            &img.bgra[((static_cast<size_t>(r.y0) + y) * img.w + static_cast<size_t>(r.x0) + x) * 4], 4);
        return o;
    }

    bool needsFlip(const Image& img, const Rect& r)
    {
        static const float mean[3] = {127.5f, 127.5f, 127.5f};
        static const float scale[3] = {1 / 127.5f, 1 / 127.5f, 1 / 127.5f};
        const float ratio = (r.x1 - r.x0) / (r.y1 - r.y0);
        const int rw = std::min(kClsW, static_cast<int>(std::ceil(kClsH * ratio)));
        std::vector<float> input(static_cast<size_t>(3) * kClsW * kClsH, 0.f);
        resizeNormalize(img, r, rw, kClsH, kClsW, input.data(), mean, scale);
        std::vector<int64_t> os;
        std::vector<float> p = infer(cls, input, {1, 3, kClsH, kClsW}, os);
        return p.size() >= 2 && p[1] > p[0] && p[1] > 0.9f;
    }

    std::string recognize(const Image& img, const Rect& r)
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
        if (static_cast<int64_t>(keys.size()) + 1 == C) keys.push_back(" ");  // use_space_char
        else if (static_cast<int64_t>(keys.size()) != C)
            throw std::runtime_error("dictionary size " + std::to_string(keys.size()) + " != classes "
                                     + std::to_string(C));
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
};

OrtOcr::OrtOcr() : d(std::make_unique<Impl>()) {}

OrtOcr::~OrtOcr()
{
    if (!d->api) return;
    for (auto* n : {&d->det, &d->cls, &d->rec})
        if (n->session) d->api->ReleaseSession(n->session);
    if (d->mem) d->api->ReleaseMemoryInfo(d->mem);
    if (d->env) d->api->ReleaseEnv(d->env);
}

bool OrtOcr::init(const OrtConfig& cfg, std::string& err)
{
    d->cfg = cfg;
    d->dll = LoadLibraryExW((cfg.ortDir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!d->dll) { err = "onnxruntime.dll not loadable from " + narrow(cfg.ortDir); return false; }
    using GetApiBase = const OrtApiBase* (ORT_API_CALL*)();
    auto base = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(d->dll, "OrtGetApiBase")));
    if (!base) { err = "OrtGetApiBase missing"; return false; }
    d->api = base()->GetApi(ORT_API_VERSION);
    if (!d->api) { err = "onnxruntime.dll does not support ORT_API_VERSION " + std::to_string(ORT_API_VERSION); return false; }
    d->version = base()->GetVersionString();
    try {
        d->check(d->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "ocr_bench", &d->env));
        d->check(d->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d->mem));
        d->check(d->api->GetAllocatorWithDefaultOptions(&d->alloc));
        if (!cfg.detModel.empty()) d->open(d->det, cfg.detModel);
        if (!cfg.clsModel.empty()) d->open(d->cls, cfg.clsModel);
        d->open(d->rec, cfg.recModel);
        if (d->rec.inShape.size() == 4 && d->rec.inShape[2] > 0) d->recH = static_cast<int>(d->rec.inShape[2]);
        d->loadKeys();
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

std::vector<std::string> OrtOcr::run(const Image& img, Times& t)
{
    std::vector<Rect> boxes = d->det.session ? d->detect(img, t) : d->splitLines(img, t);
    // Line grouping (OCR_MIMARI G1/K4a): boxes whose vertical centre falls inside a line's first box belong to that
    // line and are read left to right, so one subtitle line never comes back as two lines in the wrong order.
    std::vector<std::vector<Rect>> rows;
    std::sort(boxes.begin(), boxes.end(), [](const Rect& a, const Rect& b) { return a.y0 + a.y1 < b.y0 + b.y1; });
    for (const Rect& r : boxes) {
        const float cy = (r.y0 + r.y1) * 0.5f;
        if (!rows.empty() && cy >= rows.back()[0].y0 && cy <= rows.back()[0].y1) rows.back().push_back(r);
        else rows.push_back({r});
    }
    std::vector<std::string> lines;
    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.x0 < b.x0; });
        std::string text;
        for (const Rect& r : row) {
            const Image* src = &img;
            Rect rr = r;
            Image flipped;
            if (d->cls.session) {
                double c0 = nowMs();
                if (d->needsFlip(img, r)) {
                    flipped = Impl::rotate180(img, r);
                    src = &flipped;
                    rr = Rect{0, 0, float(flipped.w), float(flipped.h)};
                }
                t.cls += nowMs() - c0;
            }
            double r0 = nowMs();
            std::string s = d->recognize(*src, rr);
            t.rec += nowMs() - r0;
            ++t.lines;
            if (s.empty()) continue;
            text += (text.empty() ? "" : " ") + s;
        }
        if (!text.empty()) lines.push_back(std::move(text));
    }
    return lines;
}

std::string OrtOcr::describe() const
{
    return "onnxruntime " + d->version + ", rec height " + std::to_string(d->recH) + ", " +
           std::to_string(d->keys.size()) + " classes (" + d->keysSource + ")";
}

} // namespace bench
