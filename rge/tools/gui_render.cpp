// ===========================================================================
//  gui_render.cpp -- render the NOISEFORGE editor offscreen to a PNG
//
//  gui_render NoiseForge.vst3 out.png             one frame
//  gui_render NoiseForge.vst3 out.png --refresh   a frame, a resize, a
//                                                 hide/show and a full
//                                                 detach/reattach
//  gui_render NoiseForge.vst3 out.png --live      run audio into the panel
//                                                 first, so the scope, the
//                                                 spectrum and the voice
//                                                 counter are not empty
//
//  The PNG is written with stored (uncompressed) deflate blocks, so no zlib
//  and no third-party image library is involved.
// ===========================================================================

#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"

#include "../src/NoiseForgeCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static bool iidEq(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

template <class T>
static T* query(void* obj, const TUID iid) {
    if (!obj) return nullptr;
    void* out = nullptr;
    if (static_cast<FUnknown*>(obj)->queryInterface(iid, &out) != kResultOk)
        return nullptr;
    return static_cast<T*>(out);
}

// ---------------------------------------------------------------------------
//  Minimal PNG writer (RGBA, 8 bit, stored deflate)
// ---------------------------------------------------------------------------
static uint32_t g_crcTable[256];
static void crcInit() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[n] = c;
    }
}
static uint32_t crc32(const unsigned char* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) crc = g_crcTable[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t adler32(const unsigned char* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
static void be32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back((unsigned char)(x >> 24));
    v.push_back((unsigned char)(x >> 16));
    v.push_back((unsigned char)(x >> 8));
    v.push_back((unsigned char)x);
}
static void wrChunk(FILE* f, const char type[4], const unsigned char* data,
                    size_t len) {
    unsigned char hdr[8] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                             (unsigned char)(len >> 8), (unsigned char)len };
    std::memcpy(hdr + 4, type, 4);
    std::fwrite(hdr, 1, 8, f);
    if (len) std::fwrite(data, 1, len, f);
    const uint32_t c =
        crc32(data, len, crc32((const unsigned char*)type, 4)) ^ 0xFFFFFFFFu;
    unsigned char cb[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16),
                            (unsigned char)(c >> 8), (unsigned char)c };
    std::fwrite(cb, 1, 4, f);
}

static bool writePng(const char* path, const unsigned char* bgra, int w, int h) {
    crcInit();
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(w * 4 + 1) * (size_t)h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = bgra + ((size_t)y * (size_t)w + (size_t)x) * 4;
            raw.push_back(p[2]);
            raw.push_back(p[1]);
            raw.push_back(p[0]);
            // GDI blits do not maintain the alpha byte of a 32bpp DIB, so it
            // reads back as 0. Writing that through produces a fully
            // transparent PNG, which every viewer shows as white.
            raw.push_back(255);
        }
    }
    std::vector<unsigned char> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = raw.size() - off;
        if (n > 65535) n = 65535;
        const bool last = (off + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((unsigned char)(n & 0xFF));
        z.push_back((unsigned char)(n >> 8));
        const uint16_t nl = (uint16_t)(~n & 0xFFFF);
        z.push_back((unsigned char)(nl & 0xFF));
        z.push_back((unsigned char)(nl >> 8));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
        off += n;
    }
    be32(z, adler32(raw.data(), raw.size()));

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::fwrite(sig, 1, 8, f);
    std::vector<unsigned char> ihdr;
    be32(ihdr, (uint32_t)w);
    be32(ihdr, (uint32_t)h);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    wrChunk(f, "IHDR", ihdr.data(), ihdr.size());
    wrChunk(f, "IDAT", z.data(), z.size());
    wrChunk(f, "IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
static long paintToDib(HWND child, int W, int H, const char* outPath) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits || !memdc) return -1;
    HGDIOBJ oldBmp = SelectObject(memdc, dib);

    RECT full = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memdc, &full, bg);
    DeleteObject(bg);

    SendMessageW(child, WM_PRINTCLIENT, (WPARAM)memdc, 0);

    const unsigned char* px = static_cast<const unsigned char*>(bits);
    long nonBlack = 0;
    for (long i = 0; i < (long)W * H; ++i)
        if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) ++nonBlack;
    if (outPath) writePng(outPath, px, W, H);

    SelectObject(memdc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memdc);
    return nonBlack;
}

// ---------------------------------------------------------------------------
//  A minimal event list, so the panel can be rendered with a note playing.
// ---------------------------------------------------------------------------
class EvList : public IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (iidEq(iid, IEventList::iid) || iidEq(iid, FUnknown::iid)) {
            *obj = static_cast<IEventList*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }
    int32 PLUGIN_API getEventCount() SMTG_OVERRIDE { return (int32)e_.size(); }
    tresult PLUGIN_API getEvent(int32 i, Event& e) SMTG_OVERRIDE {
        if (i < 0 || i >= (int32)e_.size()) return kInvalidArgument;
        e = e_[(size_t)i];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) SMTG_OVERRIDE {
        e_.push_back(e);
        return kResultOk;
    }
    void clear() { e_.clear(); }
    void note(int16 pitch, float vel) {
        Event e = {};
        e.type = Event::kNoteOnEvent;
        e.noteOn.pitch = pitch;
        e.noteOn.velocity = vel;
        e.noteOn.noteId = -1;
        e_.push_back(e);
    }

private:
    std::vector<Event> e_;
};

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dllPath = "NoiseForge.vst3";
    const char* outPath = "noiseforge_ui.png";
    if (argc > 1 && argv[1][0] != '-') dllPath = argv[1];
    if (argc > 2 && argv[2][0] != '-') outPath = argv[2];
    bool refresh = false;
    bool live = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--refresh") == 0) refresh = true;
        if (std::strcmp(argv[i], "--live") == 0) live = true;
    }

    std::printf("==========================================================================\n");
    std::printf(" NOISEFORGE GUI render: %s -> %s%s\n", dllPath, outPath,
                refresh ? "  [refresh]" : "");
    std::printf("==========================================================================\n");

    HMODULE mod = LoadLibraryA(dllPath);
    if (!mod) { std::printf("  LoadLibrary failed (%lu)\n", GetLastError()); return 2; }

    typedef IPluginFactory* (*GetFactoryFn)();
    GetFactoryFn getFactory =
        reinterpret_cast<GetFactoryFn>(GetProcAddress(mod, "GetPluginFactory"));
    if (!getFactory) { std::printf("  no GetPluginFactory\n"); return 2; }

    IPluginFactory* factory = getFactory();
    TUID cid = {};
    bool found = false;
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        PClassInfo ci = {};
        if (factory->getClassInfo(i, &ci) != kResultOk) continue;
        if (std::strcmp(ci.category, kVstAudioEffectClass) == 0) {
            std::memcpy(cid, ci.cid, 16);
            found = true;
            break;
        }
    }
    if (!found) { std::printf("  no audio class\n"); return 2; }

    void* obj = nullptr;
    if (factory->createInstance(cid, IEditController::iid, &obj) != kResultOk || !obj) {
        std::printf("  createInstance(IEditController) failed\n");
        return 2;
    }
    IEditController* ctrl = static_cast<IEditController*>(obj);
    IComponent* comp = query<IComponent>(obj, IComponent::iid);
    IAudioProcessor* proc = query<IAudioProcessor>(obj, IAudioProcessor::iid);
    if (comp) comp->initialize(nullptr);
    ctrl->initialize(nullptr);

    // ---- run the instrument so the scope, spectrum and voice count are live
    if (live && comp && proc) {
        ProcessSetup setup = {};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 512;
        setup.sampleRate = 48000.0;
        proc->setupProcessing(setup);
        comp->setActive(true);

        float outL[512] = {}, outR[512] = {};
        float* outs[2] = { outL, outR };
        AudioBusBuffers bus = {};
        bus.numChannels = 2;
        bus.channelBuffers32 = outs;
        ProcessData data = {};
        data.numOutputs = 1;
        data.outputs = &bus;
        data.numSamples = 512;
        data.symbolicSampleSize = kSample32;

        EvList ev;
        ev.note(45, 0.9f);
        data.inputEvents = &ev;
        proc->process(data);
        data.inputEvents = nullptr;
        ev.clear();

        // ~1.5 s of audio: enough for the scope ring and the band meter to
        // settle, and for the decay to have moved the envelope off its peak.
        for (int i = 0; i < 3 * 48000 / 512; ++i) proc->process(data);

        comp->setActive(false);
        std::printf("  rendered 3 s of audio into the panel\n");
    }

    IPlugView* view = ctrl->createView(nullptr);
    if (!view) { std::printf("  createView returned null\n"); return 2; }

    ViewRect vr = {};
    view->getSize(&vr);
    const int W = vr.right - vr.left;
    const int H = vr.bottom - vr.top;
    std::printf("  editor size: %d x %d\n", W, H);

    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, W, H, nullptr,
                                  nullptr, nullptr, nullptr);
    if (!parent) { std::printf("  parent window failed\n"); return 2; }
    if (view->attached(parent, kPlatformTypeHWND) != kResultOk) {
        std::printf("  view->attached failed\n");
        return 2;
    }

    HWND child = FindWindowExW(parent, nullptr, L"NoiseForgePanel", nullptr);
    if (!child) { std::printf("  editor created no child window\n"); return 2; }

    int failures = 0;
    auto step = [&](const char* name, int w2, int h2, const char* out) {
        if (w2 > 0 && h2 > 0) {
            ViewRect r{ 0, 0, (int32)w2, (int32)h2 };
            view->onSize(&r);
        }
        ViewRect now{};
        view->getSize(&now);
        const int cw = now.right - now.left;
        const int chh = now.bottom - now.top;
        RECT cr{};
        if (child) GetClientRect(child, &cr);
        const long px = paintToDib(child, cw, chh, out);
        const double pct = (double)px * 100.0 / ((double)cw * (double)chh);
        std::printf("  %-22s view=%dx%d child=%ldx%ld drawn=%.1f%%  %s\n", name, cw,
                    chh, (long)(cr.right - cr.left), (long)(cr.bottom - cr.top), pct,
                    px > 1000 ? "ok" : "BLANK");
        if (px <= 1000) ++failures;
    };

    step("first paint", W, H, outPath);

    if (refresh) {
        step("after onSize 960x600", 960, 600, nullptr);
        step("after onSize 1280x800", 1280, 800, nullptr);

        if (child) {
            ShowWindow(child, SW_HIDE);
            ShowWindow(child, SW_SHOW);
        }
        step("after hide/show", 0, 0, nullptr);

        // A host "reload" is: tear the editor down completely, then build a
        // fresh view. Reusing the same view object would not exercise the path
        // that actually breaks.
        view->removed();
        view->release();
        view = ctrl->createView(nullptr);
        std::printf("  reload: createView -> %s\n", view ? "ok" : "NULL");
        if (!view) {
            ++failures;
        } else {
            const tresult re = view->attached(parent, kPlatformTypeHWND);
            std::printf("  reload: attached -> %s\n",
                        re == kResultOk ? "ok" : "FAILED");
            child = FindWindowExW(parent, nullptr, L"NoiseForgePanel", nullptr);
            if (!child) {
                std::printf("  reload created no child window\n");
                ++failures;
            } else {
                step("after reload", 0, 0, nullptr);
            }
        }
    }

    if (view) { view->removed(); view->release(); }
    DestroyWindow(parent);

    if (proc) proc->release();
    static_cast<FUnknown*>(obj)->release();
    FreeLibrary(mod);

    std::printf("\n==========================================================================\n");
    if (failures == 0) std::printf("GUI RENDERED%s\n", refresh ? " AND SURVIVED REFRESH" : "");
    else std::printf("GUI FAILED (%d blank frame(s))\n", failures);
    std::printf("==========================================================================\n");
    return failures == 0 ? 0 : 1;
}
