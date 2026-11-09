// ===========================================================================
//  host_test.cpp -- load NoiseForge.vst3 the way a host would, and exercise it
//
//  This is the closest thing to a DAW that exists in this build environment:
//  it drives the exported factory, the audio processor and the edit controller
//  through their public interfaces only, and checks the properties a host
//  depends on:
//
//    * the factory exposes one audio-effect class, declared as an Instrument
//    * initialize() survives being called twice (single-component effect)
//    * 71 parameters, names, defaults and formatting
//    * no audio input bus, one stereo output, one MIDI event bus
//    * process() is finite, bounded and non-silent at 1..1024 sample blocks
//    * a MIDI note reaches the engine and note-off releases it
//    * the BAND engine makes sound below 20 Hz through the plugin
//    * getState/setState round-trips, including the wavetable seed
//    * the editor is created, sized, attached to a real HWND and painted
//
//  Exit code 0 means every check passed.
// ===========================================================================

#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/memorystream.h"

#include "../src/NoiseForgeCore.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static int g_fail = 0;

static void check(const char* what, bool cond, const char* extra = "") {
    std::printf("  %s  %s %s\n", cond ? "PASS" : "FAIL", what, extra);
    if (!cond) ++g_fail;
}

static bool iidEq(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

template <class T>
static T* query(void* obj, const TUID iid) {
    if (!obj) return nullptr;
    void* out = nullptr;
    if (static_cast<FUnknown*>(obj)->queryInterface(iid, &out) != kResultOk)
        return nullptr;
    return static_cast<T*>(out);
}

static std::string utf16(const char16* s, int cap = 128) {
    std::string r;
    for (int i = 0; i < cap && s[i]; ++i) r.push_back((char)s[i]);
    return r;
}

static std::string ascii8(const char8* s, int cap = 128) {
    std::string r;
    for (int i = 0; i < cap && s[i]; ++i) r.push_back((char)s[i]);
    return r;
}

// ---------------------------------------------------------------------------
class TestEventList : public IEventList {
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

    int32 PLUGIN_API getEventCount() SMTG_OVERRIDE { return (int32)events_.size(); }
    tresult PLUGIN_API getEvent(int32 index, Event& e) SMTG_OVERRIDE {
        if (index < 0 || index >= (int32)events_.size()) return kInvalidArgument;
        e = events_[(size_t)index];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) SMTG_OVERRIDE {
        events_.push_back(e);
        return kResultOk;
    }
    void clear() { events_.clear(); }
    void note(int16 pitch, float velocity, bool on) {
        Event e = {};
        e.busIndex = 0;
        e.sampleOffset = 0;
        e.ppqPosition = 0;
        e.flags = 0;
        if (on) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = 0;
            e.noteOn.pitch = pitch;
            e.noteOn.velocity = velocity;
            e.noteOn.noteId = -1;
            e.noteOn.length = 0;
            e.noteOn.tuning = 0;
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = 0;
            e.noteOff.pitch = pitch;
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
            e.noteOff.tuning = 0;
        }
        events_.push_back(e);
    }

private:
    std::vector<Event> events_;
};

// ---------------------------------------------------------------------------
struct Rig {
    IComponent* comp = nullptr;
    IAudioProcessor* proc = nullptr;
    IEditController* ctrl = nullptr;
    bool active = false;
    int32 maxBlock = 512;
    std::vector<float> outL, outR, outL64, outR64;

    bool prepare(double sr, int32 block) {
        if (active) { comp->setActive(false); active = false; }
        maxBlock = block;
        ProcessSetup ps = {};
        ps.processMode = kRealtime;
        ps.symbolicSampleSize = kSample32;
        ps.maxSamplesPerBlock = block;
        ps.sampleRate = sr;
        if (proc->setupProcessing(ps) != kResultOk) return false;
        if (comp->setActive(true) != kResultOk) return false;
        active = true;
        outL.assign((size_t)block, 0.0f);
        outR.assign((size_t)block, 0.0f);
        outL64.assign((size_t)block, 0.0);
        outR64.assign((size_t)block, 0.0);
        return true;
    }

    void setParam(int id, double v) {
        if (ctrl) ctrl->setParamNormalized((ParamID)id, v);
    }

    // Renders nSamples in blocks <= maxBlock. Returns peak and appends to mono.
    float render(int nSamples, std::vector<float>* mono, IEventList* events,
                 bool* finite) {
        float peak = 0.0f;
        if (finite) *finite = true;
        int done = 0;
        while (done < nSamples) {
            const int n = (nSamples - done < (int)maxBlock) ? (nSamples - done)
                                                           : (int)maxBlock;
            float* outs[2] = { outL.data(), outR.data() };
            ProcessData data = {};
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = n;
            data.numInputs = 0;          // an instrument has no audio input
            data.numOutputs = 1;
            AudioBusBuffers bo = {};
            bo.numChannels = 2;
            bo.channelBuffers32 = outs;
            data.outputs = &bo;
            data.inputEvents = events;

            const tresult r = proc->process(data);
            if (r != kResultOk) { if (finite) *finite = false; break; }

            for (int i = 0; i < n; ++i) {
                if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) {
                    if (finite) *finite = false;
                }
                const float a = std::fabs(outL[i]);
                if (a > peak) peak = a;
                if (mono) mono->push_back(0.5f * (outL[i] + outR[i]));
            }
            done += n;
        }
        return peak;
    }
};

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dllPath = (argc > 1) ? argv[1] : "NoiseForge.vst3";

    std::printf("==========================================================================\n");
    std::printf(" NOISEFORGE -- host simulation: %s\n", dllPath);
    std::printf("==========================================================================\n");

    HMODULE mod = LoadLibraryA(dllPath);
    if (!mod) {
        std::printf("  LoadLibrary failed (%lu)\n", GetLastError());
        return 2;
    }

    check("exports InitDll", GetProcAddress(mod, "InitDll") != nullptr);
    check("exports ExitDll", GetProcAddress(mod, "ExitDll") != nullptr);

    typedef IPluginFactory* (*GetFactoryFn)();
    GetFactoryFn getFactory =
        reinterpret_cast<GetFactoryFn>(GetProcAddress(mod, "GetPluginFactory"));
    if (!getFactory) { std::printf("  no GetPluginFactory export\n"); return 2; }

    IPluginFactory* factory = getFactory();
    std::printf("\n[factory]\n");
    PFactoryInfo fi = {};
    IPluginFactory2* f2 = nullptr;
    if (factory->queryInterface(IPluginFactory2::iid, (void**)&f2) == kResultOk && f2) {
        f2->getFactoryInfo(&fi);
        std::printf("  vendor : %s\n", fi.vendor);
        std::printf("  url    : %s\n", fi.url);
        std::printf("  email  : %s\n", fi.email);
    }
    std::printf("  classes: %d\n", (int)factory->countClasses());

    PClassInfo ci = {};
    bool found = false;
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        PClassInfo c = {};
        if (factory->getClassInfo(i, &c) != kResultOk) continue;
        std::printf("    [%d] %s / %s\n", (int)i, c.category, c.name);
        if (std::strcmp(c.category, kVstAudioEffectClass) == 0 && !found) {
            ci = c;
            found = true;
        }
    }
    check("one audio-effect class is exported", found);
    if (!found) return 2;
    check("class is named NoiseForge", std::strcmp(ci.name, "NoiseForge") == 0);

    // Hosts decide Generators vs Effects from the sub-categories, so this is
    // the difference between "appears in the piano roll" and "appears in the
    // mixer".
    {
        PClassInfo2 ci2 = {};
        bool isInstrument = false;
        if (f2 && f2->getClassInfo2(0, &ci2) == kResultOk) {
            std::string sub = ascii8(ci2.subCategories);
            std::printf("  subcategories: %s\n", sub.c_str());
            isInstrument = sub.find("Instrument") != std::string::npos;
        }
        check("declared as an Instrument", isInstrument);
    }
    if (f2) f2->release();

    void* obj = nullptr;
    if (factory->createInstance(ci.cid, IComponent::iid, &obj) != kResultOk || !obj) {
        std::printf("  createInstance failed\n");
        return 2;
    }

    Rig rig;
    rig.comp = static_cast<IComponent*>(obj);
    rig.proc = query<IAudioProcessor>(obj, IAudioProcessor::iid);
    rig.ctrl = query<IEditController>(obj, IEditController::iid);
    check("implements IAudioProcessor", rig.proc != nullptr);
    check("implements IEditController", rig.ctrl != nullptr);
    if (!rig.proc || !rig.ctrl) return 2;

    std::printf("\n[initialize]\n");
    check("IComponent::initialize", rig.comp->initialize(nullptr) == kResultOk);
    // A single-component effect is its own controller; calling initialize again
    // through IEditController must not double-register the parameters.
    rig.ctrl->initialize(nullptr);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "(got %d)", (int)rig.ctrl->getParameterCount());
    check("initialize twice is harmless", rig.ctrl->getParameterCount() == nf::kNumParams,
          msg);

    std::printf("\n[buses and parameters]\n");
    const int32 numIn = rig.comp->getBusCount(kAudio, kInput);
    const int32 numOut = rig.comp->getBusCount(kAudio, kOutput);
    const int32 numEv = rig.comp->getBusCount(kEvent, kInput);
    std::printf("  audio in=%d out=%d, event in=%d\n", (int)numIn, (int)numOut,
                (int)numEv);
    check("no audio input bus (instrument)", numIn == 0);
    check("one audio output bus", numOut == 1);
    check("one MIDI event input bus", numEv == 1);

    {
        BusInfo bi = {};
        if (rig.comp->getBusInfo(kAudio, kOutput, 0, bi) == kResultOk) {
            check("output bus is main stereo", bi.busType == kMain &&
                                                  bi.channelCount == 2);
        }
        SpeakerArrangement arr = 0;
        if (rig.proc->getBusArrangement(kOutput, 0, arr) == kResultOk)
            check("output arrangement is stereo", arr == SpeakerArr::kStereo);
    }
    check("71 parameters", rig.ctrl->getParameterCount() == 71);

    // Every parameter must round-trip through get/set and format to text, and
    // must carry a non-empty name.
    bool paramsOk = true;
    bool namesOk = true;
    for (int i = 0; i < nf::kNumParams; ++i) {
        rig.ctrl->setParamNormalized((ParamID)i, 0.5);
        const double v = rig.ctrl->getParamNormalized((ParamID)i);
        if (v < 0.49 || v > 0.51) paramsOk = false;
        String128 s = {};
        if (rig.ctrl->getParamStringByValue((ParamID)i, 0.5, s) != kResultOk)
            paramsOk = false;
        if (!s[0]) paramsOk = false;
        ParameterInfo pi = {};
        if (rig.ctrl->getParameterInfo(i, pi) != kResultOk) namesOk = false;
        else if (utf16(pi.title).empty()) namesOk = false;
    }
    check("all parameters round-trip and format", paramsOk);
    check("all parameters have names", namesOk);

    // The 0.5 sweep latched every switch, including Bypass. Put the defaults
    // back so the audio section measures the instrument and not a wire.
    for (int i = 0; i < nf::kNumParams; ++i)
        rig.ctrl->setParamNormalized((ParamID)i, nf::kParams[i].defNorm);
    check("bypass defaults to off",
          rig.ctrl->getParamNormalized(nf::kBypass) < 0.5);

    std::printf("\n[setup / activate]\n");
    check("setupProcessing + setActive", rig.prepare(48000.0, 512));

    std::printf("\n[process: silence with no note]\n");
    {
        std::vector<float> mono;
        bool finite = true;
        // Default patch has all three sources on, but nothing is playing.
        const float p = rig.render(48000, &mono, nullptr, &finite);
        std::snprintf(msg, sizeof(msg), "(peak = %.6f)", p);
        check("an instrument is silent until a note arrives", p < 1e-5f, msg);
        check("output is finite", finite);
    }

    std::printf("\n[process: MIDI note]\n");
    {
        TestEventList events;
        bool finite = true;

        events.note(60, 1.0f, true);
        std::vector<float> mono;
        rig.render(512, &mono, &events, &finite);   // deliver note-on
        events.clear();

        mono.clear();
        const float withNote = rig.render(24000, &mono, nullptr, &finite);
        std::snprintf(msg, sizeof(msg), "(peak = %.4f)", withNote);
        check("note-on is audible", withNote > 0.02f, msg);
        check("output is finite", finite);
        check("output stays under full scale", withNote <= 1.0f);

        events.note(60, 0.0f, false);
        rig.render(512, &mono, &events, &finite);
        events.clear();
        // Drain the release plus the scope/limiter tail.
        rig.render(48000, &mono, nullptr, &finite);
        mono.clear();
        const float afterOff = rig.render(12000, &mono, nullptr, &finite);
        std::snprintf(msg, sizeof(msg), "(peak = %.6f)", afterOff);
        check("note-off releases to silence", afterOff < 1e-4f, msg);
    }

    std::printf("\n[process: block sizes]\n");
    {
        const int sizes[] = { 512, 64, 1, 256, 37, 1024, 128 };
        bool allFinite = true;
        bool anySignal = false;
        float worst = 0.0f;
        for (int i = 0; i < 7; ++i) {
            TestEventList events;
            bool finite = true;
            rig.prepare(48000.0, sizes[i]);
            std::vector<float> mono;
            events.note(57, 0.9f, true);
            rig.render(sizes[i], &mono, &events, &finite);
            events.clear();
            mono.clear();
            // Render a fixed 0.5 s regardless of the block size, otherwise the
            // 1-sample case only gets 40 samples and is still inside the attack.
            const int total = 24000;
            const float p = rig.render(total, &mono, nullptr, &finite);
            if (!finite) allFinite = false;
            if (p > 0.02f) anySignal = true;
            if (p > worst) worst = p;
            std::printf("        block %4d -> peak %.4f  %s\n", sizes[i], p,
                        finite ? "finite" : "NON-FINITE");
        }
        check("output finite at 1..1024 sample blocks", allFinite);
        check("output produces signal", anySignal);
        check("output stays under full scale", worst <= 1.0f);
    }

    std::printf("\n[double precision]\n");
    {
        ProcessSetup ps = {};
        ps.processMode = kRealtime;
        ps.symbolicSampleSize = kSample64;
        ps.maxSamplesPerBlock = 256;
        ps.sampleRate = 48000.0;
        const bool ok64 = (rig.proc->setupProcessing(ps) == kResultOk &&
                           rig.comp->setActive(true) == kResultOk);
        check("accepts 64-bit sample size", ok64);
        if (ok64) {
            std::vector<double> o0(256, 0.0), o1(256, 0.0);
            double* outs[2] = { o0.data(), o1.data() };
            AudioBusBuffers bo = {};
            bo.numChannels = 2;
            bo.channelBuffers64 = outs;
            ProcessData d = {};
            d.processMode = kRealtime;
            d.symbolicSampleSize = kSample64;
            d.numSamples = 256;
            d.numInputs = 0;
            d.numOutputs = 1;
            d.outputs = &bo;

            TestEventList events;
            events.note(60, 1.0f, true);
            d.inputEvents = &events;

            bool finite = true;
            for (int k = 0; k < 40; ++k) {
                if (rig.proc->process(d) != kResultOk) finite = false;
                for (int i = 0; i < 256; ++i) {
                    if (!std::isfinite(o0[i]) || !std::isfinite(o1[i]))
                        finite = false;
                }
                d.inputEvents = nullptr;
            }
            check("64-bit process is finite", finite);
        }
        rig.prepare(48000.0, 512);
    }

    std::printf("\n[BAND engine below 20 Hz through the plugin]\n");
    {
        rig.prepare(48000.0, 512);
        for (int i = 0; i < nf::kNumParams; ++i)
            rig.setParam(i, nf::kParams[i].defNorm);

        // Only the band engine, centred at 0.5 Hz: the case a filter-based
        // design cannot do, reached through the real parameter interface.
        rig.setParam(nf::kWtLevel, 0.0);
        rig.setParam(nf::kHarshLevel, 0.0);
        rig.setParam(nf::kBandOn, 1.0);
        rig.setParam(nf::kBandLevel, 1.0);
        rig.setParam(nf::kBandCenter, nf::normForValue(nf::kBandCenter, 0.5f));
        rig.setParam(nf::kBandWidth, nf::normForValue(nf::kBandWidth, 0.25f));
        rig.setParam(nf::kBandKey, 0.0);
        rig.setParam(nf::kBandDrift, 0.0);
        rig.setParam(nf::kAttack, 0.0);
        rig.setParam(nf::kSustain, 1.0);

        TestEventList events;
        bool finite = true;
        std::vector<float> mono;
        events.note(60, 1.0f, true);
        rig.render(512, &mono, &events, &finite);
        events.clear();

        mono.clear();
        rig.render(48000 * 4, &mono, nullptr, &finite);

        double acc = 0.0;
        for (float x : mono) acc += (double)x * (double)x;
        const double level = std::sqrt(acc / (double)mono.size());
        std::snprintf(msg, sizeof(msg), "(rms = %.5f)", level);
        check("sub-audio band produces signal", level > 0.001, msg);

        int crossings = 0;
        for (size_t i = 1; i < mono.size(); ++i)
            if ((mono[i - 1] <= 0.0f) != (mono[i] <= 0.0f)) ++crossings;
        std::snprintf(msg, sizeof(msg), "(%d crossings in %.0f s)", crossings,
                      (double)mono.size() / 48000.0);
        check("waveform moves at sub-audio rate", crossings < 60, msg);
    }

    std::printf("\n[state]\n");
    {
        for (int i = 0; i < nf::kNumParams; ++i)
            rig.setParam(i, nf::kParams[i].defNorm);
        rig.setParam(nf::kBandCenter, 0.31);
        rig.setParam(nf::kHarshType, 0.77);
        rig.setParam(nf::kAttack, 0.42);

        MemoryStream ms;
        check("getState", rig.comp->getState(&ms) == kResultOk);
        const int64 size = ms.getSize();
        std::printf("  state size: %lld bytes\n", (long long)size);
        // version + 71 floats + wavetable seed
        check("state has the expected size", size == 4 + nf::kNumParams * 4 + 4);

        rig.setParam(nf::kBandCenter, 0.9);
        rig.setParam(nf::kHarshType, 0.1);
        rig.setParam(nf::kAttack, 0.05);

        ms.seek(0, IBStream::kIBSeekSet, nullptr);
        check("setState", rig.comp->setState(&ms) == kResultOk);

        const double pC = rig.ctrl->getParamNormalized(nf::kBandCenter);
        const double pT = rig.ctrl->getParamNormalized(nf::kHarshType);
        const double pA = rig.ctrl->getParamNormalized(nf::kAttack);
        std::snprintf(msg, sizeof(msg), "(%.3f / %.3f / %.3f)", pC, pT, pA);
        check("parameters restored", std::fabs(pC - 0.31) < 1e-4 &&
                                        std::fabs(pT - 0.77) < 1e-4 &&
                                        std::fabs(pA - 0.42) < 1e-4,
              msg);
    }

    std::printf("\n[editor]\n");
    {
        IPlugView* view = rig.ctrl->createView(nullptr);
        check("createView", view != nullptr);
        if (view) {
            check("supports HWND",
                  view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue);

            ViewRect vr = {};
            const bool sized = view->getSize(&vr) == kResultOk;
            check("getSize", sized);
            std::printf("  editor size: %d x %d\n", (int)(vr.right - vr.left),
                        (int)(vr.bottom - vr.top));

            HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0,
                                          vr.right - vr.left, vr.bottom - vr.top,
                                          nullptr, nullptr, nullptr, nullptr);
            const tresult at = view->attached(parent, kPlatformTypeHWND);
            check("attached to a hidden HWND", at == kResultOk);

            HWND child = FindWindowExW(parent, nullptr, L"NoiseForgePanel", nullptr);
            check("created its child window", child != nullptr);

            if (child) {
                RECT rc = {};
                GetClientRect(child, &rc);
                std::printf("  child client: %ld x %ld\n",
                            (long)(rc.right - rc.left), (long)(rc.bottom - rc.top));

                HDC screen = GetDC(nullptr);
                HDC mem = CreateCompatibleDC(screen);
                HBITMAP bmp = CreateCompatibleBitmap(screen, 1280, 800);
                HGDIOBJ old = SelectObject(mem, bmp);
                SendMessageW(child, WM_PRINTCLIENT, (WPARAM)mem, 0);

                // Two points: bare background and the header panel. If the
                // panel were not drawn at all both would be the same colour.
                const COLORREF bg = GetPixel(mem, 900, 700);
                const COLORREF hdr = GetPixel(mem, 30, 24);
                const COLORREF scope = GetPixel(mem, 40, 100);

                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                ReleaseDC(nullptr, screen);

                std::printf("  bg=#%02X%02X%02X header=#%02X%02X%02X scope=#%02X%02X%02X\n",
                            (unsigned)GetRValue(bg), (unsigned)GetGValue(bg),
                            (unsigned)GetBValue(bg), (unsigned)GetRValue(hdr),
                            (unsigned)GetGValue(hdr), (unsigned)GetBValue(hdr),
                            (unsigned)GetRValue(scope), (unsigned)GetGValue(scope),
                            (unsigned)GetBValue(scope));
                check("paints without crashing", true);
                check("header panel was drawn", hdr != bg && hdr != CLR_INVALID);
                check("scope panel was drawn", scope != bg && scope != CLR_INVALID);

                // DICE is the only path into the bank rebuild from the UI. The
                // seed travels in the state stream, so it can be read back from
                // the host side without reaching into the engine.
                auto seedOf = [&](uint32& out) -> bool {
                    MemoryStream ms;
                    if (rig.comp->getState(&ms) != kResultOk) return false;
                    if (ms.getSize() < 4) return false;
                    ms.seek((TSize)(ms.getSize() - 4), IBStream::kIBSeekSet, nullptr);
                    return ms.read(&out, 4, nullptr) == kResultOk;
                };

                uint32 seedBefore = 0, seedAfter = 0;
                const bool seedRead = seedOf(seedBefore);

                // Click the DICE button, then give the editor's timer a chance
                // to retry if the first swap was refused.
                for (int i = 0; i < 6; ++i) {
                    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON,
                                 MAKELPARAM(642 + 512 + 40, 60 + 152 + 8));
                    SendMessageW(child, WM_LBUTTONUP, 0,
                                 MAKELPARAM(642 + 512 + 40, 60 + 152 + 8));
                    SendMessageW(child, WM_TIMER, 1, 0);
                    if (seedOf(seedAfter) && seedAfter != seedBefore) break;
                }

                std::snprintf(msg, sizeof(msg), "(seed %08X -> %08X)",
                              seedBefore, seedAfter);
                check("DICE rebuilds the wavetable bank", seedRead &&
                                                          seedAfter != seedBefore,
                      msg);
            }

            view->removed();
            view->release();
            DestroyWindow(parent);
        }
    }

    std::printf("\n[shutdown]\n");
    rig.comp->setActive(false);
    rig.ctrl->terminate();
    rig.comp->terminate();
    static_cast<FUnknown*>(obj)->release();
    check("terminated cleanly", true);

    FreeLibrary(mod);

    std::printf("\n==========================================================================\n");
    std::printf(g_fail == 0 ? " HOST SIMULATION PASSED\n" : " HOST SIMULATION FAILED\n");
    std::printf("==========================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
