// ===========================================================================
//  NoiseForgeVST3.cpp -- VST3 wrapper for the NOISEFORGE engine
//
//  One translation unit on purpose: the wrapper is the part that cannot be
//  tested without a host, so it is kept small and conservative about which SDK
//  symbols it touches.
//
//  THREADING
//      audio thread    -> process()            -> engine + atomics
//      UI thread       -> window proc          -> IParamSink only
//      host/automation -> setParamNormalized() -> atomics + a window message
//  The editor never touches the engine, and the audio thread never touches the
//  editor. The only shared state is the parameter atomics and the scope ring,
//  which is a ring of std::atomic<float>.
//
//  This is an INSTRUMENT: it declares no audio input, registers one stereo
//  output and one MIDI event bus, and uses PlugType::kInstrumentSynth so hosts
//  (FL Studio in particular) file it under Generators and route the piano roll
//  to it without any manual wiring.
// ===========================================================================

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fstreamer.h"

#include "NoiseForgeEditor.h"
#include "../src/NoiseForgeCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void widen(char16* dst, const char* src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; i < cap - 1 && src && src[i]; ++i)
        dst[i] = static_cast<char16>(static_cast<unsigned char>(src[i]));
    for (; i < cap; ++i) dst[i] = 0;
}

const char16* outputBusName() {
    static char16 n[64];
    static bool init = false;
    if (!init) { widen(n, "Stereo Out", 64); init = true; }
    return n;
}
const char16* eventBusName() {
    static char16 n[64];
    static bool init = false;
    if (!init) { widen(n, "MIDI In", 64); init = true; }
    return n;
}

// ---------------------------------------------------------------------------
class NoiseForgeVST3 : public SingleComponentEffect, public nf::IParamSink {
public:
    NoiseForgeVST3();

    static FUnknown* createInstance(void*) {
        return static_cast<IAudioProcessor*>(new NoiseForgeVST3());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    tresult PLUGIN_API setupProcessing(ProcessSetup& s) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize(int32 n) SMTG_OVERRIDE;
    // An instrument with a 12 s release must tell the host there is a tail, or
    // offline bounce / freeze cuts the last note's decay.
    uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE {
        const double sr = hostRate_ > 1.0 ? hostRate_ : 48000.0;
        return (uint32)(12.5 * sr);
    }
    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE;

    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) SMTG_OVERRIDE;
    tresult PLUGIN_API getParamStringByValue(ParamID tag, ParamValue value,
                                             String128 string) SMTG_OVERRIDE;

    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE;

    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE;

    // ---- nf::IParamSink ---------------------------------------------------
    void onBeginEdit(int id) SMTG_OVERRIDE { beginEdit((ParamID)id); }
    void onPerformEdit(int id, double v) SMTG_OVERRIDE {
        performEdit((ParamID)id, v);
    }
    void onEndEdit(int id) SMTG_OVERRIDE { endEdit((ParamID)id); }
    double getNormalized(int id) SMTG_OVERRIDE {
        return getParamNormalized((ParamID)id);
    }
    void copyScope(float* dst, int n) SMTG_OVERRIDE { engine_.copyScope(dst, n); }
    float peak() SMTG_OVERRIDE { return engine_.peak(); }
    float takePeak() SMTG_OVERRIDE { return engine_.takePeak(); }
    uint32_t scopeCount() SMTG_OVERRIDE { return engine_.scopeGeneration(); }
    bool randomizeTables() SMTG_OVERRIDE { return engine_.randomizeTables(); }
    uint32_t tableSeed() SMTG_OVERRIDE { return engine_.tableSeed(); }
    const char* tableName(int t) SMTG_OVERRIDE { return engine_.tableName(t); }
    void copyTablePreview(int t, float pos, float* dst, int n) SMTG_OVERRIDE {
        engine_.copyTablePreview(t, pos, dst, n);
    }
    int activeVoices() SMTG_OVERRIDE { return engine_.activeVoices(); }
    double sampleRate() SMTG_OVERRIDE {
        return hostRate_ > 1.0 ? hostRate_ : 48000.0;
    }
    void setActiveEditor(IPlugView* v) SMTG_OVERRIDE { editor_ = v; }

    static const FUID uid;

private:
    void applyParams();
    void applyEvent(const Event& e);

    nf::Engine engine_;
    std::atomic<float> p_[nf::kNumParams];

    std::vector<float> outL_, outR_;
    double hostRate_ = 48000.0;

    IPlugView* editor_ = nullptr;
    bool prepared_ = false;
    bool initialized_ = false;
    float tempo_ = 0.0f;
};

const FUID NoiseForgeVST3::uid(0x4E464F52, 0x47455A31, 0xA1B2C3D4, 0x55667788);

// ---------------------------------------------------------------------------
NoiseForgeVST3::NoiseForgeVST3() {
    for (int i = 0; i < nf::kNumParams; ++i)
        p_[i].store(nf::kParams[i].defNorm, std::memory_order_relaxed);
}

tresult PLUGIN_API NoiseForgeVST3::initialize(FUnknown* context) {
    // A single-component effect is both processor and controller, so a host can
    // initialise the same object twice. The guard has to come *before* the base
    // call: ComponentBase::initialize() returns kResultFalse once hostContext is
    // set, and returning that to the host reads as "plugin failed to load".
    if (initialized_) return kResultOk;
    const tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;

    addAudioOutput(outputBusName(), SpeakerArr::kStereo);
    addEventInput(eventBusName(), 16);

    for (int i = 0; i < nf::kNumParams; ++i) {
        const nf::ParamMeta& m = nf::kParams[i];
        ParameterInfo info = {};
        info.id = (ParamID)i;
        widen(info.title, m.name, 128);
        widen(info.units, m.unit, 128);
        info.stepCount = m.steps;
        info.defaultNormalizedValue = m.defNorm;
        info.flags = ParameterInfo::kCanAutomate;
        if (i == nf::kBypass) info.flags |= ParameterInfo::kIsBypass;
        parameters.addParameter(info);
    }

    for (int i = 0; i < nf::kNumParams; ++i) {
        Parameter* prm = parameters.getParameter((ParamID)i);
        if (prm) prm->setNormalized(nf::kParams[i].defNorm);
    }

    initialized_ = true;
    return kResultOk;
}

tresult PLUGIN_API NoiseForgeVST3::terminate() {
    prepared_ = false;
    editor_ = nullptr;
    outL_.clear();
    outR_.clear();
    return SingleComponentEffect::terminate();
}

tresult PLUGIN_API NoiseForgeVST3::canProcessSampleSize(int32 n) {
    return (n == kSample32 || n == kSample64) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API NoiseForgeVST3::setupProcessing(ProcessSetup& s) {
    int32 maxBlock = s.maxSamplesPerBlock;
    if (maxBlock < 1) maxBlock = 1;
    if (maxBlock > (1 << 20)) maxBlock = 1 << 20;

    try {
        const size_t n = (size_t)maxBlock;
        outL_.assign(n, 0.0f);
        outR_.assign(n, 0.0f);
    } catch (...) {
        prepared_ = false;
        return kResultFalse;
    }

    hostRate_ = s.sampleRate;
    engine_.prepare(s.sampleRate);
    prepared_ = true;
    return SingleComponentEffect::setupProcessing(s);
}

tresult PLUGIN_API NoiseForgeVST3::setProcessing(TBool state) {
    if (state) applyParams();
    return kResultOk;
}

tresult PLUGIN_API NoiseForgeVST3::setActive(TBool state) {
    if (state) {
        engine_.reset();
        applyParams();
    }
    return SingleComponentEffect::setActive(state);
}

tresult PLUGIN_API NoiseForgeVST3::setParamNormalized(ParamID tag, ParamValue value) {
    if (tag < (ParamID)nf::kNumParams) {
        p_[tag].store((float)value, std::memory_order_relaxed);
        if (editor_) nf::notifyEditorParamChanged(editor_, (int)tag);
    }
    return SingleComponentEffect::setParamNormalized(tag, value);
}

tresult PLUGIN_API NoiseForgeVST3::getParamStringByValue(ParamID tag,
                                                         ParamValue value,
                                                         String128 string) {
    if (tag >= (ParamID)nf::kNumParams) return kResultFalse;
    char buf[64] = {};
    nf::formatParam((int)tag, (float)value, buf, (int)sizeof(buf));
    widen(string, buf, 128);
    return kResultOk;
}

IPlugView* PLUGIN_API NoiseForgeVST3::createView(FIDString name) {
    if (name && std::strcmp(name, "editor") != 0) return nullptr;
    return nf::createNoiseForgeEditor(this);
}

void NoiseForgeVST3::applyParams() {
    for (int i = 0; i < nf::kNumParams; ++i)
        engine_.setParam(i, p_[i].load(std::memory_order_relaxed));
}

void NoiseForgeVST3::applyEvent(const Event& e) {
    switch (e.type) {
    case Event::kNoteOnEvent:
        engine_.noteOn(e.noteOn.pitch, e.noteOn.velocity);
        break;
    case Event::kNoteOffEvent:
        engine_.noteOff(e.noteOff.pitch);
        break;
    case Event::kPolyPressureEvent:
        // Poly pressure has no per-voice target here, so it drives the global
        // aftertouch source. Channel pressure arrives the same way.
        engine_.setAftertouch(e.polyPressure.pressure);
        break;
    case Event::kDataEvent:
        break;
    default:
        break;
    }
}

tresult PLUGIN_API NoiseForgeVST3::process(ProcessData& data) {
    // ---- 1. automation -----------------------------------------------------
    if (data.inputParameterChanges) {
        const int32 n = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
            if (!q) continue;
            const int32 points = q->getPointCount();
            if (points <= 0) continue;
            ParamValue v = 0.0;
            int32 offset = 0;
            if (q->getPoint(points - 1, offset, v) == kResultOk) {
                const ParamID id = q->getParameterId();
                if (id < (ParamID)nf::kNumParams)
                    p_[id].store((float)v, std::memory_order_relaxed);
            }
        }
    }
    applyParams();

    // ---- 2. tempo (for the synced LFOs) ------------------------------------
    if (data.processContext &&
        (data.processContext->state & ProcessContext::kTempoValid)) {
        if (data.processContext->tempo != tempo_) {
            tempo_ = data.processContext->tempo;
            engine_.setTempo(tempo_);
        }
    }

    // ---- 3. structural sanity ---------------------------------------------
    if (data.numSamples <= 0) return kResultOk;
    if (!prepared_ || outL_.empty()) return kResultOk;
    if (data.numOutputs <= 0 || data.outputs[0].numChannels <= 0) return kResultOk;

    AudioBusBuffers& busOut = data.outputs[0];
    const int32 nCh = std::min<int32>(busOut.numChannels, 2);
    busOut.silenceFlags = 0;

    const bool is32 = (data.symbolicSampleSize == kSample32);
    const int32 maxChunk = (int32)outL_.size();
    const int32 total = data.numSamples;

    // ---- 4. render, chunked to the buffer actually allocated --------------
    int32 done = 0;
    auto renderTo = [&](int32 until) {
        if (until > total) until = total;
        while (done < until) {
            const int32 n = std::min(maxChunk, until - done);
            if (n <= 0) break;

            engine_.process(outL_.data(), outR_.data(), n);

            if (is32) {
                if (busOut.channelBuffers32[0]) {
                    for (int32 i = 0; i < n; ++i)
                        busOut.channelBuffers32[0][done + i] = outL_[i];
                }
                if (nCh > 1 && busOut.channelBuffers32[1]) {
                    for (int32 i = 0; i < n; ++i)
                        busOut.channelBuffers32[1][done + i] = outR_[i];
                }
            } else {
                if (busOut.channelBuffers64[0]) {
                    for (int32 i = 0; i < n; ++i)
                        busOut.channelBuffers64[0][done + i] = outL_[i];
                }
                if (nCh > 1 && busOut.channelBuffers64[1]) {
                    for (int32 i = 0; i < n; ++i)
                        busOut.channelBuffers64[1][done + i] = outR_[i];
                }
            }
            done += n;
        }
    };

    // ---- 5. notes, sample-accurate ----------------------------------------
    // Events carry a sample offset; applying them all at sample 0 shifts every
    // note by up to one block (21 ms at 1024 samples / 48 kHz).
    Event ev[96];
    int32 evN = 0;
    if (data.inputEvents) {
        const int32 count = data.inputEvents->getEventCount();
        for (int32 i = 0; i < count; ++i) {
            Event e = {};
            if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
            if (evN < (int32)(sizeof(ev) / sizeof(ev[0]))) {
                if (e.sampleOffset < 0) e.sampleOffset = 0;
                if (e.sampleOffset > total) e.sampleOffset = total;
                ev[evN++] = e;
            } else {
                applyEvent(e);   // pathological event count: apply immediately
            }
        }
        for (int32 i = 1; i < evN; ++i) {   // insertion sort, n is tiny
            const Event key = ev[i];
            int32 j = i - 1;
            while (j >= 0 && ev[j].sampleOffset > key.sampleOffset) {
                ev[j + 1] = ev[j];
                --j;
            }
            ev[j + 1] = key;
        }
    }
    for (int32 i = 0; i < evN; ++i) {
        renderTo(ev[i].sampleOffset);
        applyEvent(ev[i]);
    }
    renderTo(total);
    return kResultOk;
}

tresult PLUGIN_API NoiseForgeVST3::getState(IBStream* state) {
    if (!state) return kInvalidArgument;
    IBStreamer s(state, kLittleEndian);
    if (!s.writeInt32u(1)) return kResultFalse;   // format version
    for (int i = 0; i < nf::kNumParams; ++i) {
        if (!s.writeFloat(p_[i].load(std::memory_order_relaxed)))
            return kResultFalse;
    }
    // The wavetable seed travels with the project: a patch built on a random
    // bank has to come back with the same bank, or it is not the same patch.
    if (!s.writeInt32u(engine_.tableSeed())) return kResultFalse;
    return kResultOk;
}

tresult PLUGIN_API NoiseForgeVST3::setState(IBStream* state) {
    if (!state) return kInvalidArgument;
    IBStreamer s(state, kLittleEndian);
    uint32 version = 0;
    if (!s.readInt32u(version)) return kResultFalse;
    if (version < 1 || version > 1) return kResultFalse;

    for (int i = 0; i < nf::kNumParams; ++i) {
        float v = 0.0f;
        if (!s.readFloat(v)) return kResultFalse;
        if (!(v == v)) v = nf::kParams[i].defNorm;   // reject NaN
        v = nf::clampf(v, 0.0f, 1.0f);
        p_[i].store(v, std::memory_order_relaxed);
        Parameter* prm = parameters.getParameter((ParamID)i);
        if (prm) prm->setNormalized(v);
        if (editor_) nf::notifyEditorParamChanged(editor_, i);
    }

    uint32 seed = 0;
    if (s.readInt32u(seed)) engine_.setTableSeed(seed);
    applyParams();
    return kResultOk;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
BEGIN_FACTORY_DEF("NoiseForge Labs",
                  "https://example.invalid/noiseforge",
                  "nobody@example.invalid")

    DEF_CLASS2(INLINE_UID_FROM_FUID(NoiseForgeVST3::uid),
               PClassInfo::kManyInstances,
               kVstAudioEffectClass,
               "NoiseForge",
               Vst::kDistributable,
               Vst::PlugType::kInstrumentSynth,
               "1.0.0",
               kVstVersionString,
               NoiseForgeVST3::createInstance)

END_FACTORY
