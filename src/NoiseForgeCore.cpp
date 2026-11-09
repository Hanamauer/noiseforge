// ===========================================================================
//  NoiseForgeCore.cpp
// ===========================================================================

#include "NoiseForgeCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace nf {

float g_sin[4097];

static void initSinTable() {
    static bool done = false;
    if (done) return;
    for (int i = 0; i <= 4096; ++i)
        g_sin[i] = (float)std::sin(6.28318530717958647692 * (double)i / 4096.0);
    done = true;
}

// ---------------------------------------------------------------------------
//  Parameter table
// ---------------------------------------------------------------------------
static const char* kHarshTypes =
    "WHITE|PINK|BROWN|BLUE|VIOLET|VELVET|CRACKLE|DIGITAL";
static const char* kWtNames =
    "SAW|PULSE|RIP|FORMANT|METAL|HARMONIC|RANDOM|VOID";
static const char* kLfoShapes = "SINE|TRI|SAW|SQUARE|S&H|SMOOTH";
static const char* kSrcNames =
    "OFF|ENV1|ENV2|LFO1|LFO2|RAND|VEL|KEY|GATE|AFTER";
static const char* kDstNames =
    "OFF|PITCH|WT POS|BAND C|BAND W|HARSH CUT|HARSH DRV|CRUSH|PAN|LEVEL";

const char* const kModSrcNames[kNumSrc] = {
    "OFF", "ENV1", "ENV2", "LFO1", "LFO2", "RAND", "VEL", "KEY", "GATE", "AFTER"};
const char* const kModDstNames[kNumDst] = {
    "OFF", "PITCH", "WT POS", "BAND C", "BAND W", "HARSH CUT", "HARSH DRV",
    "CRUSH", "PAN", "LEVEL"};

const char* const kGroupTitles[kNumGroups] = {
    "VOICE", "ENVELOPES", "WAVETABLE OSC", "HARSH NOISE", "BAND NOISE",
    "MODULATION", "OUTPUT"};

// clang-format off
const ParamMeta kParams[kNumParams] = {
//   label          name               unit    lo        hi      kind      st  def      log grp        choices
    {"GLIDE",      "Glide",           "s",    0.0f,     2.0f,   kKnob,    0,  0.00f,  0, kGrpVoice, nullptr},
    {"VEL>AMP",    "Velocity to Amp", "",     0.0f,     1.0f,   kKnob,    0,  0.60f,  0, kGrpVoice, nullptr},
    {"VEL>ENV",    "Velocity to Env", "",     0.0f,     1.0f,   kKnob,    0,  0.30f,  0, kGrpVoice, nullptr},
    {"OCTAVE",     "Octave",          "",    -3.0f,     3.0f,   kKnob,    6,  0.50f,  0, kGrpVoice, nullptr},

    {"ATTACK",     "Attack",          "s",    0.002f,  10.0f,   kKnob,    0,  0.352f, 1, kGrpEnv,   nullptr},
    {"DECAY",      "Decay",           "s",    0.005f,  10.0f,   kKnob,    0,  0.60f,  1, kGrpEnv,   nullptr},
    {"SUSTAIN",    "Sustain",         "",     0.0f,     1.0f,   kKnob,    0,  0.70f,  0, kGrpEnv,   nullptr},
    {"RELEASE",    "Release",         "s",    0.012f,  12.0f,   kKnob,    0,  0.42f,  1, kGrpEnv,   nullptr},
    {"ENV2 A",     "Env2 Attack",     "s",    0.002f,   5.0f,   kKnob,    0,  0.45f,  1, kGrpEnv,   nullptr},
    {"ENV2 D",     "Env2 Decay",      "s",    0.005f,   5.0f,   kKnob,    0,  0.55f,  1, kGrpEnv,   nullptr},
    {"ENV2 AMT",   "Env2 Amount",     "",    -1.0f,     1.0f,   kKnob,    0,  0.65f,  0, kGrpEnv,   nullptr},
    {"GATE",       "Gate Length",     "s",    0.0f,     8.0f,   kKnob,    0,  0.00f,  0, kGrpEnv,   nullptr},

    {"WT ON",      "Wavetable On",    "",     0.0f,     1.0f,   kSwitch,  1,  1.00f,  0, kGrpWt,    nullptr},
    {"TABLE",      "Wavetable",       "",     0.0f,     7.0f,   kChoice,  7,  0.00f,  0, kGrpWt,    kWtNames},
    {"POSITION",   "Table Position",  "",     0.0f,     1.0f,   kKnob,    0,  0.35f,  0, kGrpWt,    nullptr},
    {"WT LEVEL",   "Wavetable Level", "",     0.0f,     1.0f,   kKnob,    0,  0.55f,  0, kGrpWt,    nullptr},
    {"PITCH",      "Pitch",           "st",  -24.0f,    24.0f,  kKnob,    0,  0.50f,  0, kGrpWt,    nullptr},
    {"FINE",       "Fine",            "ct", -100.0f,   100.0f,  kKnob,    0,  0.50f,  0, kGrpWt,    nullptr},
    {"WT KEY",     "Keytrack",        "",     0.0f,     1.0f,   kKnob,    0,  1.00f,  0, kGrpWt,    nullptr},
    {"WT PAN",     "Wavetable Pan",   "",     0.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpWt,    nullptr},
    {"WT DRIFT",   "Table Drift",     "",     0.0f,     1.0f,   kKnob,    0,  0.15f,  0, kGrpWt,    nullptr},

    {"HARSH ON",   "Harsh On",        "",     0.0f,     1.0f,   kSwitch,  1,  1.00f,  0, kGrpHarsh, nullptr},
    {"TYPE",       "Noise Type",      "",     0.0f,     7.0f,   kChoice,  7,  0.00f,  0, kGrpHarsh, kHarshTypes},
    {"LEVEL",      "Harsh Level",     "",     0.0f,     1.0f,   kKnob,    0,  0.45f,  0, kGrpHarsh, nullptr},
    {"COLOR",      "Colour",          "",     0.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpHarsh, nullptr},
    {"CUTOFF",     "Harsh Cutoff",    "Hz",  20.0f, 18000.0f,   kKnob,    0,  0.50f,  1, kGrpHarsh, nullptr},
    {"RESO",       "Resonance",       "",     0.0f,     1.0f,   kKnob,    0,  0.35f,  0, kGrpHarsh, nullptr},
    {"DRIVE",      "Drive",           "",     0.0f,     1.0f,   kKnob,    0,  0.30f,  0, kGrpHarsh, nullptr},
    {"FOLD",       "Wavefold",        "",     0.0f,     1.0f,   kKnob,    0,  0.10f,  0, kGrpHarsh, nullptr},
    {"BITS",       "Bits",            "bit",  1.0f,    16.0f,   kKnob,   15,  1.00f,  0, kGrpHarsh, nullptr},
    {"DOWN",       "Downsample",      "x",    1.0f,    64.0f,   kKnob,    0,  0.00f,  1, kGrpHarsh, nullptr},
    {"HARSH PAN",  "Harsh Pan",       "",     0.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpHarsh, nullptr},

    {"BAND ON",    "Band On",         "",     0.0f,     1.0f,   kSwitch,  1,  0.00f,  0, kGrpBand,  nullptr},
    {"CENTRE",     "Band Centre",     "Hz",   0.05f, 20000.0f,  kKnob,    0,  0.75f,  1, kGrpBand,  nullptr},
    {"WIDTH",      "Band Width",      "oct",  0.02f,     8.0f,  kKnob,    0,  0.65f,  1, kGrpBand,  nullptr},
    {"BAND LVL",   "Band Level",      "",     0.0f,     1.0f,   kKnob,    0,  0.55f,  0, kGrpBand,  nullptr},
    {"PARTIALS",   "Partials",        "",     1.0f,    96.0f,   kKnob,   95,  0.30f,  0, kGrpBand,  nullptr},
    {"BAND KEY",   "Band Keytrack",   "",     0.0f,     1.0f,   kKnob,    0,  1.00f,  0, kGrpBand,  nullptr},
    {"BAND DRIFT", "Band Drift",      "",     0.0f,     1.0f,   kKnob,    0,  0.25f,  0, kGrpBand,  nullptr},
    {"BAND PAN",   "Band Pan",        "",     0.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpBand,  nullptr},

    {"LFO1 RATE",  "LFO 1 Rate",      "Hz",  0.01f,    40.0f,   kKnob,    0,  0.55f,  1, kGrpMod,   nullptr},
    {"LFO1 SHAPE", "LFO 1 Shape",     "",     0.0f,     5.0f,   kChoice,  5,  0.00f,  0, kGrpMod,   kLfoShapes},
    {"LFO1 SYNC",  "LFO 1 Sync",      "",     0.0f,     1.0f,   kSwitch,  1,  0.00f,  0, kGrpMod,   nullptr},
    {"LFO2 RATE",  "LFO 2 Rate",      "Hz",  0.01f,    40.0f,   kKnob,    0,  0.70f,  1, kGrpMod,   nullptr},
    {"LFO2 SHAPE", "LFO 2 Shape",     "",     0.0f,     5.0f,   kChoice,  5,  0.20f,  0, kGrpMod,   kLfoShapes},
    {"LFO2 SYNC",  "LFO 2 Sync",      "",     0.0f,     1.0f,   kSwitch,  1,  0.00f,  0, kGrpMod,   nullptr},
    {"RAND RATE",  "Random Rate",     "Hz",  0.01f,    40.0f,   kKnob,    0,  0.50f,  1, kGrpMod,   nullptr},
    {"RAND SMOOTH","Random Smooth",   "",     0.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpMod,   nullptr},

    {"SRC 1",      "Mod 1 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.30f,  0, kGrpMod,   kSrcNames},
    {"DST 1",      "Mod 1 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.30f,  0, kGrpMod,   kDstNames},
    {"AMT 1",      "Mod 1 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.625f, 0, kGrpMod,   nullptr},
    {"SRC 2",      "Mod 2 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.50f,  0, kGrpMod,   kSrcNames},
    {"DST 2",      "Mod 2 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.60f,  0, kGrpMod,   kDstNames},
    {"AMT 2",      "Mod 2 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.60f,  0, kGrpMod,   nullptr},
    {"SRC 3",      "Mod 3 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kSrcNames},
    {"DST 3",      "Mod 3 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kDstNames},
    {"AMT 3",      "Mod 3 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpMod,   nullptr},
    {"SRC 4",      "Mod 4 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kSrcNames},
    {"DST 4",      "Mod 4 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kDstNames},
    {"AMT 4",      "Mod 4 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpMod,   nullptr},
    {"SRC 5",      "Mod 5 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kSrcNames},
    {"DST 5",      "Mod 5 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kDstNames},
    {"AMT 5",      "Mod 5 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpMod,   nullptr},
    {"SRC 6",      "Mod 6 Source",    "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kSrcNames},
    {"DST 6",      "Mod 6 Dest",      "",     0.0f,     9.0f,   kChoice,  9,  0.00f,  0, kGrpMod,   kDstNames},
    {"AMT 6",      "Mod 6 Amount",    "",    -1.0f,     1.0f,   kKnob,    0,  0.50f,  0, kGrpMod,   nullptr},

    {"DRIVE",      "Master Drive",    "",     0.0f,     1.0f,   kKnob,    0,  0.25f,  0, kGrpOut,   nullptr},
    {"WIDTH",      "Stereo Width",    "",     0.0f,     2.0f,   kKnob,    0,  0.50f,  0, kGrpOut,   nullptr},
    {"OUTPUT",     "Output",          "",     0.0f,     2.0f,   kKnob,    0,  0.40f,  0, kGrpOut,   nullptr},
    {"CEILING",    "Ceiling",         "",     0.05f,    1.0f,   kKnob,    0,  0.947f, 0, kGrpOut,   nullptr},
    {"BYPASS",     "Bypass",          "",     0.0f,     1.0f,   kSwitch,  1,  0.00f,  0, kGrpOut,   nullptr},
};
// clang-format on

// ---------------------------------------------------------------------------
float mapParam(int id, float norm) {
    const ParamMeta& m = kParams[id];
    norm = clampf(norm, 0.0f, 1.0f);
    if (m.kind == kChoice) return std::floor(norm * (m.hi - m.lo) + 0.5f) + m.lo;
    if (m.kind == kSwitch) return norm >= 0.5f ? 1.0f : 0.0f;

    if (id == kGateLen) {
        if (norm < 0.015f) return 0.0f;
        return 0.02f * std::pow(400.0f, (norm - 0.015f) / 0.985f);
    }
    // Knobs that declare steps are steppers (OCTAVE, BITS, PARTIALS). Snapping
    // here keeps the readout and the actual value in agreement.
    if (m.steps > 0) {
        const float st = (m.hi - m.lo) / (float)m.steps;
        const float v = m.lo + (m.hi - m.lo) * norm;
        return clampf(std::floor(v / st + 0.5f) * st, m.lo, m.hi);
    }
    if (m.logScale && m.lo > 0.0f)
        return m.lo * std::pow(m.hi / m.lo, norm);
    return m.lo + (m.hi - m.lo) * norm;
}

float normForValue(int id, float value) {
    const ParamMeta& m = kParams[id];
    if (m.kind == kChoice) {
        const float v = (value - m.lo) / (m.hi - m.lo);
        return clampf(v, 0.0f, 1.0f);
    }
    if (m.kind == kSwitch) return value >= 0.5f ? 1.0f : 0.0f;
    if (id == kGateLen) {
        if (value <= 0.0001f) return 0.0f;
        return 0.015f + 0.985f * (std::log(value / 0.02f) / std::log(400.0f));
    }
    if (m.logScale && m.lo > 0.0f)
        return clampf(std::log(value / m.lo) / std::log(m.hi / m.lo), 0.0f, 1.0f);
    return clampf((value - m.lo) / (m.hi - m.lo), 0.0f, 1.0f);
}

const char* choiceText(int id, int index) {
    const char* s = kParams[id].choices;
    if (!s) return "";
    int n = 0;
    const char* p = s;
    while (*p) {
        if (n == index) return p;
        if (*p == '|') ++n;
        ++p;
    }
    return s;
}

void formatParam(int id, float norm, char* out, int cap) {
    if (cap <= 0) return;
    const ParamMeta& m = kParams[id];
    if (m.kind == kChoice) {
        const char* t = choiceText(id, (int)mapParam(id, norm));
        int i = 0;
        for (; t[i] && t[i] != '|' && i < cap - 1; ++i) out[i] = t[i];
        out[i] = 0;
        return;
    }
    if (m.kind == kSwitch) {
        std::snprintf(out, (size_t)cap, "%s", norm >= 0.5f ? "ON" : "OFF");
        return;
    }
    const float v = mapParam(id, norm);
    const char* u = m.unit;
    if (id == kGateLen) {
        if (v <= 0.0001f) std::snprintf(out, (size_t)cap, "OFF");
        else if (v < 1.0f) std::snprintf(out, (size_t)cap, "%.0f ms", v * 1000.0f);
        else std::snprintf(out, (size_t)cap, "%.2f s", v);
        return;
    }
    if (id == kHarshBits || id == kBandPartials) {
        std::snprintf(out, (size_t)cap, "%d", (int)(v + 0.5f));
        return;
    }
    if (id == kOctave) {
        const int o = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
        std::snprintf(out, (size_t)cap, "%+d", o);
        return;
    }
    if (id == kWtPitch) {
        std::snprintf(out, (size_t)cap, "%+.1f %s", v, u);
        return;
    }
    if (u && std::strcmp(u, "Hz") == 0) {
        if (v < 10.0f) std::snprintf(out, (size_t)cap, "%.2f Hz", v);
        else if (v < 1000.0f) std::snprintf(out, (size_t)cap, "%.1f Hz", v);
        else std::snprintf(out, (size_t)cap, "%.2f kHz", v / 1000.0f);
        return;
    }
    if (u && std::strcmp(u, "s") == 0) {
        if (v < 1.0f) std::snprintf(out, (size_t)cap, "%.0f ms", v * 1000.0f);
        else std::snprintf(out, (size_t)cap, "%.2f s", v);
        return;
    }
    if (u && std::strcmp(u, "oct") == 0) {
        std::snprintf(out, (size_t)cap, "%.2f oct", v);
        return;
    }
    if (u && std::strcmp(u, "x") == 0) {
        std::snprintf(out, (size_t)cap, "%.1fx", v);
        return;
    }
    if (u && u[0]) std::snprintf(out, (size_t)cap, "%.1f %s", v, u);
    else std::snprintf(out, (size_t)cap, "%.2f", v);
}

// ---------------------------------------------------------------------------
//  Envelope
// ---------------------------------------------------------------------------
void Engine::Env::setTimes(double sr, float a, float d, float s, float r) {
    // A hard floor on attack and release is the whole point: a note is never
    // allowed to switch on or off instantly.
    a = clampf(a, 0.008f, 60.0f);
    d = clampf(d, 0.005f, 60.0f);
    r = clampf(r, 0.012f, 60.0f);

    // Linear attack: the exact number of samples it takes to cross 1.0.
    const float aSamples = a * (float)sr;
    aInc = aSamples > 0.5f ? 1.0f / aSamples : 1.0f;

    // Exponential decay/release, defined as reaching 1/1000 of the distance in
    // the nominal time (a 60 dB definition, which is how it is heard).
    //
    // The release coefficient carries a 6% margin so the parameter is a true
    // minimum: with an exact 60 dB definition the tail is already below the
    // audibility threshold one sample *before* the nominal time, which makes a
    // 12 ms release measure as 11.98 ms. The envelope is also only zeroed at
    // -66 dB, so it fades rather than stopping.
    dCoef = 1.0f - std::exp(-6.9077553f / (d * (float)sr));
    rCoef = 1.0f - std::exp(-6.9077553f / (r * 1.06f * (float)sr));
    sustain = clampf(s, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
void Engine::Svf::set(double sr, float hz, float q) {
    hz = clampf(hz, 1.0f, (float)sr * 0.45f);
    const float w = 6.28318530718f * hz / (float)sr;
    const float sn = std::sin(w * 0.5f);
    g = sn * sn;
    k = 1.0f / clampf(q, 0.3f, 40.0f);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
}

// ---------------------------------------------------------------------------
//  Spectral band noise
// ---------------------------------------------------------------------------
void Engine::Spectral::rebuild(Rng& rng, double sr, float loHz, float hiHz,
                               int count, bool keepPhase) {
    const int oldN = n;
    n = count < 1 ? 1 : (count > kMaxPartials ? kMaxPartials : count);
    const float ratio = hiHz / loHz;
    for (int i = 0; i < n; ++i) {
        // Partials are spread over the band with a jitter so the spectrum never
        // looks like a comb; amplitudes are skewed toward a few loud partials.
        const float t = ((float)i + rng.uni() * 0.9f) / (float)n;
        const float f = loHz * std::pow(ratio, t);
        P& q = p[i];
        q.base = f;
        q.incr = f / (float)sr;
        q.target = std::pow(rng.uni(), 1.35f) * 1.6f + 0.10f;
        // Restarting a partial that is already sounding is a step in the
        // waveform. Retune it instead; only new partials start from scratch,
        // and they fade in through slewAmps().
        if (!keepPhase) {
            q.phase = rng.uni();
            q.amp = q.target;
        } else if (i >= oldN) {
            q.phase = rng.uni();
            q.amp = 0.0f;
        }
    }
    // Equal-ish loudness regardless of partial count: sum of n random phases
    // grows like sqrt(n).
    norm = 1.75f / std::sqrt((float)n);
    refresh = 0.0f;
    refreshPeriod = 0.35f + rng.uni() * 1.6f;
}

void Engine::Spectral::setScale(double sr, float s) {
    for (int i = 0; i < n; ++i) p[i].incr = p[i].base * s / (float)sr;
}

void Engine::Spectral::retarget(Rng& rng) {
    for (int i = 0; i < n; ++i) {
        const float r = rng.uni();
        p[i].target = (0.06f + 1.9f * r * r) * 1.7f;
    }
}

void Engine::Spectral::slewAmps(float coef) {
    for (int i = 0; i < n; ++i) p[i].amp += coef * (p[i].target - p[i].amp);
}

// ---------------------------------------------------------------------------
//  Wavetable bank
// ---------------------------------------------------------------------------
namespace {

struct Spec {
    float amp[513];
    float ph[513];
};

inline void addHarmonic(float* dst, int N, int k, float a, float ph01) {
    if (a == 0.0f) return;
    for (int i = 0; i < N; ++i) {
        const float p = (float)((k * i) & (N - 1)) * (1.0f / (float)N) + ph01;
        dst[i] += a * sinT(p);
    }
}

inline void normalizeFrame(float* dst, int N) {
    float peak = 0.0f;
    for (int i = 0; i < N; ++i) {
        const float a = std::fabs(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak < 1e-6f) return;
    const float s = 1.0f / peak;
    for (int i = 0; i < N; ++i) dst[i] *= s;
}

// Spectral recipe for one table frame. t is the frame position 0..1.
void makeSpectrum(int table, float t, uint32_t frameSeed, Spec& sp) {
    for (int k = 0; k <= 512; ++k) { sp.amp[k] = 0.0f; sp.ph[k] = 0.0f; }

    nf::Rng rng;
    rng.seed(0x51ED270B00000000ull ^ (uint64_t)(table * 977 + (int)(t * 7.0f) * 131),
             frameSeed);

    switch (table) {
    case 0: {   // SAW: harmonic rolloff that darkens across the frames
        const float p = 1.0f + 3.0f * t;
        for (int k = 1; k <= 256; ++k) sp.amp[k] = 1.0f / std::pow((float)k, p);
        break;
    }
    case 1: {   // PULSE: pulse width 50% -> 4%
        const float d = 0.5f - 0.46f * t;
        for (int k = 1; k <= 256; ++k) {
            const float s = std::sin(3.14159265f * (float)k * d);
            sp.amp[k] = s / (float)k;
        }
        break;
    }
    case 2: {   // RIP: odd-heavy, bright, torn
        for (int k = 1; k <= 256; ++k) {
            const float odd = (k & 1) ? 1.0f : 0.22f;
            sp.amp[k] = odd / std::pow((float)k, 0.72f + 0.5f * t);
            sp.ph[k] = (k & 1) ? 0.0f : 0.25f;
        }
        break;
    }
    case 3: {   // FORMANT: a moving resonant peak
        const float fc = 1.5f + 46.0f * t;
        const float bw = 2.0f + 6.0f * t;
        for (int k = 1; k <= 256; ++k) {
            const float d = ((float)k - fc) / bw;
            sp.amp[k] = std::exp(-d * d) + 0.06f / (float)k;
        }
        break;
    }
    case 4: {   // METAL: sparse high partials, random phases
        for (int k = 1; k <= 256; ++k) {
            const float w = 0.5f + 0.5f * std::sin((float)k * (3.0f + 9.0f * t));
            sp.amp[k] = w * w / std::pow((float)k, 0.55f);
            sp.ph[k] = rng.uni();
        }
        break;
    }
    case 5: {   // HARMONIC: random but tonal
        for (int k = 1; k <= 256; ++k) {
            const float r = rng.uni();
            sp.amp[k] = (r * r) / std::pow((float)k, 0.8f + 0.6f * t);
            sp.ph[k] = rng.uni();
        }
        break;
    }
    case 6: {   // RANDOM: fully random amplitudes and phases
        for (int k = 1; k <= 256; ++k) {
            const float r = rng.uni();
            sp.amp[k] = std::pow(r, 1.5f) / std::sqrt((float)k);
            sp.ph[k] = rng.uni();
        }
        break;
    }
    default: {  // VOID: sub-heavy, very few harmonics
        sp.amp[1] = 1.0f;
        sp.amp[2] = 0.55f * t;
        sp.amp[3] = 0.30f * (1.0f - t);
        sp.amp[4] = 0.18f * t;
        for (int k = 5; k <= 64; ++k)
            sp.amp[k] = 0.05f / (float)k * (1.0f - t);
        break;
    }
    }
}

} // namespace

void Engine::buildBank(Bank& b, uint32_t seed) {
    initSinTable();
    b.d.assign((size_t)kTables * kFrames * kMips * kTableSize, 0.0f);

    static const int kLimits[kMips] = {256, 64, 16, 4};

    for (int t = 0; t < kTables; ++t) {
        // choiceText returns the rest of the choice string, so cut it at the
        // first '|' -- otherwise every table is named "SAW|PULSE|RIP|...".
        const char* nm = choiceText(kWtTable, t);
        int ni = 0;
        for (; nm[ni] && nm[ni] != '|' && ni < (int)sizeof(b.name[t]) - 1; ++ni)
            b.name[t][ni] = nm[ni];
        b.name[t][ni] = 0;
        for (int f = 0; f < kFrames; ++f) {
            const float pos = (float)f / (float)(kFrames - 1);
            Spec sp;
            makeSpectrum(t, pos,
                         seed * 2654435761u + (uint32_t)(t * 131 + f * 7919), sp);
            for (int m = 0; m < kMips; ++m) {
                float* dst =
                    &b.d[(size_t)(((t * kFrames + f) * kMips + m) * kTableSize)];
                std::memset(dst, 0, sizeof(float) * kTableSize);
                const int lim = kLimits[m];
                for (int k = 1; k <= lim; ++k)
                    addHarmonic(dst, kTableSize, k, sp.amp[k], sp.ph[k]);
                normalizeFrame(dst, kTableSize);
            }
        }
    }
}

const char* Engine::tableName(int t) const {
    if (t < 0) t = 0;
    if (t >= kTables) t = kTables - 1;
    return bank_[bankIndex_.load(std::memory_order_acquire)].name[t];
}

// ---------------------------------------------------------------------------
//  Engine
// ---------------------------------------------------------------------------
Engine::Engine() {
    initSinTable();
    for (int i = 0; i < kNumParams; ++i)
        p_[i].store(kParams[i].defNorm, std::memory_order_relaxed);
    for (int i = 0; i < kNumParams; ++i)
        pv_[i] = mapParam(i, kParams[i].defNorm);

    rng_.seed(0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull);

    // Every instance starts on its own bank. A fixed seed would make two
    // instances (or two instances inside the same host project) sound identical,
    // which defeats the point of a noise instrument. The clock gives the
    // entropy, the atomic counter guarantees uniqueness even when several
    // engines are constructed in the same millisecond.
    static std::atomic<uint32_t> instanceCount{0};
    const uint64_t nowNs = (uint64_t)std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
    uint32_t s = (uint32_t)(nowNs ^ (nowNs >> 32)) * 2654435761u +
                 (instanceCount.fetch_add(1) + 1) * 40503u;
    if (s == 0) s = 1;

    seed_.store(s);
    seedCounter_ = s + 1;
    buildBank(bank_[0], s);
    bank_[1] = bank_[0];
    bankIndex_.store(0);
    // Pretend two blocks have already been rendered so that DICE works even
    // before the first process() call (the editor may open first).
    swapBlock_.store(blockCount_.load(std::memory_order_relaxed) - 2u,
                     std::memory_order_relaxed);

    for (int i = 0; i < kScopeSize; ++i) scope_[i].store(0.0f);
    controlCounter_ = 0;
}

void Engine::prepare(double sampleRate) {
    sr_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    reset();
}

void Engine::reset() {
    // Seed every voice from the instance seed: two instances must never produce
    // the same noise, or layering them is just a phase/comb artefact.
    const uint64_t inst = (uint64_t)seed_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        v = Voice();
        v.rng.seed(0x243F6A8885A308D3ull ^
                       (inst * 0x9E3779B97F4A7C15ull) +
                       (uint64_t)i * 0x9E3779B97F4A7C15ull,
                   0x13198A2E03707344ull ^
                       (inst * 0xC2B2AE3D27D4EB4Full) + (uint64_t)i);
    }
    dcL_ = dcR_ = dcInL_ = dcInR_ = 0.0;
    masterFade_ = 0.0f;
    controlCounter_ = 0;
    for (int i = 0; i < kScopeSize; ++i) scope_[i].store(0.0f);
    scopeW_.store(0);
    peak_.store(0.0f);
    active_.store(0);
}

void Engine::setParam(int id, float norm) {
    if (id < 0 || id >= kNumParams) return;
    p_[id].store(clampf(norm, 0.0f, 1.0f), std::memory_order_relaxed);
}

bool Engine::swapAllowed() const {
    // Two block boundaries of grace after the previous swap: process() latches
    // the bank index once per block, so the buffer being written cannot be the
    // one the audio thread is inside.
    const uint32_t now = blockCount_.load(std::memory_order_acquire);
    if (now - swapBlock_.load(std::memory_order_acquire) >= 2) return true;
    // Transport stopped: blockCount_ never advances, so fall back to "nothing
    // has been processed for a while" -- otherwise loading a project while
    // stopped silently drops the stored wavetable seed.
    const uint64_t t = lastBlockNs_.load(std::memory_order_relaxed);
    if (t == 0) return true;
    const uint64_t n = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return n - t > 60;   // 60 ms without a block -> nothing in flight
}

// ---------------------------------------------------------------------------
bool Engine::randomizeTables() {
    if (!swapAllowed()) return false;

    const int next = 1 - bankIndex_.load(std::memory_order_relaxed);
    const uint32_t s = seedCounter_++;
    buildBank(bank_[next], s);
    seed_.store(s, std::memory_order_relaxed);
    bankIndex_.store(next, std::memory_order_release);
    swapBlock_.store(blockCount_.load(std::memory_order_acquire),
                     std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
void Engine::setTableSeed(uint32_t seed) {
    if (seed == 0) seed = 1;
    if (seed == seed_.load(std::memory_order_relaxed)) return;
    if (!swapAllowed()) return;

    const int next = 1 - bankIndex_.load(std::memory_order_relaxed);
    buildBank(bank_[next], seed);
    seed_.store(seed, std::memory_order_relaxed);
    if (seedCounter_ <= seed) seedCounter_ = seed + 1;
    bankIndex_.store(next, std::memory_order_release);
    swapBlock_.store(blockCount_.load(std::memory_order_acquire),
                     std::memory_order_release);
}

// ---------------------------------------------------------------------------
int Engine::allocVoice(int note) {
    for (int i = 0; i < kMaxVoices; ++i)
        if (!voices_[i].active) return i;

    // Nothing free: take the oldest voice that is not already fading, and start
    // its 40 ms fade now. noteOn() will queue the new note on it and the
    // retrigger happens at the end of the fade in controlTick().
    int best = -1;
    uint32_t bestAge = 0xFFFFFFFFu;
    for (int i = 0; i < kMaxVoices; ++i) {
        const Voice& v = voices_[i];
        if (v.stealing) continue;
        if (v.age < bestAge) {
            bestAge = v.age;
            best = i;
        }
    }
    if (best >= 0) {
        voices_[best].stealing = true;
        voices_[best].steal = 1.0f;
    }
    (void)note;
    return best;
}

void Engine::noteOn(int note, float velocity) {
    note = note < 0 ? 0 : (note > 127 ? 127 : note);
    const float vel = clampf(velocity, 0.0f, 1.0f);

    // Re-use the same voice for a repeated note so a legato retrigger glides
    // instead of stacking two copies.
    int idx = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.active && !v.stealing && v.note == note && v.held) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = allocVoice(note);
    if (idx < 0) return;

    Voice& v = voices_[idx];
    if (v.stealing) {
        // Still fading out; queue this note for when the fade completes.
        v.pendingNote = note;
        v.pendingVel = vel;
        return;
    }

    const bool wasActive = v.active;
    const uint32_t keepAge = wasActive ? v.age : ++voiceAge_;
    const float keepEnv = wasActive ? v.env.level : 0.0f;
    const float keepEnv2 = wasActive ? v.env2.level : 0.0f;
    // A fresh voice has to start *at* the pitch of its note, transposition
    // included, otherwise the first note glides up from the untransposed key.
    // Read the atomics directly: pv_ is only refreshed on the control tick, so
    // it still holds the previous value when the host sends a note early.
    const float startPitch =
        (float)(note - 69) *
            mapParam(kWtKey, p_[kWtKey].load(std::memory_order_relaxed)) +
        mapParam(kOctave, p_[kOctave].load(std::memory_order_relaxed)) * 12.0f +
        mapParam(kWtPitch, p_[kWtPitch].load(std::memory_order_relaxed)) +
        mapParam(kWtFine, p_[kWtFine].load(std::memory_order_relaxed)) * 0.01f;
    const float keepGlide = wasActive ? v.glideCur : startPitch;
    Spectral keepBand = v.band;
    const bool keepBandInit = wasActive && v.bandInit;
    const float keepLo = v.bandLo, keepHi = v.bandHi;
    const int keepCount = v.bandCount;
    const Rng keepRng = v.rng;

    v.active = true;
    v.held = true;
    v.stealing = false;
    v.steal = 1.0f;
    v.note = note;
    v.vel = vel;
    v.age = keepAge;
    v.glideCur = keepGlide;
    v.glideTarget = startPitch;
    // Random phase only for a new voice: a retrigger of a sounding note keeps
    // its phase, so repeating a key does not click.
    if (!wasActive) v.phase = rng_.uni();
    v.env.stage = Env::kAttack;
    v.env.level = keepEnv;
    v.env2.stage = Env::kAttack;
    v.env2.level = keepEnv2;
    v.gateTimer = 0.0f;
    v.gated = false;
    v.lfo1Phase = 0.0f;
    v.lfo2Phase = 0.0f;
    v.lfo1SH = rng_.bi();
    v.lfo2SH = rng_.bi();
    v.lfo1Next = rng_.bi();
    v.lfo2Next = rng_.bi();
    v.randPhase = 0.0f;
    v.randVal = rng_.bi();
    v.randTarget = rng_.bi();
    v.driftWt = rng_.bi() * 0.35f;
    v.driftBand = rng_.bi() * 0.35f;
    v.gateVal = 1.0f;
    v.dsCount = 0;
    v.holdCount = 0;
    v.crackEnv = 0.0f;
    v.brown = 0.0f;
    for (int i = 0; i < 7; ++i) v.pink[i] = 0.0f;
    v.hf1.clear();
    v.hf2.clear();
    v.tilt.clear();

    // The band bank survives a retrigger: re-randomising it would make every
    // repeated note start with a different spectrum and click.
    if (keepBandInit) {
        v.band = keepBand;
        v.bandInit = true;
        v.bandLo = keepLo;
        v.bandHi = keepHi;
        v.bandCount = keepCount;
    } else {
        v.bandInit = false;
    }
    v.rng = keepRng;

    // A fresh voice starts at zero gain and ramps in over one control block, so
    // even a stolen voice cannot produce a discontinuity.
    if (!wasActive) v.env.level = 0.0f;
}

void Engine::noteOff(int note) {
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.active && v.note == note && v.held) {
            v.held = false;
            v.env.off();
            v.env2.off();
        }
    }
}

void Engine::allNotesOff() {
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.active) {
            v.held = false;
            v.env.off();
            v.env2.off();
        }
    }
}

// ---------------------------------------------------------------------------
void Engine::controlTick() {
    for (int i = 0; i < kNumParams; ++i)
        pv_[i] = mapParam(i, p_[i].load(std::memory_order_relaxed));

    for (int s = 0; s < 6; ++s) {
        // pv_ already holds the *mapped* value for every parameter, choices
        // include: mapping it a second time clamped 3.0 to 1.0 and turned every
        // route into "AFTER -> LEVEL".
        modSrc_[s] = (int)pv_[kMod1Src + s * 3];
        modDst_[s] = (int)pv_[kMod1Dst + s * 3];
        modAmt_[s] = pv_[kMod1Amt + s * 3];
    }

    const float dt = (float)kControlRate / (float)sr_;
    const float glideSec = pv_[kGlide];
    const float glideCoef =
        glideSec <= 0.0005f
            ? 1.0f
            : 1.0f - std::exp(-(float)kControlRate / (glideSec * (float)sr_));

    const float randRate = pv_[kRandRate];
    const float randSmooth = pv_[kRandSmooth];
    // Smooth = 1 gives a very slow slew, smooth = 0 is a hard step.
    const float randCoef =
        1.0f - std::exp(-6.2831853f * randRate *
                        (0.02f + 0.98f * (1.0f - randSmooth)) * dt);

    const int lfo1Shape = (int)pv_[kLfo1Shape];
    const int lfo2Shape = (int)pv_[kLfo2Shape];
    float lfo1Rate = pv_[kLfo1Rate];
    float lfo2Rate = pv_[kLfo2Rate];
    if (pv_[kLfo1Sync] >= 0.5f) lfo1Rate = std::max(0.05f, bpm_ / 60.0f);
    if (pv_[kLfo2Sync] >= 0.5f) lfo2Rate = std::max(0.05f, bpm_ / 60.0f);

    const float att = pv_[kAttack];
    const float dec = pv_[kDecay];
    const float sus = pv_[kSustain];
    const float rel = pv_[kRelease];
    const float att2 = pv_[kEnv2Attack];
    const float dec2 = pv_[kEnv2Decay];
    const float gateLen = pv_[kGateLen];
    const float velToAmp = pv_[kVelToAmp];
    const float velToEnv = pv_[kVelToEnv];

    int act = 0;

    for (int vi = 0; vi < kMaxVoices; ++vi) {
        Voice& v = voices_[vi];
        if (!v.active) continue;

        // ---- voice steal: fade, then retrigger the queued note --------------
        if (v.stealing) {
            v.steal -= (float)kControlRate / (0.040f * (float)sr_);
            if (v.steal <= 0.0f) {
                const int pn = v.pendingNote;
                const float pvel = v.pendingVel;
                const uint32_t a = v.age;
                v = Voice();
                v.active = true;
                v.held = true;
                v.note = pn;
                v.vel = pvel;
                v.age = a;
                v.steal = 1.0f;
                v.env.stage = Env::kAttack;
                v.env2.stage = Env::kAttack;
                v.glideCur = (float)(pn - 69) * pv_[kWtKey] +
                             pv_[kOctave] * 12.0f + pv_[kWtPitch] +
                             pv_[kWtFine] * 0.01f;
                v.glideTarget = v.glideCur;
                v.phase = rng_.uni();
                v.gateVal = 1.0f;
                v.bandInit = false;
                v.rng.seed(0x9E3779B97F4A7C15ull * (uint64_t)(pn + 1) + rng_.next(),
                           rng_.next() | 1ull);
            }
            ++act;
            continue;
        }

        v.env.setTimes(sr_, att, dec, sus, rel);
        v.env2.setTimes(sr_, att2, dec2, 1.0f, rel);

        // ---- gate length ---------------------------------------------------
        if (gateLen > 0.0001f && v.held) {
            if (!v.gated) {
                v.gated = true;
                v.gateTimer = gateLen;
            }
            v.gateTimer -= dt;
            if (v.gateTimer <= 0.0f) {
                v.held = false;
                v.env.off();
                v.env2.off();
            }
        }
        v.gateVal += ((v.held ? 1.0f : 0.0f) - v.gateVal) * 0.12f;

        // ---- LFOs ----------------------------------------------------------
        v.lfo1Phase += lfo1Rate * dt;
        if (v.lfo1Phase >= 1.0f) {
            v.lfo1Phase -= std::floor(v.lfo1Phase);
            v.lfo1SH = v.lfo1Next;
            v.lfo1Next = v.rng.bi();
        }
        v.lfo2Phase += lfo2Rate * dt;
        if (v.lfo2Phase >= 1.0f) {
            v.lfo2Phase -= std::floor(v.lfo2Phase);
            v.lfo2SH = v.lfo2Next;
            v.lfo2Next = v.rng.bi();
        }
        v.lfo1 = lfoValue(lfo1Shape, v.lfo1Phase, v.lfo1SH, v.lfo1Next);
        v.lfo2 = lfoValue(lfo2Shape, v.lfo2Phase, v.lfo2SH, v.lfo2Next);

        // ---- random --------------------------------------------------------
        v.randPhase += randRate * dt;
        if (v.randPhase >= 1.0f) {
            v.randPhase -= std::floor(v.randPhase);
            v.randTarget = v.rng.bi();
        }
        v.randVal += randCoef * (v.randTarget - v.randVal);

        // ---- slow per-voice drift ------------------------------------------
        v.driftWt = clampf(v.driftWt * 0.995f + v.rng.bi() * 0.02f, -1.0f, 1.0f);
        v.driftBand = clampf(v.driftBand * 0.995f + v.rng.bi() * 0.02f, -1.0f, 1.0f);

        // ---- modulation matrix ---------------------------------------------
        float src[kNumSrc] = {0};
        src[kSrcEnv1] = v.env.level;
        src[kSrcEnv2] = v.env2.level;
        src[kSrcLfo1] = v.lfo1;
        src[kSrcLfo2] = v.lfo2;
        src[kSrcRand] = v.randVal;
        src[kSrcVel] = v.vel;
        src[kSrcKey] = (float)v.note / 127.0f;
        src[kSrcGate] = v.gateVal;
        src[kSrcAfter] = after_;

        float dst[kNumDst] = {0};
        for (int s = 0; s < 6; ++s) {
            const int si = modSrc_[s];
            const int di = modDst_[s];
            if (si <= 0 || di <= 0) continue;
            dst[di] += src[si] * modAmt_[s];
        }

        v.dPitch = dst[kDstPitch] * 24.0f;
        v.dWtPos = dst[kDstWtPos];
        v.dBandC = dst[kDstBandCenter] * 4.0f;
        v.dBandW = dst[kDstBandWidth] * 3.0f;
        v.dCut = dst[kDstHarshCut] * 4.0f;
        v.dDrive = dst[kDstHarshDrive];
        v.dCrush = dst[kDstCrush];
        v.dPan = dst[kDstPan];
        v.dLevel = dst[kDstLevel];

        // ---- pitch ----------------------------------------------------------
        const float keyAmt = pv_[kWtKey];
        // MIDI note 69 is A4 = 440 Hz, which is the reference every host and
        // every other instrument uses.
        const float pitchSemis = (float)(v.note - 69) * keyAmt +
                                 pv_[kOctave] * 12.0f + pv_[kWtPitch] +
                                 pv_[kWtFine] * 0.01f + v.dPitch;
        v.glideTarget = pitchSemis;
        v.glideCur += glideCoef * (v.glideTarget - v.glideCur);
        const float freq = 440.0f * std::pow(2.0f, v.glideCur / 12.0f);
        v.incr = (double)freq / sr_;

        // ---- wavetable ------------------------------------------------------
        v.table = (int)pv_[kWtTable];
        v.wtDrift = pv_[kWtDrift];
        v.wtFrame = clampf(pv_[kWtPos] + v.dWtPos * 0.5f +
                               v.driftWt * v.wtDrift * 0.5f,
                           0.0f, 1.0f);
        v.wtLevel = clampf(pv_[kWtLevel], 0.0f, 1.0f);
        v.wtPan = clampf(pv_[kWtPan] + v.dPan * 0.5f, 0.0f, 1.0f);

        // ---- harsh ----------------------------------------------------------
        v.harshType = pv_[kHarshType];
        v.hCut = pv_[kHarshCut] * std::pow(2.0f, v.dCut);
        v.hReso = pv_[kHarshReso];
        v.hDrive = clampf(pv_[kHarshDrive] + v.dDrive, 0.0f, 1.0f);
        v.hFold = pv_[kHarshFold];
        v.hBits = pv_[kHarshBits];
        v.hDown = pv_[kHarshDown];
        v.hColor = pv_[kHarshColor];
        v.hLevel = clampf(pv_[kHarshLevel], 0.0f, 1.0f);
        v.hPan = clampf(pv_[kHarshPan] + v.dPan * 0.5f, 0.0f, 1.0f);
        v.crushAmt = clampf(v.dCrush, -1.0f, 1.0f);

        const float q = 0.5f + v.hReso * 19.5f;
        v.hf1.set(sr_, v.hCut, q);
        v.hf2.set(sr_, v.hCut * 1.45f, q * 0.7f);
        v.tilt.set(sr_, 200.0f + 9000.0f * v.hColor * v.hColor);

        // ---- band -----------------------------------------------------------
        const float keyBand = pv_[kBandKey];
        const float bandCentre =
            pv_[kBandCenter] *
            std::pow(2.0f, (float)(v.note - 60) * keyBand / 12.0f + v.dBandC +
                             v.driftBand * pv_[kBandDrift] * 1.5f);
        const float bandWidth = pv_[kBandWidth] * std::pow(2.0f, v.dBandW);
        const int nPartials = (int)clampf(pv_[kBandPartials], 1.0f, 96.0f);

        float lo = bandCentre * std::pow(2.0f, -bandWidth * 0.5f);
        float hi = bandCentre * std::pow(2.0f, bandWidth * 0.5f);
        lo = clampf(lo, 0.01f, 20000.0f);
        // Never synthesise above Nyquist: at 44.1 kHz a partial at 23 kHz just
        // folds back into the audible band.
        const float bandMax = std::min(23500.0f, (float)(sr_ * 0.45));
        hi = clampf(hi, lo * 1.004f, bandMax);

        const bool changed =
            !v.bandInit || nPartials != v.bandCount ||
            lo > v.bandLo * 1.02f || lo < v.bandLo * 0.98f ||
            hi > v.bandHi * 1.02f || hi < v.bandHi * 0.98f;
        if (changed) {
            // Keep the running partials' phase/amplitude when the band only
            // moved: re-randomising them on every control tick is a crackle.
            v.band.rebuild(v.rng, sr_, lo, hi, nPartials, v.bandInit);
            v.bandLo = lo;
            v.bandHi = hi;
            v.bandCount = nPartials;
            v.bandInit = true;
        }
        v.band.refresh += dt;
        if (v.band.refresh >= v.band.refreshPeriod) {
            v.band.refresh = 0.0f;
            v.band.refreshPeriod = 0.35f + v.rng.uni() * 1.6f;
            v.band.retarget(v.rng);
        }
        v.band.slewAmps(1.0f - std::exp(-(float)kControlRate /
                                        (0.055f * (float)sr_)));
        v.bandPan = clampf(pv_[kBandPan] + v.dPan * 0.5f, 0.0f, 1.0f);
        v.bLevel = clampf(pv_[kBandLevel], 0.0f, 1.0f);

        // ---- amp / velocity --------------------------------------------------
        v.ampGain = clampf(1.0f + v.dLevel, 0.0f, 2.0f);
        v.velGain = 1.0f - velToAmp * (1.0f - v.vel);
        v.env2Amt = pv_[kEnv2Amount] * (1.0f - velToEnv * (1.0f - v.vel));

        ++act;
    }

    active_.store(act, std::memory_order_relaxed);

    tDrive_ = pv_[kDrive];
    tWidth_ = pv_[kWidth];
    tOut_ = pv_[kOutput];
    tCeil_ = pv_[kCeiling];
}

// ---------------------------------------------------------------------------
float Engine::lfoValue(int shape, float phase, float sh, float next) {
    switch (shape) {
    case 0: return std::sin(6.28318530718f * phase);
    case 1: return 1.0f - 4.0f * std::fabs(phase - 0.5f);
    case 2: return 2.0f * phase - 1.0f;
    case 3: return phase < 0.5f ? 1.0f : -1.0f;
    case 4: return sh;
    default: {
        const float s = 0.5f - 0.5f * std::cos(6.28318530718f * phase);
        return sh + (next - sh) * s;
    }
    }
}

// ---------------------------------------------------------------------------
float Engine::harshSample(Voice& v) {
    const int type = (int)(v.harshType + 0.5f);
    float w = 0.0f;

    switch (type) {
    case 0:
        w = v.rng.bi();
        break;
    case 1: {   // pink (Paul Kellet's economy filter)
        const float white = v.rng.bi();
        v.pink[0] = 0.99886f * v.pink[0] + white * 0.0555179f;
        v.pink[1] = 0.99332f * v.pink[1] + white * 0.0750759f;
        v.pink[2] = 0.96900f * v.pink[2] + white * 0.1538520f;
        v.pink[3] = 0.86650f * v.pink[3] + white * 0.3104856f;
        v.pink[4] = 0.55000f * v.pink[4] + white * 0.5329522f;
        v.pink[5] = -0.7616f * v.pink[5] - white * 0.0168980f;
        w = (v.pink[0] + v.pink[1] + v.pink[2] + v.pink[3] + v.pink[4] +
             v.pink[5] + v.pink[6] + white * 0.5362f) *
            0.11f;
        v.pink[6] = white * 0.115926f;
        break;
    }
    case 2:     // brown
        v.brown += v.rng.bi() * 0.02f;
        v.brown *= 0.9992f;
        w = clampf(v.brown * 3.5f, -1.5f, 1.5f);
        break;
    case 3: {   // blue: differentiated white
        const float x = v.rng.bi();
        w = (x - v.lastWhite) * 0.6f;
        v.lastWhite = x;
        break;
    }
    case 4: {   // violet: second difference
        const float x = v.rng.bi();
        const float d1 = x - v.lastWhite;
        v.lastWhite = x;
        w = (d1 - v.prevDiff) * 0.4f;
        v.prevDiff = d1;
        break;
    }
    case 5:     // velvet: sparse impulses that decay
        if (v.rng.uni() < 0.02f) v.holdVal = v.rng.bi() * 1.7f;
        else v.holdVal *= 0.72f;
        w = v.holdVal;
        break;
    case 6:     // crackle: envelope-gated spikes
        v.crackEnv *= 0.9988f;
        if (v.rng.uni() < 0.0045f) {
            v.crackEnv = 0.55f + 0.45f * v.rng.uni();
            v.crackSign = v.rng.bi() > 0.0f ? 1.0f : -1.0f;
        }
        w = v.crackSign * v.crackEnv * (0.4f + 0.6f * std::fabs(v.rng.bi()));
        break;
    default:    // digital: sample-and-hold at a low rate
        if (--v.holdCount <= 0) {
            v.holdCount = 1 + (int)(v.rng.uni() * 9.0f);
            v.holdVal = v.rng.bi();
        }
        w = v.holdVal;
        break;
    }

    // Colour: a tilt from dark (low-passed) to bright (high-passed).
    {
        const float lp = v.tilt.lp(w);
        if (v.hColor < 0.5f) {
            const float a = 2.0f * v.hColor;
            w = w * a + lp * (1.0f - a);
        } else {
            const float a = 2.0f * v.hColor - 1.0f;
            w = w * (1.0f - a) + (w - lp) * a;
        }
    }

    // Resonant band-pass stage plus a little of the raw source.
    const float bp = v.hf1.bp(w) * (1.0f + v.hReso * 3.0f);
    const float bp2 = v.hf2.bp(w) * (0.5f + v.hReso);
    w = bp * 1.15f + bp2 * 0.35f + w * 0.25f;

    // Wavefolder.
    if (v.hFold > 0.001f) {
        w = w * (1.0f - v.hFold) +
            std::sin(w * (1.0f + v.hFold * 7.0f) * 1.5707963f) * v.hFold * 1.2f;
    }

    // Drive.
    w = std::tanh(w * (1.0f + v.hDrive * 7.0f));

    // Bit crush.
    float bits = v.hBits;
    if (v.crushAmt < 0.0f) bits = clampf(bits + v.crushAmt * 14.0f, 1.0f, 16.0f);
    if (bits < 15.9f) {
        const float levels = std::pow(2.0f, bits) * 0.5f;
        w = std::floor(w * levels + 0.5f) / levels;
    }

    // Sample-rate reduction.
    float down = v.hDown;
    if (v.crushAmt > 0.0f)
        down = clampf(down + v.crushAmt * 63.0f, 1.0f, 64.0f);
    const int ds = (int)(down + 0.5f);
    if (ds > 1) {
        if (--v.dsCount <= 0) {
            v.dsCount = ds;
            v.crushHold = w;
        }
        w = v.crushHold;
    }

    return w * 0.7f;
}

// ---------------------------------------------------------------------------
float Engine::oscSample(Voice& v, const Bank& b) {
    int table = v.table;
    if (table < 0) table = 0;
    if (table >= kTables) table = kTables - 1;

    const float pos = v.wtFrame * (float)(kFrames - 1);
    int f0 = (int)pos;
    if (f0 < 0) f0 = 0;
    if (f0 > kFrames - 1) f0 = kFrames - 1;
    int f1 = f0 + 1;
    if (f1 > kFrames - 1) f1 = kFrames - 1;
    const float ff = pos - (float)f0;

    // Most detailed mip that cannot alias at this pitch.
    const double fundamental = (double)v.incr * sr_;
    const double maxHarm = (sr_ * 0.45) / std::max(0.5, fundamental);
    static const double kLim[kMips] = {256.0, 64.0, 16.0, 4.0};
    int mip = kMips - 1;
    for (int m = 0; m < kMips; ++m) {
        if (kLim[m] <= maxHarm) {
            mip = m;
            break;
        }
    }

    const float* a0 = tablePtr(b, table, f0, mip);
    const float* a1 = tablePtr(b, table, f1, mip);

    float p = (float)(v.phase - std::floor(v.phase));
    const float x = p * (float)kTableSize;
    int i0 = (int)x;
    if (i0 < 0) i0 = 0;
    if (i0 > kTableSize - 1) i0 = kTableSize - 1;
    const int i1 = (i0 + 1) & (kTableSize - 1);
    const float pf = x - (float)i0;

    const float s0 = a0[i0] + (a0[i1] - a0[i0]) * pf;
    const float s1 = a1[i0] + (a1[i1] - a1[i0]) * pf;

    v.phase += v.incr;
    if (v.phase >= 1.0) v.phase -= std::floor(v.phase);
    if (v.phase < 0.0) v.phase += 1.0;

    return s0 + (s1 - s0) * ff;
}

// ---------------------------------------------------------------------------
void Engine::process(float* outL, float* outR, int n) {
    if (n <= 0) return;
    blockCount_.fetch_add(1, std::memory_order_acq_rel);
    lastBlockNs_.store(
        (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    const Bank& bank = bank_[bankIndex_.load(std::memory_order_acquire)];
    if (bank.d.empty()) {
        for (int i = 0; i < n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }
        return;
    }

    const bool bypass = p_[kBypass].load(std::memory_order_relaxed) >= 0.5f;
    const bool wtOn = p_[kOscOn].load(std::memory_order_relaxed) >= 0.5f;
    const bool harshOn = p_[kHarshOn].load(std::memory_order_relaxed) >= 0.5f;
    const bool bandOn = p_[kBandOn].load(std::memory_order_relaxed) >= 0.5f;

    const float fadeStep = 1.0f / (0.020f * (float)sr_);
    const float bypassStep = 1.0f / (0.008f * (float)sr_);

    float peak = 0.0f;
    uint32_t scopeCounter = 0;
    uint32_t sw = scopeW_.load(std::memory_order_relaxed);

    for (int i = 0; i < n; ++i) {
        if ((controlCounter_ & (kControlRate - 1)) == 0) controlTick();
        ++controlCounter_;

        if (masterFade_ < 1.0f) {
            masterFade_ += fadeStep;
            if (masterFade_ > 1.0f) masterFade_ = 1.0f;
        }

        // Bypass is a ramp, not a switch: hard-muting a sounding voice is a
        // click at the toggle.
        if (bypass) {
            bypassGain_ -= bypassStep;
            if (bypassGain_ < 0.0f) bypassGain_ = 0.0f;
        } else {
            bypassGain_ += bypassStep;
            if (bypassGain_ > 1.0f) bypassGain_ = 1.0f;
        }

        float l = 0.0f, r = 0.0f;

        for (int vi = 0; vi < kMaxVoices; ++vi) {
            Voice& v = voices_[vi];
            if (!v.active) continue;

            const float e1 = v.env.tick();
            const float e2 = v.env2.tick();
            if (v.env.stage == Env::kIdle && !v.stealing) {
                v.active = false;
                continue;
            }

            const float amp = e1 * v.ampGain * v.steal * v.velGain;
            if (amp < 1e-7f) {
                if (v.env.stage == Env::kIdle) v.active = false;
                continue;
            }

            float vl = 0.0f, vr = 0.0f;

            if (wtOn && v.wtLevel > 0.0005f) {
                const float s = oscSample(v, bank) * v.wtLevel;
                const float gl = (1.0f - v.wtPan) * 1.41421356f;
                const float gr = v.wtPan * 1.41421356f;
                vl += s * gl;
                vr += s * gr;
            }
            if (harshOn && v.hLevel > 0.0005f) {
                const float s = harshSample(v) * v.hLevel;
                const float gl = (1.0f - v.hPan) * 1.41421356f;
                const float gr = v.hPan * 1.41421356f;
                vl += s * gl;
                vr += s * gr;
            }
            if (bandOn && v.bLevel > 0.0005f) {
                // ENV2 shapes the band level around its centre, so a sub rumble
                // can be made to breathe without a separate VCA.
                const float shape = 1.0f + v.env2Amt * (e2 - 0.5f) * 1.6f;
                const float s = bandSample(v) * v.bLevel * clampf(shape, 0.0f, 2.5f);
                const float gl = (1.0f - v.bandPan) * 1.41421356f;
                const float gr = v.bandPan * 1.41421356f;
                vl += s * gl;
                vr += s * gr;
            }

            l += vl * amp;
            r += vr * amp;
        }

        // ---- master drive ---------------------------------------------------
        if (tDrive_ > 0.001f) {
            const float dg = 1.0f + tDrive_ * 6.0f;
            const float comp = 1.0f / (1.0f + tDrive_ * 2.5f);
            l = std::tanh(l * dg) * comp;
            r = std::tanh(r * dg) * comp;
        }

        // ---- stereo width (mid/side) ----------------------------------------
        if (tWidth_ != 1.0f) {
            const float mid = (l + r) * 0.5f;
            const float side = (l - r) * 0.5f * tWidth_;
            l = mid + side;
            r = mid - side;
        }

        // ---- DC blocker at ~0.02 Hz -----------------------------------------
        // Deliberately far below the audio band: sub-20 Hz material is content,
        // not an artefact, so only true DC is removed.
        {
            const double R = 0.9999993;
            const double yl = (double)l - dcInL_ + R * dcL_;
            dcInL_ = l;
            dcL_ = yl;
            const double yr = (double)r - dcInR_ + R * dcR_;
            dcInR_ = r;
            dcR_ = yr;
            l = (float)yl;
            r = (float)yr;
        }

        // ---- output ----------------------------------------------------------
        l *= tOut_ * masterFade_;
        r *= tOut_ * masterFade_;

        // Soft knee, then a hard guarantee: the ceiling is real.
        const float ceil = tCeil_;
        l = ceil * std::tanh(l / ceil);
        r = ceil * std::tanh(r / ceil);
        if (l > ceil) l = ceil;
        if (l < -ceil) l = -ceil;
        if (r > ceil) r = ceil;
        if (r < -ceil) r = -ceil;
        if (!(l == l)) l = 0.0f;   // NaN guard
        if (!(r == r)) r = 0.0f;

        const float oL = l * bypassGain_;
        const float oR = r * bypassGain_;
        outL[i] = oL;
        outR[i] = oR;

        const float a = std::fabs(oL);
        if (a > peak) peak = a;

        if ((scopeCounter & 3u) == 0u) {
            scope_[sw & (kScopeSize - 1)].store(oL, std::memory_order_relaxed);
            ++sw;
        }
        ++scopeCounter;
    }

    scopeW_.store(sw, std::memory_order_relaxed);
    peak_.store(peak, std::memory_order_relaxed);
}

void Engine::processSilent(int n) {
    blockCount_.fetch_add(1, std::memory_order_acq_rel);
    uint32_t sw = scopeW_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        scope_[sw & (kScopeSize - 1)].store(0.0f, std::memory_order_relaxed);
        ++sw;
    }
    scopeW_.store(sw, std::memory_order_relaxed);
    peak_.store(0.0f, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
void Engine::copyScope(float* dst, int n) const {
    if (!dst || n <= 0) return;
    if (n > kScopeSize) n = kScopeSize;
    const uint32_t w = scopeW_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const uint32_t idx = (w + (uint32_t)i) & (kScopeSize - 1);
        dst[i] = scope_[idx].load(std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
//  Editor preview: one table frame, read from the bank the audio thread is
//  currently using. Only the editor calls this, and it copies -- it never
//  exposes the bank's memory.
void Engine::copyTablePreview(int table, float pos, float* dst, int n) const {
    if (!dst || n <= 0) return;
    if (table < 0) table = 0;
    if (table >= kTables) table = kTables - 1;
    const Bank& b = bank_[bankIndex_.load(std::memory_order_acquire)];
    if (b.d.empty()) {
        for (int i = 0; i < n; ++i) dst[i] = 0.0f;
        return;
    }

    pos = clampf(pos, 0.0f, 1.0f);
    const float fp = pos * (float)(kFrames - 1);
    int f0 = (int)fp;
    if (f0 < 0) f0 = 0;
    if (f0 > kFrames - 1) f0 = kFrames - 1;
    int f1 = f0 + 1;
    if (f1 > kFrames - 1) f1 = kFrames - 1;
    const float ff = fp - (float)f0;

    const float* a = tablePtr(b, table, f0, 0);
    const float* c = tablePtr(b, table, f1, 0);

    for (int i = 0; i < n; ++i) {
        const float x = (float)i * (float)kTableSize / (float)n;
        int i0 = (int)x & (kTableSize - 1);
        const int i1 = (i0 + 1) & (kTableSize - 1);
        const float pf = x - std::floor(x);
        const float s0 = a[i0] + (a[i1] - a[i0]) * pf;
        const float s1 = c[i0] + (c[i1] - c[i0]) * pf;
        dst[i] = s0 + (s1 - s0) * ff;
    }
}

} // namespace nf
