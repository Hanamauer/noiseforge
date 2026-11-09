// ===========================================================================
//  NoiseForgeCore.h -- NOISEFORGE, a noise instrument
//
//  TWO SEPARATE NOISE ENGINES (deliberately independent):
//
//    HARSH   broadband "coarse" noise: white / pink / brown / blue / violet /
//            velvet / crackle / digital, then a resonant tone stage, a wave
//            folder, a bit crusher and a sample-rate reducer.  This is the
//            ugly, torn one.
//
//    BAND    band-limited noise built by *direct spectral synthesis*: a bank of
//            randomly placed sine partials inside an arbitrary frequency band.
//            Because it is synthesis and not filtering, the band can sit
//            entirely below 20 Hz (0.05 Hz .. 20 Hz) with no settling time and
//            no DC drift.  It can also be a 1/50-octave whistle or a
//            full-spectrum wash.
//
//  Both can run at once, each with its own level, pan and modulation targets.
//
//  THE WAVETABLE OSCILLATOR is a third source.  Eight tables, each with eight
//  morph frames and four band-limited mip levels, are *generated at runtime*
//  from harmonic spectra -- the same idea as Vital's WaveSource plus Fourier
//  frames.  Two tables are fully random per seed and the whole bank can be
//  regenerated ("DICE") without disturbing the audio thread's copy.
//
//  MIDI: note-on selects pitch (all three sources key-track), note-off starts
//  the release stage.  There is no instant on/off: the envelope has at least an
//  8 ms attack and a 12 ms release, and voice stealing fades out over 40 ms, so
//  a note can never produce a click.
//
//  THREADING
//    audio thread -> process(), noteOn(), noteOff(), setParam()   (no alloc)
//    UI thread    -> randomizeTables(), copyScope(), takePeak()
//  The only cross-thread state is a wavetable double-buffer (atomic index), the
//  parameter atomics and the scope ring.
// ===========================================================================

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nf {

// A 4096-point sine, interpolated. Everything that needs a sine (partials,
// wavetable synthesis, the LFOs) reads this instead of calling std::sin in the
// audio loop.
extern float g_sin[4097];

inline float sinT(float ph) {
    ph -= std::floor(ph);
    const float x = ph * 4096.0f;
    int i = (int)x;
    if (i < 0) i = 0;
    if (i > 4095) i = 4095;
    const float f = x - (float)i;
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f;
}

// ---------------------------------------------------------------------------
//  Parameters
// ---------------------------------------------------------------------------
enum ParamId : int {
    // VOICE
    kGlide = 0, kVelToAmp, kVelToEnv, kOctave,
    // ENVELOPES
    kAttack, kDecay, kSustain, kRelease,
    kEnv2Attack, kEnv2Decay, kEnv2Amount, kGateLen,
    // WAVETABLE OSCILLATOR
    kOscOn, kWtTable, kWtPos, kWtLevel, kWtPitch, kWtFine, kWtKey, kWtPan,
    kWtDrift,
    // HARSH NOISE
    kHarshOn, kHarshType, kHarshLevel, kHarshColor, kHarshCut, kHarshReso,
    kHarshDrive, kHarshFold, kHarshBits, kHarshDown, kHarshPan,
    // BAND-LIMITED NOISE
    kBandOn, kBandCenter, kBandWidth, kBandLevel, kBandPartials, kBandKey,
    kBandDrift, kBandPan,
    // MODULATION
    kLfo1Rate, kLfo1Shape, kLfo1Sync, kLfo2Rate, kLfo2Shape, kLfo2Sync,
    kRandRate, kRandSmooth,
    kMod1Src, kMod1Dst, kMod1Amt,
    kMod2Src, kMod2Dst, kMod2Amt,
    kMod3Src, kMod3Dst, kMod3Amt,
    kMod4Src, kMod4Dst, kMod4Amt,
    kMod5Src, kMod5Dst, kMod5Amt,
    kMod6Src, kMod6Dst, kMod6Amt,
    // OUTPUT
    kDrive, kWidth, kOutput, kCeiling, kBypass,
    kNumParams
};

enum ParamKind : int { kKnob = 0, kSwitch = 1, kChoice = 2 };

enum GroupId : int {
    kGrpVoice = 0, kGrpEnv, kGrpWt, kGrpHarsh, kGrpBand, kGrpMod, kGrpOut,
    kNumGroups
};

struct ParamMeta {
    const char* label;
    const char* name;
    const char* unit;
    float       lo;
    float       hi;
    int         kind;
    int         steps;
    float       defNorm;
    bool        logScale;
    int         group;
    const char* choices;   // '|' separated, only for kChoice
};

extern const ParamMeta kParams[kNumParams];
extern const char* const kGroupTitles[kNumGroups];

float mapParam(int id, float norm);          // norm -> real value
float normForValue(int id, float value);     // real value -> norm
void  formatParam(int id, float norm, char* out, int cap);
const char* choiceText(int id, int index);

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
//  Modulation sources / destinations
// ---------------------------------------------------------------------------
enum ModSource : int {
    kSrcOff = 0, kSrcEnv1, kSrcEnv2, kSrcLfo1, kSrcLfo2, kSrcRand,
    kSrcVel, kSrcKey, kSrcGate, kSrcAfter,
    kNumSrc
};

enum ModDest : int {
    kDstOff = 0, kDstPitch, kDstWtPos, kDstBandCenter, kDstBandWidth,
    kDstHarshCut, kDstHarshDrive, kDstCrush, kDstPan, kDstLevel,
    kNumDst
};

extern const char* const kModSrcNames[kNumSrc];
extern const char* const kModDstNames[kNumDst];

// ---------------------------------------------------------------------------
enum {
    kMaxVoices   = 8,
    kMaxPartials = 96,
    kTables      = 8,
    kFrames      = 8,
    kMips        = 4,
    kTableSize   = 1024,
    kScopeSize   = 1024,
    kControlRate = 32
};

// ---------------------------------------------------------------------------
// xorshift128+, seeded per voice / per table build. Cheap and good enough for
// audio-rate noise; the whole point is that nothing ever repeats.
struct Rng {
    uint64_t s0 = 0x9E3779B97F4A7C15ull, s1 = 0xBF58476D1CE4E5B9ull;
    void seed(uint64_t a, uint64_t b) {
        s0 = a ? a : 0x9E3779B97F4A7C15ull;
        s1 = b ? b : 0xBF58476D1CE4E5B9ull;
        for (int i = 0; i < 4; ++i) next();
    }
    inline uint64_t next() {
        uint64_t x = s0;
        const uint64_t y = s1;
        s0 = y;
        x ^= x << 23;
        s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s1 + y;
    }
    inline float uni() { return (float)((next() >> 40) * (1.0 / 16777216.0)); }
    inline float bi() { return uni() * 2.0f - 1.0f; }
};

// ---------------------------------------------------------------------------
class Engine {
public:
    Engine();

    void prepare(double sampleRate);
    void reset();
    void setTempo(float bpm) { bpm_ = bpm > 1.0f ? bpm : 120.0f; }

    // Audio-thread entry points. No allocation, no locks, no I/O.
    void process(float* outL, float* outR, int n);
    void processSilent(int n);
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();
    void setAftertouch(float v) { after_ = clampf(v, 0.0f, 1.0f); }

    void setParam(int id, float norm);

    // UI thread: rebuild the wavetable bank with a fresh seed and swap it in.
    // Returns false when the audio thread is still inside the previous bank;
    // the caller should retry on its next timer tick.
    bool randomizeTables();
    uint32_t tableSeed() const { return seed_.load(std::memory_order_relaxed); }
    // Rebuild the bank from a stored seed (state restore). Not audio thread.
    void setTableSeed(uint32_t seed);
    const char* tableName(int t) const;

    // Display, safe from the UI thread.
    void copyScope(float* dst, int n) const;
    // One wavetable frame (interpolated at pos 0..1, mip 0) resampled to n
    // points. Used only by the editor's table preview.
    void copyTablePreview(int table, float pos, float* dst, int n) const;
    float peak() const { return peak_.load(std::memory_order_relaxed); }
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }
    uint32_t scopeGeneration() const { return scopeW_.load(std::memory_order_relaxed); }
    int  activeVoices() const { return active_.load(std::memory_order_relaxed); }
    double sampleRate() const { return sr_; }

private:
    // ---- envelope ---------------------------------------------------------
    struct Env {
        enum { kIdle = 0, kAttack, kDecay, kSustain, kRelease };
        int   stage = kIdle;
        float level = 0.0f, sustain = 0.7f, relFrom = 1.0f;
        float aInc = 0.001f;     // linear attack: 0 -> 1 in exactly `a` seconds
        float dCoef = 0.001f;    // exponential decay toward sustain
        float rCoef = 0.001f;    // exponential release toward zero
        void setTimes(double sr, float a, float d, float s, float r);
        inline void on() { stage = kAttack; }
        inline void off() {
            if (stage != kIdle) {
                stage = kRelease;
                relFrom = level > 0.0005f ? level : 0.0005f;
            }
        }
        inline float tick() {
            switch (stage) {
            case kAttack:
                // Linear, so the attack parameter means what it says and the
                // first sample can never jump further than one step.
                level += aInc;
                if (level >= 1.0f) { level = 1.0f; stage = kDecay; }
                break;
            case kDecay:
                level += dCoef * (sustain - level);
                if (std::fabs(level - sustain) < 0.002f) {
                    level = sustain;
                    stage = kSustain;
                }
                break;
            case kSustain:
                level = sustain;
                break;
            case kRelease:
                level -= rCoef * level;
                if (level < relFrom * 0.0005f || level < 0.00002f) {
                    level = 0.0f;
                    stage = kIdle;
                }
                break;
            default:
                level = 0.0f;
                break;
            }
            return level;
        }
    };

    // ---- filters ----------------------------------------------------------
    struct Svf {   // TPT state-variable, stable at any cutoff
        float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
        void  set(double sr, float hz, float q);
        inline float bp(float x) {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return v1;
        }
        inline float lp(float x) {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return v2;
        }
        void clear() { ic1 = ic2 = 0; }
    };

    struct OnePole {
        float a = 0.0f, z = 0.0f;
        inline void set(double sr, float hz) {
            a = 1.0f - std::exp(-6.28318530718f * hz / (float)sr);
            a = clampf(a, 0.0f, 1.0f);
        }
        inline float lp(float x) { z += a * (x - z); return z; }
        inline void clear() { z = 0.0f; }
    };

    // ---- band-limited spectral noise --------------------------------------
    struct Spectral {
        struct P { float phase, incr, base, amp, target; };
        P p[kMaxPartials];
        int n = 0;
        float norm = 1.0f;
        float refresh = 0.0f, refreshPeriod = 0.7f;

        // keepPhase: keep the running phase and amplitude of every partial that
        // already exists, so a band that moves is retuned instead of restarted.
        void rebuild(Rng& rng, double sr, float loHz, float hiHz, int count,
                     bool keepPhase);
        void setScale(double sr, float s);
        void retarget(Rng& rng);
        void slewAmps(float coef);
        inline float next() {
            float s = 0.0f;
            for (int i = 0; i < n; ++i) {
                P& q = p[i];
                s += q.amp * sinT(q.phase);
                q.phase += q.incr;
                if (q.phase >= 1.0f) q.phase -= 1.0f;
            }
            return s * norm;
        }
    };

    // ---- wavetable bank ---------------------------------------------------
    struct Bank {
        std::vector<float> d;          // kTables*kFrames*kMips*kTableSize
        char name[kTables][24];
    };
    void buildBank(Bank& b, uint32_t seed);
    inline const float* tablePtr(const Bank& b, int t, int f, int m) const {
        return &b.d[(size_t)(((t * kFrames + f) * kMips + m) * kTableSize)];
    }

    // ---- voice ------------------------------------------------------------
    struct Voice {
        bool  active = false, held = false, gated = false;
        bool  stealing = false;
        int   note = 60;
        float vel = 0.8f;
        float gateTimer = 0.0f;

        double phase = 0.0, incr = 0.0;
        float  glideCur = 0.0f, glideTarget = 0.0f;

        Env   env, env2;
        Rng   rng;

        // harsh noise state
        float pink[7] = {}, brown = 0.0f, lastWhite = 0.0f, prevDiff = 0.0f;
        float holdVal = 0.0f, crackEnv = 0.0f, crackSign = 1.0f;
        int   holdCount = 0, dsCount = 0;
        float crushHold = 0.0f;
        Svf   hf1, hf2;
        OnePole tilt;

        // band noise state
        Spectral band;
        bool  bandInit = false;
        float bandLo = 0.0f, bandHi = 0.0f;
        int   bandCount = 0;
        float bandPan = 0.5f, bLevel = 0.0f;

        // modulation state
        float lfo1Phase = 0.0f, lfo2Phase = 0.0f;
        float lfo1SH = 0.0f, lfo2SH = 0.0f;
        float lfo1Next = 0.0f, lfo2Next = 0.0f;
        float lfo1 = 0.0f, lfo2 = 0.0f;
        float randPhase = 0.0f, randVal = 0.0f, randTarget = 0.0f;
        float driftWt = 0.0f, driftBand = 0.0f;
        float gateVal = 0.0f;

        // per-voice control-rate values
        float dPitch = 0.0f, dWtPos = 0.0f, dBandC = 0.0f, dBandW = 0.0f;
        float dCut = 0.0f, dDrive = 0.0f, dCrush = 0.0f, dPan = 0.0f, dLevel = 0.0f;
        float wtFrame = 0.0f, wtDrift = 0.0f, wtLevel = 0.0f, wtPan = 0.5f;
        int   table = 0;
        float harshType = 0.0f;
        float hCut = 1000.0f, hReso = 0.3f, hDrive = 0.0f, hFold = 0.0f;
        float hBits = 16.0f, hDown = 1.0f, hColor = 0.5f, hLevel = 0.0f, hPan = 0.5f;
        float ampGain = 1.0f, velGain = 1.0f, env2Amt = 0.0f, crushAmt = 0.0f;

        float steal = 1.0f;
        int   pendingNote = 60;
        float pendingVel = 0.8f;
        uint32_t age = 0;
    };

    // ---- helpers ----------------------------------------------------------
    void  controlTick();
    float harshSample(Voice& v);
    float bandSample(Voice& v) { return v.band.next(); }
    float oscSample(Voice& v, const Bank& b);
    static float lfoValue(int shape, float phase, float sh, float next);
    int   allocVoice(int note);
    bool  swapAllowed() const;

    // ---- state ------------------------------------------------------------
    double sr_ = 48000.0;
    float  bpm_ = 120.0f;
    Rng    rng_;
    uint32_t seedCounter_ = 2;
    std::atomic<uint32_t> seed_{1};
    std::atomic<int>  bankIndex_{0};
    std::atomic<uint32_t> blockCount_{0};
    std::atomic<uint32_t> swapBlock_{0};
    std::atomic<uint64_t> lastBlockNs_{0};
    Bank   bank_[2];
    uint32_t voiceAge_ = 0;

    Voice voices_[kMaxVoices];

    std::atomic<float> p_[kNumParams];
    float pv_[kNumParams] = {};

    // smoothed globals (control rate)
    float smDrive_ = 0.0f, smWidth_ = 1.0f, smOut_ = 0.8f, smCeil_ = 0.95f;
    float tDrive_ = 0.0f, tWidth_ = 1.0f, tOut_ = 0.8f, tCeil_ = 0.95f;
    float masterFade_ = 0.0f;
    float bypassGain_ = 1.0f;   // ramped, so bypass never clicks

    float after_ = 0.0f;
    double dcL_ = 0.0, dcR_ = 0.0, dcInL_ = 0.0, dcInR_ = 0.0;

    int   controlCounter_ = 0;
    int   modSrc_[6] = {}, modDst_[6] = {};
    float modAmt_[6] = {};

    // display
    std::atomic<float>    scope_[kScopeSize];
    std::atomic<uint32_t> scopeW_{0};
    std::atomic<float>    peak_{0.0f};
    std::atomic<int>      active_{0};
};

} // namespace nf
