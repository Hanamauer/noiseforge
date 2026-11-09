// ===========================================================================
//  NoiseForgeEditor.cpp -- NOISEFORGE panel, "Pulse Demon" edition
//
//  White paper, black ink, xerox grain: the whole surface is printed rather
//  than lit.  Everything is still drawn with the GDI+ flat C API -- no assets,
//  no VSTGUI -- so the same g++ that builds the plugin builds the panel.
//
//  THREADING
//      UI thread       -> wndProc             -> IParamSink only
//      audio thread    -> engine              -> never touches this file
//      host/automation -> setParamNormalized  -> posts WM_APP_PARAM
//  The panel reads the scope ring, the peak, the seed; it never calls into the
//  engine's audio path.
// ===========================================================================

#include "NoiseForgeEditor.h"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../src/NoiseForgeCore.h"

using namespace Steinberg;
using namespace Gdiplus;
using namespace Gdiplus::DllExports;

namespace nf {
namespace {

// Posted by the plugin when a parameter moves from the host or automation.
const UINT WM_APP_PARAM = WM_APP + 1;

// ---------------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------------
const int kW = 1280;
const int kH = 800;

struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
};

inline bool inside(const RectI& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Panels, indexed by GroupId.  Header: 0..58.  Visualisers: 66..240.
// Panels: 246..764.  Status: 770..800.
const RectI kPanel[kNumGroups] = {
    {  12, 246, 288, 130 },   // VOICE
    { 308, 246, 288, 190 },   // ENVELOPES
    { 604, 246, 288, 268 },   // WAVETABLE OSC
    { 308, 444, 288, 320 },   // HARSH NOISE
    {  12, 384, 288, 380 },   // BAND NOISE
    { 900, 246, 368, 518 },   // MODULATION
    { 604, 522, 288, 160 },   // OUTPUT
};

const RectI kInfoRect{ 604, 690, 288, 74 };

// Controls per group, in panel order.
const int kCtl[kNumGroups][12] = {
    { kGlide, kVelToAmp, kVelToEnv, kOctave },
    { kAttack, kDecay, kSustain, kRelease,
      kEnv2Attack, kEnv2Decay, kEnv2Amount, kGateLen },
    { kOscOn, kWtTable, kWtPos, kWtLevel, kWtPitch, kWtFine, kWtKey, kWtPan,
      kWtDrift },
    { kHarshOn, kHarshType, kHarshLevel, kHarshColor, kHarshCut, kHarshReso,
      kHarshDrive, kHarshFold, kHarshBits, kHarshDown, kHarshPan },
    { kBandOn, kBandCenter, kBandWidth, kBandLevel, kBandPartials, kBandKey,
      kBandDrift, kBandPan },
    { 0 },
    { kDrive, kWidth, kOutput, kCeiling, kBypass },
};
const int kCtlCount[kNumGroups] = { 4, 8, 9, 11, 8, 0, 5 };
const int kCtlCols[kNumGroups]  = { 4, 4, 3, 4, 3, 0, 3 };

// MODULATION panel: eight globals in a 4x2 block, then six route rows.
const int kModGlobals[8] = { kLfo1Rate, kLfo1Shape, kLfo1Sync, kLfo2Rate,
                             kLfo2Shape, kLfo2Sync, kRandRate, kRandSmooth };

const RectI kScopeRect{ 12, 66, 612, 174 };
const RectI kTableRect{ 632, 66, 636, 174 };
const int kDiceW = 84, kDiceH = 20;

inline bool isModSrc(int id) {
    return id >= kMod1Src && id <= kMod6Src && ((id - kMod1Src) % 3) == 0;
}
inline bool isModDst(int id) {
    return id >= kMod1Dst && id <= kMod6Dst && ((id - kMod1Dst) % 3) == 0;
}
inline bool isModAmt(int id) {
    return id >= kMod1Amt && id <= kMod6Amt && ((id - kMod1Amt) % 3) == 0;
}
inline bool isPan(int id) {
    return id == kWtPan || id == kHarshPan || id == kBandPan;
}
inline bool isBipolar(int id) {
    return id == kOctave || id == kWtPitch || id == kWtFine ||
           id == kEnv2Amount || isModAmt(id) || isPan(id) || id == kWidth;
}

// ---------------------------------------------------------------------------
//  Palette.  Paper, ink, one red for clipping -- nothing else.
// ---------------------------------------------------------------------------
inline ARGB argb(unsigned a, unsigned r, unsigned g, unsigned b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}
const ARGB cPaper   = argb(255, 237, 233, 223);
const ARGB cPanel   = argb(255, 250, 248, 242);
const ARGB cInk     = argb(255, 13, 13, 11);
const ARGB cInk70   = argb(255, 58, 57, 52);
const ARGB cInk45   = argb(255, 112, 108, 99);
const ARGB cInk25   = argb(255, 170, 165, 154);
const ARGB cPaper20 = argb(200, 237, 233, 223);
const ARGB cClip    = argb(255, 172, 24, 20);

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
void guiLog(const char* fmt, ...) {
    static int written = 0;
    if (written > 100) return;
    ++written;
    wchar_t path[MAX_PATH];
    const DWORD len = GetTempPathW(MAX_PATH, path);
    if (!len) return;
    lstrcatW(path, L"noiseforge_gui.log");
    FILE* f = _wfopen(path, written == 1 ? L"wb" : L"ab");
    if (!f) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(f, "%s\n", buf);
    std::fclose(f);
}

bool gdiAcquire() {
    static ULONG_PTR token = 0;
    static int refs = 0;
    if (refs == 0) {
        GdiplusStartupInput in;
        if (GdiplusStartup(&token, &in, nullptr) != Ok) return false;
    }
    ++refs;
    return true;
}

void toW(const char* s, wchar_t* d, int n) {
    if (!d || n <= 0) return;
    d[0] = 0;
    if (!s) return;
    // The literals in this file are UTF-8; widening byte by byte turns every
    // multi-byte character into mojibake.
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, d, n) <= 0) d[0] = 0;
    d[n - 1] = 0;
}

// Deterministic ink noise for the paper and the barcode.
inline uint32_t xs(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ---------------------------------------------------------------------------
class NoiseForgeEditor : public IPlugView {
public:
    explicit NoiseForgeEditor(IParamSink* sink) : sink_(sink) {}
    virtual ~NoiseForgeEditor();

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (std::memcmp(iid, IPlugView::iid, 16) == 0 ||
            std::memcmp(iid, FUnknown::iid, 16) == 0) {
            *obj = static_cast<IPlugView*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refs_; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE {
        const uint32 n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE {
        return (type && std::strcmp(type, kPlatformTypeHWND) == 0) ? kResultTrue
                                                                  : kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API removed() SMTG_OVERRIDE;

    tresult PLUGIN_API getSize(ViewRect* size) SMTG_OVERRIDE {
        if (!size) return kInvalidArgument;
        size->left = 0;
        size->top = 0;
        size->right = (int32)surfW_;
        size->bottom = (int32)surfH_;
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect* r) SMTG_OVERRIDE {
        if (!r) return kInvalidArgument;
        const int pxW = (int)(r->right - r->left);
        const int pxH = (int)(r->bottom - r->top);
        if (pxW > 0 && pxH > 0) layoutToWindow(pxW, pxH);
        return kResultOk;
    }
    tresult PLUGIN_API canResize() SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) SMTG_OVERRIDE {
        if (!r) return kInvalidArgument;
        const int w = (int)(r->right - r->left);
        const int h = (int)(r->bottom - r->top);
        int nw = w, nh = h;
        // 1100x688 is the smallest size at which the 8.5 px type is still a
        // readable 7.3 px; below that the panel is just blurred paper.
        if (nw < 1100) nw = 1100;
        if (nh < 688) nh = 688;
        if (nw > 2560) nw = 2560;
        if (nh > 1600) nh = 1600;
        if (nw != w || nh != h) {
            r->right = r->left + (int32)nw;
            r->bottom = r->top + (int32)nh;
        }
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* f) SMTG_OVERRIDE {
        frame_ = f;
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) SMTG_OVERRIDE { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) SMTG_OVERRIDE {
        return kResultFalse;
    }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) SMTG_OVERRIDE {
        return kResultFalse;
    }

    void onParamChanged(int id) {
        if (hwnd_) PostMessageW(hwnd_, WM_APP_PARAM, (WPARAM)id, 0);
    }

private:
    bool initGdi();
    void shutdownGdi();
    void layout();
    bool makeWindow(HWND parent, HINSTANCE inst);

    static LRESULT CALLBACK thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
        NoiseForgeEditor* self = reinterpret_cast<NoiseForgeEditor*>(
            GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->wndProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }
    LRESULT wndProc(HWND h, UINT m, WPARAM w, LPARAM l);

    // ---- painting ---------------------------------------------------------
    void paint(HDC dc);
    void drawBackground(GpGraphics* g);
    void drawHeader(GpGraphics* g);
    void drawScope(GpGraphics* g);
    void drawTable(GpGraphics* g);
    void drawPanel(GpGraphics* g, int group);
    void drawModPanel(GpGraphics* g);
    void drawInfo(GpGraphics* g);
    void drawStatus(GpGraphics* g);
    void drawControl(GpGraphics* g, int id, const RectI& r, float radius, bool hot);
    void drawKnob(GpGraphics* g, int id, const RectI& r, float radius, bool hot);
    void drawPill(GpGraphics* g, int id, const RectI& r, bool hot);
    void drawSlider(GpGraphics* g, int id, const RectI& r, bool hot);
    void panelFrame(GpGraphics* g, const RectI& r, const char* title,
                    const char* note);

    // ---- helpers ----------------------------------------------------------
    void txt(GpGraphics* g, const wchar_t* s, GpFont* f, GpBrush* b, float x,
             float y, float w, float h, int align);
    void txtA(GpGraphics* g, const char* s, GpFont* f, GpBrush* b, float x,
              float y, float w, float h, int align);
    void box(GpGraphics* g, float x, float y, float w, float h, GpBrush* fill,
             GpPen* edge);
    RectI pillRect(int id, const RectI& cell) const;
    double valueOf(int id) const { return sink_ ? sink_->getNormalized(id) : 0.0; }
    void setValue(int id, double v);

    void ensureSurface();
    void layoutToWindow(int pxW, int pxH);
    void fitTo(int pxW, int pxH);
    int  mx(LPARAM l) const { return (int)((GET_X_LPARAM(l) - offX_) / fitScale_); }
    int  my(LPARAM l) const { return (int)((GET_Y_LPARAM(l) - offY_) / fitScale_); }
    bool buildPaper();

    int  hitTest(int x, int y) const;

    // ---- state ------------------------------------------------------------
    IParamSink* sink_ = nullptr;
    IPlugFrame* frame_ = nullptr;
    HWND    hwnd_ = nullptr;
    uint32  refs_ = 1;
    bool    registered_ = false;

    RectI   cell_[kNumParams];
    float   radius_[kNumParams] = {};
    bool    laid_[kNumParams] = {};

    // Offscreen surface: a DIB section shared with the GDI+ bitmap, so the
    // pixels drawn by GDI+ are the pixels BitBlt'ed to the window.
    GpGraphics* gfx_ = nullptr;
    GpBitmap*   bmp_ = nullptr;
    HBITMAP     hbm_ = nullptr;
    HGDIOBJ     oldBmp_ = nullptr;
    HDC         memDC_ = nullptr;
    GpBitmap*   paper_ = nullptr;   // pre-rendered paper texture
    int         surfErr_ = 0;

    GpFontFamily* famDisplay_ = nullptr;
    GpFontFamily* famUI_ = nullptr;
    GpFontFamily* famMono_ = nullptr;
    GpFont* fWord_ = nullptr;
    GpFont* fPanel_ = nullptr;
    GpFont* fLabel_ = nullptr;
    GpFont* fValue_ = nullptr;
    GpFont* fTiny_ = nullptr;
    GpFont* fMono_ = nullptr;

    GpSolidFill* brPaper_ = nullptr;
    GpSolidFill* brPanel_ = nullptr;
    GpSolidFill* brInk_ = nullptr;
    GpSolidFill* brInk70_ = nullptr;
    GpSolidFill* brInk45_ = nullptr;
    GpSolidFill* brInk25_ = nullptr;
    GpSolidFill* brPaper20_ = nullptr;
    GpSolidFill* brClip_ = nullptr;

    GpPen* pFrame_ = nullptr;     // 2 px ink
    GpPen* pThin_ = nullptr;      // 1 px ink
    GpPen* pHair_ = nullptr;      // 1 px ink25
    GpPen* pRing_ = nullptr;      // 2.2 px ink (knobs)
    GpPen* pArc_ = nullptr;       // 3 px ink
    GpPen* pDash_ = nullptr;      // 1 px ink45 dashed
    GpPen* pWhite_ = nullptr;     // 2 px paper (inverted knob)

    GpStringFormat* fmtL_ = nullptr;
    GpStringFormat* fmtC_ = nullptr;
    GpStringFormat* fmtR_ = nullptr;

    int    hot_ = -1;
    int    drag_ = -1;
    int    dragMode_ = 0;
    int    dragX_ = 0, dragY_ = 0;
    double dragVal_ = 0.0;
    int    dragSpan_ = 200;

    float  scope_[512] = {};
    float  bands_[64] = {};
    float  table_[512] = {};
    float  meter_ = 0.0f;
    uint32_t lastGen_ = 0;
    int    diceRetry_ = 0;
    uint32_t seedShown_ = 0;
    int    voices_ = 0;

    float  fitScale_ = 1.0f;
    int    offX_ = 0, offY_ = 0;
    int    surfW_ = kW, surfH_ = kH;
    bool   askedHost_ = false;
    int    tick_ = 0;
};

// ---------------------------------------------------------------------------
//  GDI+ lifetime
// ---------------------------------------------------------------------------
bool NoiseForgeEditor::initGdi() {
    if (!gdiAcquire()) return false;

    GdipCreateFontFamilyFromName(L"Impact", nullptr, &famDisplay_);
    if (!famDisplay_) GdipCreateFontFamilyFromName(L"Arial Black", nullptr, &famDisplay_);
    if (!famDisplay_) GdipCreateFontFamilyFromName(L"Arial", nullptr, &famDisplay_);

    GdipCreateFontFamilyFromName(L"Bahnschrift", nullptr, &famUI_);
    if (!famUI_) GdipCreateFontFamilyFromName(L"Segoe UI", nullptr, &famUI_);
    if (!famUI_) GdipCreateFontFamilyFromName(L"Tahoma", nullptr, &famUI_);

    GdipCreateFontFamilyFromName(L"Consolas", nullptr, &famMono_);
    if (!famMono_) famMono_ = famUI_;

    if (!famDisplay_ || !famUI_) return false;

    GdipCreateFont(famDisplay_, 40.0f, FontStyleRegular, UnitPixel, &fWord_);
    GdipCreateFont(famUI_, 10.0f, FontStyleBold, UnitPixel, &fPanel_);
    GdipCreateFont(famUI_, 9.0f, FontStyleRegular, UnitPixel, &fLabel_);
    GdipCreateFont(famMono_, 10.0f, FontStyleRegular, UnitPixel, &fValue_);
    GdipCreateFont(famUI_, 8.0f, FontStyleRegular, UnitPixel, &fTiny_);
    GdipCreateFont(famMono_, 9.0f, FontStyleRegular, UnitPixel, &fMono_);

    GdipCreateSolidFill(cPaper, &brPaper_);
    GdipCreateSolidFill(cPanel, &brPanel_);
    GdipCreateSolidFill(cInk, &brInk_);
    GdipCreateSolidFill(cInk70, &brInk70_);
    GdipCreateSolidFill(cInk45, &brInk45_);
    GdipCreateSolidFill(cInk25, &brInk25_);
    GdipCreateSolidFill(cPaper20, &brPaper20_);
    GdipCreateSolidFill(cClip, &brClip_);

    GdipCreatePen1(cInk, 2.0f, UnitPixel, &pFrame_);
    GdipCreatePen1(cInk, 1.0f, UnitPixel, &pThin_);
    GdipCreatePen1(cInk25, 1.0f, UnitPixel, &pHair_);
    GdipCreatePen1(cInk, 2.2f, UnitPixel, &pRing_);
    GdipCreatePen1(cInk, 3.0f, UnitPixel, &pArc_);
    GdipCreatePen1(cInk45, 1.0f, UnitPixel, &pDash_);
    GdipCreatePen1(cPaper, 2.2f, UnitPixel, &pWhite_);
    if (pDash_) GdipSetPenDashStyle(pDash_, DashStyleDot);

    GdipCreateStringFormat(0, 0, &fmtL_);
    GdipCreateStringFormat(0, 0, &fmtC_);
    GdipCreateStringFormat(0, 0, &fmtR_);
    GdipSetStringFormatAlign(fmtL_, StringAlignmentNear);
    GdipSetStringFormatAlign(fmtC_, StringAlignmentCenter);
    GdipSetStringFormatAlign(fmtR_, StringAlignmentFar);
    GdipSetStringFormatLineAlign(fmtL_, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(fmtC_, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(fmtR_, StringAlignmentCenter);
    GdipSetStringFormatTrimming(fmtL_, StringTrimmingNone);
    GdipSetStringFormatTrimming(fmtC_, StringTrimmingNone);
    GdipSetStringFormatTrimming(fmtR_, StringTrimmingNone);

    buildPaper();
    return true;
}

bool NoiseForgeEditor::buildPaper() {
    if (paper_) return true;
    if (GdipCreateBitmapFromScan0(kW, kH, 0, PixelFormat32bppPARGB, nullptr,
                                  &paper_) != Ok ||
        !paper_) {
        paper_ = nullptr;
        return false;
    }
    BitmapData bd{};
    GpRect rc{ 0, 0, kW, kH };
    if (GdipBitmapLockBits(paper_, &rc, ImageLockModeWrite,
                           PixelFormat32bppPARGB, &bd) != Ok) {
        GdipDisposeImage(paper_);
        paper_ = nullptr;
        return false;
    }
    uint32_t s = 0xC0FFEE11u;
    for (int y = 0; y < kH; ++y) {
        uint8_t* row = (uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < kW; ++x) {
            const int n = (int)(xs(s) >> 28);           // 0..15
            int v = 236 + (n - 7) / 3;                  // speckle
            if ((y & 3) == 0) --v;                      // faint scan line
            if ((xs(s) & 2047u) == 0u) v -= 22;         // toner specks
            if (v < 208) v = 208;
            if (v > 244) v = 244;
            uint8_t* p = row + (size_t)x * 4;
            p[0] = (uint8_t)(v - 4);                    // B
            p[1] = (uint8_t)(v - 2);                    // G
            p[2] = (uint8_t)v;                          // R
            p[3] = 255;
        }
    }
    GdipBitmapUnlockBits(paper_, &bd);
    return true;
}

void NoiseForgeEditor::shutdownGdi() {
    if (gfx_) { GdipDeleteGraphics(gfx_); gfx_ = nullptr; }
    if (bmp_) { GdipDisposeImage(bmp_); bmp_ = nullptr; }
    if (paper_) { GdipDisposeImage(paper_); paper_ = nullptr; }
    if (memDC_) {
        // put the previous bitmap back, or DeleteObject on the DIB silently fails
        if (oldBmp_) { SelectObject(memDC_, oldBmp_); oldBmp_ = nullptr; }
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (hbm_) { DeleteObject(hbm_); hbm_ = nullptr; }

    GpFont* fonts[] = { fWord_, fPanel_, fLabel_, fValue_, fTiny_, fMono_ };
    for (int i = 0; i < 6; ++i)
        if (fonts[i]) GdipDeleteFont(fonts[i]);
    fWord_ = fPanel_ = fLabel_ = fValue_ = fTiny_ = fMono_ = nullptr;

    if (famDisplay_) { GdipDeleteFontFamily(famDisplay_); famDisplay_ = nullptr; }
    if (famUI_) { GdipDeleteFontFamily(famUI_); famUI_ = nullptr; }
    if (famMono_ && famMono_ != famUI_) GdipDeleteFontFamily(famMono_);
    famMono_ = nullptr;

    GpSolidFill* brs[] = { brPaper_, brPanel_, brInk_, brInk70_, brInk45_,
                           brInk25_, brPaper20_, brClip_ };
    for (int i = 0; i < 8; ++i)
        if (brs[i]) GdipDeleteBrush(brs[i]);
    brPaper_ = brPanel_ = brInk_ = brInk70_ = brInk45_ = nullptr;
    brInk25_ = brPaper20_ = brClip_ = nullptr;

    GpPen* pens[] = { pFrame_, pThin_, pHair_, pRing_, pArc_, pDash_, pWhite_ };
    for (int i = 0; i < 7; ++i)
        if (pens[i]) GdipDeletePen(pens[i]);
    pFrame_ = pThin_ = pHair_ = pRing_ = pArc_ = pDash_ = pWhite_ = nullptr;

    GpStringFormat* fmts[] = { fmtL_, fmtC_, fmtR_ };
    for (int i = 0; i < 3; ++i)
        if (fmts[i]) GdipDeleteStringFormat(fmts[i]);
    fmtL_ = fmtC_ = fmtR_ = nullptr;

    surfErr_ = 0;
}

NoiseForgeEditor::~NoiseForgeEditor() {
    if (sink_) sink_->setActiveEditor(nullptr);
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    registered_ = false;
    shutdownGdi();
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------
void NoiseForgeEditor::layout() {
    for (int i = 0; i < kNumParams; ++i) laid_[i] = false;

    for (int g = 0; g < kNumGroups; ++g) {
        if (g == kGrpMod) continue;
        const RectI& p = kPanel[g];
        const int n = kCtlCount[g];
        const int cols = kCtlCols[g];
        if (n <= 0 || cols <= 0) continue;
        const int rows = (n + cols - 1) / cols;

        const int x0 = p.x + 8;
        const int y0 = p.y + 26;
        const int w = p.w - 16;
        const int h = p.h - 34;
        const int cw = w / cols;
        const int ch = h / rows;

        for (int i = 0; i < n; ++i) {
            const int id = kCtl[g][i];
            cell_[id] = RectI{ x0 + (i % cols) * cw, y0 + (i / cols) * ch, cw, ch };
            float rad = std::min((float)cw * 0.30f, (float)ch * 0.26f);
            if (rad < 11.0f) rad = 11.0f;
            if (rad > 26.0f) rad = 26.0f;
            radius_[id] = rad;
            laid_[id] = true;
        }
    }

    // ---- MODULATION --------------------------------------------------------
    {
        const RectI& p = kPanel[kGrpMod];
        const int gx = p.x + 8;
        const int gy = p.y + 26;
        const int gw = p.w - 16;          // 352
        const int cellW = gw / 4;         // 88
        const int cellH = 88;
        for (int i = 0; i < 8; ++i) {
            const int id = kModGlobals[i];
            cell_[id] = RectI{ gx + (i % 4) * cellW, gy + (i / 4) * cellH, cellW,
                               cellH };
            float rad = std::min((float)cellW * 0.30f, (float)cellH * 0.26f);
            if (rad < 12.0f) rad = 12.0f;
            if (rad > 24.0f) rad = 24.0f;
            radius_[id] = rad;
            laid_[id] = true;
        }

        // Six route rows. panel 368x518: 26 title + 176 globals + 16 gap
        // + 14 header + 6*44 rows + 12 bottom = 508, fits.
        const int slotY = gy + 2 * cellH + 20;
        for (int s = 0; s < 6; ++s) {
            const int y = slotY + s * 44;
            const int srcId = kMod1Src + s * 3;
            const int dstId = kMod1Dst + s * 3;
            const int amtId = kMod1Amt + s * 3;
            cell_[srcId] = RectI{ gx + 24, y + 9, 108, 22 };
            cell_[dstId] = RectI{ gx + 140, y + 9, 108, 22 };
            cell_[amtId] = RectI{ gx + 254, y + 10, 64, 18 };
            laid_[srcId] = laid_[dstId] = laid_[amtId] = true;
        }
    }
}

// ---------------------------------------------------------------------------
//  Window
// ---------------------------------------------------------------------------
bool NoiseForgeEditor::makeWindow(HWND parent, HINSTANCE inst) {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = thunk;
        wc.hInstance = inst;
        wc.style = CS_DBLCLKS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"NoiseForgePanel";
        if (!RegisterClassExW(&wc)) return false;
        reg = true;
    }
    wchar_t title[128];
    toW("NOISEFORGE", title, 128);
    hwnd_ = CreateWindowExW(0, L"NoiseForgePanel", title,
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN |
                                WS_CLIPSIBLINGS,
                            0, 0, kW, kH, parent, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hwnd_, SW_SHOWNA);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return true;
}

tresult PLUGIN_API NoiseForgeEditor::attached(void* parent, FIDString type) {
    if (!parent || !type || std::strcmp(type, kPlatformTypeHWND) != 0)
        return kResultFalse;

    HMODULE inst = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&gdiAcquire), &inst);

    layout();
    if (!makeWindow(static_cast<HWND>(parent),
                    inst ? inst : GetModuleHandleW(nullptr)))
        return kResultFalse;

    shutdownGdi();
    if (!initGdi()) guiLog("initGdi FAILED");

    RECT pr{};
    GetClientRect(static_cast<HWND>(parent), &pr);
    int pw = (int)(pr.right - pr.left);
    int ph = (int)(pr.bottom - pr.top);

    if (pw < 300 || ph < 200) {
        const int32 wantW = kW, wantH = kH;
        if (frame_) {
            ViewRect want{ 0, 0, wantW, wantH };
            frame_->resizeView(this, &want);
            askedHost_ = true;
        }
        pw = wantW;
        ph = wantH;
    }
    layoutToWindow(pw, ph);

    SetTimer(hwnd_, 1, 40, nullptr);

    if (sink_) {
        sink_->setActiveEditor(this);
        registered_ = true;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return kResultOk;
}

tresult PLUGIN_API NoiseForgeEditor::removed() {
    if (hwnd_) KillTimer(hwnd_, 1);
    if (sink_) sink_->setActiveEditor(nullptr);
    registered_ = false;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    return kResultOk;
}

// ---------------------------------------------------------------------------
//  Text / shape helpers
// ---------------------------------------------------------------------------
void NoiseForgeEditor::txt(GpGraphics* g, const wchar_t* s, GpFont* f, GpBrush* b,
                           float x, float y, float w, float h, int align) {
    if (!s || !*s || !f || !b) return;
    GpStringFormat* fmt = (align == 1) ? fmtC_ : (align == 2) ? fmtR_ : fmtL_;
    RectF r(x, y, w, h);
    GdipDrawString(g, s, (INT)wcslen(s), f, &r, fmt, b);
}

void NoiseForgeEditor::txtA(GpGraphics* g, const char* s, GpFont* f, GpBrush* b,
                            float x, float y, float w, float h, int align) {
    wchar_t wbuf[192];
    toW(s, wbuf, 192);
    txt(g, wbuf, f, b, x, y, w, h, align);
}

void NoiseForgeEditor::box(GpGraphics* g, float x, float y, float w, float h,
                           GpBrush* fill, GpPen* edge) {
    if (fill) GdipFillRectangle(g, fill, x, y, w, h);
    if (edge) GdipDrawRectangle(g, edge, x, y, w, h);
}

RectI NoiseForgeEditor::pillRect(int, const RectI& c) const {
    const int bw = std::min(c.w - 8, 104);
    const float cy = (float)c.y + (float)c.h * 0.38f;
    RectI r{ (int)(c.x + (c.w - bw) * 0.5f), (int)(cy - 11.0f), bw, 22 };
    return r;
}

// ---------------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------------
void NoiseForgeEditor::panelFrame(GpGraphics* g, const RectI& r, const char* title,
                                  const char* note) {
    box(g, (float)r.x, (float)r.y, (float)r.w, (float)r.h, brPanel_, nullptr);
    box(g, (float)r.x, (float)r.y, (float)r.w, 20, brInk_, nullptr);

    // a hairline inside the ink bar keeps the "printed card" feel
    box(g, (float)r.x + 1, (float)r.y + 19, (float)r.w - 2, 1, nullptr, pHair_);

    txtA(g, title, fPanel_, brPaper_, (float)r.x + 7, (float)r.y + 2,
         (float)r.w - 14, 16, 0);
    if (note && note[0])
        txtA(g, note, fTiny_, brPaper20_, (float)r.x + 7, (float)r.y + 3,
             (float)r.w - 14, 14, 2);

    GdipDrawRectangle(g, pFrame_, (float)r.x + 1, (float)r.y + 1,
                      (float)r.w - 2, (float)r.h - 2);
}

void NoiseForgeEditor::drawBackground(GpGraphics* g) {
    if (paper_) GdipDrawImage(g, paper_, 0.0f, 0.0f);

    // toner smudges -- fixed positions, very low alpha
    static const float sx[5] = { -60, 250, 470, 760, 1060 };
    static const float sy[5] = { 120, 640, 20, 560, 700 };
    static const float sr[5] = { 240, 200, 180, 260, 220 };
    GpSolidFill* smudge = nullptr;
    GdipCreateSolidFill(argb(9, 30, 28, 22), &smudge);
    if (smudge) {
        for (int i = 0; i < 5; ++i)
            GdipFillEllipse(g, smudge, sx[i], sy[i], sr[i], sr[i] * 0.75f);
        GdipDeleteBrush(smudge);
    }

    // registration crosses at the corners
    const float m = 16.0f, len = 7.0f;
    const float cx[2] = { m, (float)kW - m }, cy[2] = { m, (float)kH - m };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            GdipDrawLine(g, pThin_, cx[i] - len, cy[j], cx[i] + len, cy[j]);
            GdipDrawLine(g, pThin_, cx[i], cy[j] - len, cx[i], cy[j] + len);
        }

    GdipDrawRectangle(g, pFrame_, 8.0f, 8.0f, (float)kW - 16, (float)kH - 16);
    GdipDrawRectangle(g, pHair_, 12.0f, 12.0f, (float)kW - 24, (float)kH - 24);
}

void NoiseForgeEditor::drawHeader(GpGraphics* g) {
    // wordmark: NOISE solid, FORGE knocked out of a black block right after it
    txt(g, L"NOISE", fWord_, brInk_, 18.0f, 0.0f, 220.0f, 42.0f, 0);
    // Fixed block position: measuring Impact's ink width is font-dependent and
    // one bad metric puts the block on top of the wordmark.
    const float bx = 176.0f;
    box(g, bx, 5.0f, 158.0f, 34.0f, brInk_, nullptr);
    txt(g, L"FORGE", fWord_, brPaper_, bx + 6.0f, 0.0f, 150.0f, 42.0f, 0);

    txtA(g, "TWO NOISE ENGINES / WAVETABLE / MOD MATRIX", fTiny_, brInk45_,
         20.0f, 42.0f, 420.0f, 10.0f, 0);

    char buf[80];
    std::snprintf(buf, sizeof(buf), "VOICES %d/%d", voices_, (int)kMaxVoices);
    txtA(g, buf, fMono_, brInk_, 560.0f, 8.0f, 150.0f, 13.0f, 0);
    std::snprintf(buf, sizeof(buf), "SEED %08X", seedShown_);
    txtA(g, buf, fMono_, brInk70_, 560.0f, 24.0f, 190.0f, 13.0f, 0);

    txtA(g, "VST3 / 64-BIT / 71 PARAMETERS", fTiny_, brInk45_, 880.0f, 8.0f,
         270.0f, 12.0f, 2);
    std::snprintf(buf, sizeof(buf), "BUILD %s %s", __DATE__, __TIME__);
    txtA(g, buf, fTiny_, brInk25_, 880.0f, 22.0f, 270.0f, 10.0f, 2);

    box(g, 1162.0f, 36.0f, 106.0f, 16.0f, brInk_, nullptr);
    txtA(g, "NF-01  SERIES 01", fTiny_, brPaper_, 1164.0f, 37.0f, 102.0f, 14.0f, 1);

    box(g, 0.0f, 58.0f, (float)kW, 3.0f, brInk_, nullptr);
    box(g, 0.0f, 62.0f, (float)kW, 1.0f, brInk25_, nullptr);
}

void NoiseForgeEditor::drawScope(GpGraphics* g) {
    const RectI& r = kScopeRect;
    panelFrame(g, r, "OUTPUT SCOPE", nullptr);

    const float ix = (float)r.x + 8, iy = (float)r.y + 26;
    const float iw = (float)r.w - 16;
    const float waveH = 84.0f;
    const float specY = iy + waveH + 6;
    const float specH = (float)(r.y + r.h) - specY - 8;

    // peak readout in the title bar
    {
        const float db = 20.0f * std::log10(meter_ > 1e-6f ? meter_ : 1e-6f);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%+.1f dB", db < -60.0f ? -60.0f : db);
        if (meter_ > 0.98f) {
            box(g, (float)r.x + r.w - 74, (float)r.y + 3, 62.0f, 14.0f, brClip_,
                nullptr);
            txtA(g, "CLIP", fTiny_, brPaper_, (float)r.x + r.w - 74,
                 (float)r.y + 4, 62.0f, 14.0f, 1);
        } else {
            txtA(g, buf, fMono_, brPaper20_, (float)r.x + r.w - 116,
                 (float)r.y + 3, 108.0f, 14.0f, 2);
        }
    }

    box(g, ix, iy, iw, waveH, brPaper_, nullptr);
    for (int i = 1; i < 12; ++i) {   // dotted grid
        const float gx = ix + iw * (float)i / 12.0f;
        for (float gy = iy + 4; gy < iy + waveH - 3; gy += 6.0f)
            GdipFillRectangle(g, brInk25_, gx, gy, 1.0f, 1.0f);
    }
    box(g, ix, iy + waveH * 0.5f, iw, 1.0f, brInk25_, nullptr);
    box(g, ix, iy, iw, waveH, nullptr, pHair_);

    if (sink_) sink_->copyScope(scope_, 512);

    {
        GpPointF pts[512];
        const float mid = iy + waveH * 0.5f;
        const float amp = waveH * 0.44f;
        for (int i = 0; i < 512; ++i) {
            pts[i].X = ix + iw * (float)i / 511.0f;
            float v = scope_[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pts[i].Y = mid - v * amp;
        }
        GdipDrawLines(g, pArc_, pts, 512);
        if (meter_ < 0.0008f)
            txtA(g, "SILENT", fTiny_, brInk25_, ix, mid - 8.0f, iw, 16.0f, 1);
    }

    // spectrum: 48 log-spaced bands of the same window
    box(g, ix, specY, iw, specH, brPaper_, nullptr);
    box(g, ix, specY, iw, specH, nullptr, pHair_);
    const double sr = sink_ ? sink_->sampleRate() : 48000.0;
    const float bw = iw / 48.0f;
    for (int b = 0; b < 48; ++b) {
        const double f0 = 20.0 * std::pow(1000.0, (double)b / 48.0);
        const double wv = 6.28318530718 * f0 / sr;
        const double cw = std::cos(wv);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < 256; ++i) {
            const double s0 = (double)scope_[i] + 2.0 * cw * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - 2.0 * cw * s1 * s2;
        float lvl = (float)(std::log10(power + 1e-9) * 0.13 + 0.52);
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        if (lvl > bands_[b]) bands_[b] = bands_[b] * 0.45f + lvl * 0.55f;
        else bands_[b] *= 0.86f;
        const float bh = (specH - 3.0f) * bands_[b];
        if (bh > 0.5f)
            box(g, ix + b * bw + bw * 0.18f, specY + specH - 1.0f - bh,
                bw * 0.64f, bh, brInk_, nullptr);
    }
    box(g, ix, specY + specH - 1.0f, iw, 1.0f, brInk_, nullptr);

    txtA(g, "20", fTiny_, brInk45_, ix + 2, specY + specH - 11.0f, 20.0f, 10.0f, 0);
    txtA(g, "200", fTiny_, brInk45_, ix + iw * 0.333f, specY + specH - 11.0f,
         24.0f, 10.0f, 1);
    txtA(g, "2k", fTiny_, brInk45_, ix + iw * 0.667f, specY + specH - 11.0f, 24.0f,
         10.0f, 1);
    txtA(g, "20k", fTiny_, brInk45_, ix + iw - 26.0f, specY + specH - 11.0f, 24.0f,
         10.0f, 2);
}

void NoiseForgeEditor::drawTable(GpGraphics* g) {
    const RectI& r = kTableRect;
    panelFrame(g, r, "WAVETABLE", nullptr);

    const int tableId = (int)mapParam(kWtTable, (float)valueOf(kWtTable));
    const char* nm = sink_ ? sink_->tableName(tableId) : "";
    if (nm && nm[0]) {
        const float tw = 92.0f;
        box(g, (float)r.x + r.w - tw - 8, (float)r.y + 3, tw, 14.0f, brPaper_,
            nullptr);
        txtA(g, nm, fTiny_, brInk_, (float)r.x + r.w - tw - 8, (float)r.y + 4, tw,
             13.0f, 1);
    }

    const float ix = (float)r.x + 8, iy = (float)r.y + 26;
    const float iw = (float)r.w - 16, ih = 104.0f;

    box(g, ix, iy, iw, ih, brPaper_, nullptr);
    for (int i = 1; i < 12; ++i) {
        const float gx = ix + iw * (float)i / 12.0f;
        for (float gy = iy + 4; gy < iy + ih - 3; gy += 6.0f)
            GdipFillRectangle(g, brInk25_, gx, gy, 1.0f, 1.0f);
    }
    box(g, ix, iy + ih * 0.5f, iw, 1.0f, brInk25_, nullptr);
    box(g, ix, iy, iw, ih, nullptr, pHair_);

    const float pos = (float)valueOf(kWtPos);
    if (sink_) sink_->copyTablePreview(tableId, pos, table_, 512);

    {
        GpPointF pts[512];
        const float mid = iy + ih * 0.5f;
        const float amp = ih * 0.43f;
        for (int i = 0; i < 512; ++i) {
            pts[i].X = ix + iw * (float)i / 511.0f;
            float v = table_[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pts[i].Y = mid - v * amp;
        }
        GdipDrawLines(g, pArc_, pts, 512);
    }

    // frame marker
    {
        const float mxs = ix + iw * pos;
        GdipDrawLine(g, pDash_, mxs, iy + 3.0f, mxs, iy + ih - 3.0f);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "FRAME %d/%d",
                      (int)(pos * 7.0f + 0.5f) + 1, (int)kFrames);
        txtA(g, buf, fTiny_, brInk45_, ix + 4.0f, iy + 3.0f, 100.0f, 10.0f, 0);
    }

    // DICE
    const RectI dice{ r.x + r.w - kDiceW - 10, r.y + r.h - kDiceH - 8, kDiceW,
                      kDiceH };
    {
        const bool hot = (hot_ == -100);
        box(g, (float)dice.x, (float)dice.y, (float)dice.w, (float)dice.h,
            hot ? brPaper_ : brInk_, hot ? pFrame_ : nullptr);
        txtA(g, "DICE", fPanel_, hot ? brInk_ : brPaper_, (float)dice.x,
             (float)dice.y, (float)dice.w, (float)dice.h, 1);
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "SEED %08X   /   D = NEW BANK", seedShown_);
    txtA(g, buf, fTiny_, brInk45_, (float)r.x + 10, (float)r.y + r.h - 20.0f,
         380.0f, 12.0f, 0);
}

void NoiseForgeEditor::drawControl(GpGraphics* g, int id, const RectI& r,
                                   float radius, bool hot) {
    if (isModAmt(id)) {
        drawSlider(g, id, r, hot);
        return;
    }
    if (isModSrc(id) || isModDst(id)) {
        drawPill(g, id, r, hot);
        return;
    }
    if (kParams[id].kind == kKnob) {
        drawKnob(g, id, r, radius, hot);
        return;
    }
    drawPill(g, id, pillRect(id, r), hot);
    {
        const float cy = (float)r.y + (float)r.h * 0.38f;
        txtA(g, kParams[id].label, fTiny_, hot ? brInk_ : brInk45_, (float)r.x,
             cy + radius + 3.0f, (float)r.w, 10.0f, 1);
    }
}

void NoiseForgeEditor::drawKnob(GpGraphics* g, int id, const RectI& r, float radius,
                                bool hot) {
    const ParamMeta& m = kParams[id];
    const double n = valueOf(id);

    const float cx = (float)r.x + (float)r.w * 0.5f;
    const float cy = (float)r.y + (float)r.h * 0.38f;
    const float R = radius;

    const bool bipolar = isBipolar(id);
    float start = 135.0f, sweep = 270.0f * (float)n;
    if (bipolar) {
        start = 270.0f;
        sweep = 270.0f * ((float)n - 0.5f);
    }

    // ticks
    for (int i = 0; i <= 12; i += 3) {
        const float ang = (135.0f + 270.0f * i / 12.0f) * 0.01745329f;
        const float c = std::cos(ang), s = std::sin(ang);
        GdipDrawLine(g, pThin_, cx + (R + 5) * c, cy + (R + 5) * s,
                     cx + (R + 9) * c, cy + (R + 9) * s);
    }

    // disc: inverted when hovered, so the value ring stays the only colour
    if (hot) {
        GdipFillEllipse(g, brInk_, cx - R, cy - R, R * 2.0f, R * 2.0f);
    } else {
        GdipFillEllipse(g, brPaper_, cx - R, cy - R, R * 2.0f, R * 2.0f);
        GdipDrawEllipse(g, pRing_, cx - R, cy - R, R * 2.0f, R * 2.0f);
        GdipDrawEllipse(g, pHair_, cx - R + 4, cy - R + 4, (R - 4) * 2.0f,
                        (R - 4) * 2.0f);
    }

    // value arc, outside the disc
    if (std::fabs(sweep) > 0.4f)
        GdipDrawArc(g, hot ? pWhite_ : pArc_, cx - R - 1.5f, cy - R - 1.5f,
                    (R + 1.5f) * 2.0f, (R + 1.5f) * 2.0f, start, sweep);

    // pointer
    {
        const float ang = (start + sweep) * 0.01745329f;
        const float c = std::cos(ang), s = std::sin(ang);
        GdipDrawLine(g, hot ? pWhite_ : pThin_, cx + (R * 0.18f) * c,
                     cy + (R * 0.18f) * s, cx + (R - 2.0f) * c,
                     cy + (R - 2.0f) * s);
        GdipFillEllipse(g, hot ? brPaper_ : brInk_, cx - 1.6f, cy - 1.6f, 3.2f,
                        3.2f);
    }

    txtA(g, m.label, fTiny_, hot ? brInk_ : brInk45_, (float)r.x,
         cy + R + 3.0f, (float)r.w, 10.0f, 1);

    char buf[32];
    formatParam(id, (float)n, buf, (int)sizeof(buf));
    txtA(g, buf, fValue_, hot ? brInk_ : brInk70_, (float)r.x, cy + R + 12.0f,
         (float)r.w, 12.0f, 1);
}

void NoiseForgeEditor::drawPill(GpGraphics* g, int id, const RectI& r, bool hot) {
    const ParamMeta& m = kParams[id];
    const double n = valueOf(id);
    const bool isChoice = (m.kind == kChoice);
    // A choice is a solid stamp unless it is the "OFF" slot of a mod route;
    // a switch is a stamp only while it is on. Hover flips either.
    const bool offSlot = (isModSrc(id) || isModDst(id)) && n <= 0.001;
    const bool printed = (m.kind == kSwitch) ? (n >= 0.5) : !offSlot;
    const bool inverse = printed != hot;
    box(g, (float)r.x, (float)r.y, (float)r.w, (float)r.h,
        inverse ? brInk_ : brPaper_, inverse ? nullptr : pFrame_);
    if (inverse)
        GdipDrawRectangle(g, pWhite_, (float)r.x + 3, (float)r.y + 3,
                          (float)r.w - 6, (float)r.h - 6);

    if (isChoice) {
        const int idx = (int)(mapParam(id, (float)n) + 0.5f);
        const char* t = choiceText(id, idx);
        char buf[40];
        int i = 0;
        for (; t[i] && t[i] != '|' && i < 39; ++i) buf[i] = t[i];
        buf[i] = 0;
        txtA(g, buf, fTiny_, inverse ? brPaper_ : brInk_, (float)r.x, (float)r.y,
             (float)r.w, (float)r.h, 1);
    } else {
        txtA(g, printed ? "ON" : "OFF", fTiny_, inverse ? brPaper_ : brInk_,
             (float)r.x, (float)r.y, (float)r.w, (float)r.h, 1);
    }
}

void NoiseForgeEditor::drawSlider(GpGraphics* g, int id, const RectI& r, bool hot) {
    const double n = valueOf(id);
    const float cy = (float)r.y + (float)r.h * 0.5f;
    const float x0 = (float)r.x, w = (float)r.w;
    const float mid = x0 + w * 0.5f;
    const float px = x0 + w * (float)n;

    box(g, x0, cy - 1.0f, w, 2.0f, brInk_, nullptr);
    box(g, mid - 0.5f, cy - 6.0f, 1.0f, 12.0f, brInk45_, nullptr);
    box(g, x0, cy - 4.0f, 1.0f, 8.0f, brInk45_, nullptr);
    box(g, x0 + w - 1.0f, cy - 4.0f, 1.0f, 8.0f, brInk45_, nullptr);

    box(g, px - 4.0f, cy - 7.0f, 8.0f, 14.0f, hot ? brPaper_ : brInk_,
        hot ? pFrame_ : nullptr);

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%+.2f", mapParam(id, (float)n));
    txtA(g, buf, fTiny_, hot ? brInk_ : brInk45_, x0 + w + 4.0f, (float)r.y - 2.0f,
         36.0f, (float)r.h + 4.0f, 0);
}

void NoiseForgeEditor::drawPanel(GpGraphics* g, int group) {
    const RectI& r = kPanel[group];
    const char* note = nullptr;
    if (group == kGrpHarsh)
        note = "BROADBAND";
    else if (group == kGrpBand)
        note = "SPECTRAL";
    else if (group == kGrpEnv)
        note = "MIN 8 ms A / 12 ms R";
    else if (group == kGrpWt)
        note = "TABLE + MIP";
    else if (group == kGrpVoice)
        note = "GLIDE / VEL";
    else if (group == kGrpOut)
        note = "MASTER";
    panelFrame(g, r, kGroupTitles[group], note);

    const int n = kCtlCount[group];
    for (int i = 0; i < n; ++i) {
        const int id = kCtl[group][i];
        if (!laid_[id]) continue;
        drawControl(g, id, cell_[id], radius_[id], id == hot_);
    }
}

void NoiseForgeEditor::drawModPanel(GpGraphics* g) {
    const RectI& r = kPanel[kGrpMod];
    panelFrame(g, r, kGroupTitles[kGrpMod], "6 ROUTES");

    for (int i = 0; i < 8; ++i) {
        const int id = kModGlobals[i];
        if (laid_[id]) drawControl(g, id, cell_[id], radius_[id], id == hot_);
    }

    // divider + column heads
    const int headY = r.y + 26 + 2 * 88 + 4;
    box(g, (float)r.x + 8, (float)headY, (float)r.w - 16, 2.0f, brInk_, nullptr);
    txtA(g, "SOURCE", fTiny_, brInk45_, (float)r.x + 28, (float)headY + 4, 100.0f,
         10.0f, 0);
    txtA(g, "DESTINATION", fTiny_, brInk45_, (float)r.x + 144, (float)headY + 4,
         100.0f, 10.0f, 0);
    txtA(g, "AMOUNT", fTiny_, brInk45_, (float)r.x + 258, (float)headY + 4, 80.0f,
         10.0f, 0);

    for (int s = 0; s < 6; ++s) {
        const int srcId = kMod1Src + s * 3;
        const int dstId = kMod1Dst + s * 3;
        const int amtId = kMod1Amt + s * 3;
        char num[8];
        std::snprintf(num, sizeof(num), "%d", s + 1);
        txtA(g, num, fPanel_, brInk25_, (float)cell_[srcId].x - 20.0f,
             (float)cell_[srcId].y, 16.0f, 22.0f, 1);
        drawControl(g, srcId, cell_[srcId], 0.0f, srcId == hot_);
        drawControl(g, dstId, cell_[dstId], 0.0f, dstId == hot_);
        drawControl(g, amtId, cell_[amtId], 0.0f, amtId == hot_);
        box(g, (float)r.x + 8, (float)cell_[srcId].y + 34.0f,
            (float)r.w - 16, 1.0f, brInk25_, nullptr);
    }
}

void NoiseForgeEditor::drawInfo(GpGraphics* g) {
    const RectI& r = kInfoRect;
    box(g, (float)r.x, (float)r.y, (float)r.w, (float)r.h, brPanel_, nullptr);
    GdipDrawRectangle(g, pFrame_, (float)r.x + 1, (float)r.y + 1,
                      (float)r.w - 2, (float)r.h - 2);

    txtA(g, "NF-01", fPanel_, brInk_, (float)r.x + 8, (float)r.y + 4, 60.0f, 14.0f,
         0);
    txtA(g, "NOISE INSTRUMENT", fTiny_, brInk45_, (float)r.x + 56, (float)r.y + 5,
         160.0f, 12.0f, 0);

    // barcode from the live seed
    uint32_t s = seedShown_ | 1u;
    float x = (float)r.x + 8;
    const float bcodeY = (float)r.y + 22;
    while (x < (float)r.x + (float)r.w - 12) {
        const float w = 1.0f + (float)(xs(s) % 3u) * 1.5f;
        if (xs(s) & 1u) box(g, x, bcodeY, w, 26.0f, brInk_, nullptr);
        x += w + 1.0f + (float)(xs(s) % 2u);
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "SEED %08X   VOICES %d", seedShown_, voices_);
    txtA(g, buf, fMono_, brInk70_, (float)r.x + 8, (float)r.y + 52.0f, 200.0f,
         12.0f, 0);
    txtA(g, "64-BIT VST3", fTiny_, brInk45_, (float)r.x + 150, (float)r.y + 62.0f,
         (float)r.w - 158, 10.0f, 2);
}

void NoiseForgeEditor::drawStatus(GpGraphics* g) {
    box(g, 0.0f, (float)kH - 30.0f, (float)kW, 30.0f, brPaper_, nullptr);
    box(g, 0.0f, (float)kH - 31.0f, (float)kW, 3.0f, brInk_, nullptr);

    const int id = (drag_ >= 0) ? drag_ : hot_;
    if (id >= 0 && id < kNumParams) {
        char buf[64];
        formatParam(id, (float)valueOf(id), buf, (int)sizeof(buf));
        txtA(g, kParams[id].name, fPanel_, brInk_, 20.0f, (float)kH - 25.0f,
             280.0f, 18.0f, 0);
        txtA(g, buf, fMono_, brInk70_, 310.0f, (float)kH - 24.0f, 200.0f, 16.0f, 0);
    } else {
        txtA(g, "DRAG A KNOB   /   WHEEL   /   SHIFT = FINE   /   "
                "DOUBLE-CLICK = RESET",
             fTiny_, brInk45_, 20.0f, (float)kH - 24.0f, 760.0f, 16.0f, 0);
    }

    txtA(g, "D = NEW WAVETABLE BANK", fTiny_, brInk45_, 860.0f,
         (float)kH - 24.0f, 260.0f, 16.0f, 2);
    txtA(g, "NF-01", fMono_, brInk_, 1160.0f, (float)kH - 24.0f, 96.0f, 16.0f, 2);
}

void NoiseForgeEditor::ensureSurface() {
    if (surfErr_) return;
    if (!memDC_) {
        memDC_ = CreateCompatibleDC(nullptr);
        if (!memDC_) { surfErr_ = -1; return; }
    }
    if (bmp_ && gfx_) return;

    // DIB section shared with GDI+: one surface, no per-frame copy.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    hbm_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm_ || !bits) { surfErr_ = -2; return; }
    oldBmp_ = SelectObject(memDC_, hbm_);

    if (GdipCreateBitmapFromScan0(kW, kH, kW * 4, PixelFormat32bppPARGB,
                                  (BYTE*)bits, &bmp_) != Ok ||
        !bmp_) {
        bmp_ = nullptr;
        surfErr_ = -3;
        return;
    }
    if (GdipGetImageGraphicsContext(bmp_, &gfx_) != Ok || !gfx_) {
        gfx_ = nullptr;
        surfErr_ = -4;
        return;
    }
}

void NoiseForgeEditor::fitTo(int pxW, int pxH) {
    if (pxW <= 0 || pxH <= 0) return;
    const float sx = (float)pxW / (float)kW;
    const float sy = (float)pxH / (float)kH;
    float s = (sx < sy) ? sx : sy;
    if (s < 0.3f) s = 0.3f;
    if (s > 4.0f) s = 4.0f;
    fitScale_ = s;
    offX_ = (int)((pxW - (float)kW * s) * 0.5f);
    offY_ = (int)((pxH - (float)kH * s) * 0.5f);
}

void NoiseForgeEditor::layoutToWindow(int pxW, int pxH) {
    if (pxW <= 0 || pxH <= 0) return;
    surfW_ = pxW;
    surfH_ = pxH;
    fitTo(pxW, pxH);
    if (hwnd_)
        SetWindowPos(hwnd_, nullptr, 0, 0, pxW, pxH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ensureSurface();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NoiseForgeEditor::paint(HDC dc) {
    if (!dc) return;

    if (hwnd_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if ((rc.right < 120 || rc.bottom < 120) && frame_ && !askedHost_) {
            askedHost_ = true;
            ViewRect want{ 0, 0, kW, kH };
            frame_->resizeView(this, &want);
        }
        if (rc.right > 0 && rc.bottom > 0) fitTo(rc.right, rc.bottom);
    }

    ensureSurface();
    if (!gfx_ || !bmp_ || !memDC_) {
        RECT rc{};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        HBRUSH br = CreateSolidBrush(RGB(237, 233, 223));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(13, 13, 11));
        DrawTextA(dc, "NOISEFORGE - GDI FALLBACK", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    if (sink_) {
        meter_ = sink_->peak();
        voices_ = sink_->activeVoices();
        seedShown_ = sink_->tableSeed();
    }

    GdipSetSmoothingMode(gfx_, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(gfx_, TextRenderingHintAntiAliasGridFit);
    GdipGraphicsClear(gfx_, cPaper);

    drawBackground(gfx_);
    drawHeader(gfx_);
    drawScope(gfx_);
    drawTable(gfx_);
    for (int g = 0; g < kNumGroups; ++g) {
        if (g == kGrpMod) continue;
        drawPanel(gfx_, g);
    }
    drawModPanel(gfx_);
    drawInfo(gfx_);
    drawStatus(gfx_);

    GdipFlush(gfx_, FlushIntentionSync);

    const int dw = (int)((float)kW * fitScale_ + 0.5f);
    const int dh = (int)((float)kH * fitScale_ + 0.5f);
    bool drawn = false;
    if (dw > 0 && dh > 0) {
        if (dw == kW && dh == kH) {
            drawn = BitBlt(dc, offX_, offY_, kW, kH, memDC_, 0, 0, SRCCOPY) != 0;
        } else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            drawn = StretchBlt(dc, offX_, offY_, dw, dh, memDC_, 0, 0, kW, kH,
                               SRCCOPY) != 0;
        }
    }
    if (!drawn) BitBlt(dc, 0, 0, kW, kH, memDC_, 0, 0, SRCCOPY);
}

// ---------------------------------------------------------------------------
//  Input
// ---------------------------------------------------------------------------
int NoiseForgeEditor::hitTest(int x, int y) const {
    if (inside(RectI{ kTableRect.x + kTableRect.w - kDiceW - 10,
                      kTableRect.y + kTableRect.h - kDiceH - 8, kDiceW, kDiceH },
               x, y))
        return -100;

    for (int id = 0; id < kNumParams; ++id) {
        if (!laid_[id]) continue;
        if (inside(cell_[id], x, y)) return id;
    }
    return -1;
}

void NoiseForgeEditor::setValue(int id, double v) {
    if (!sink_) return;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    sink_->onPerformEdit(id, v);
}

LRESULT NoiseForgeEditor::wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(237, 233, 223));
        FillRect(reinterpret_cast<HDC>(w), &rc, bg);
        DeleteObject(bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(w));
        return 0;

    case WM_TIMER: {
        if (!sink_) return 0;
        ++tick_;

        if (diceRetry_ > 0) {
            if (sink_->randomizeTables()) diceRetry_ = 0;
            else --diceRetry_;
        }

        const uint32_t gen = sink_->scopeCount();
        const bool moving = (gen != lastGen_) || meter_ > 0.001f;
        lastGen_ = gen;

        if (moving || (tick_ % 5) == 0)
            RedrawWindow(h, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

        if (HWND par = GetParent(h)) {
            RECT pr{};
            GetClientRect(par, &pr);
            RECT cr{};
            GetClientRect(h, &cr);
            const int pw2 = (int)(pr.right - pr.left), ph2 = (int)(pr.bottom - pr.top);
            const int cw2 = (int)(cr.right - cr.left), ch2 = (int)(cr.bottom - cr.top);
            if (pw2 > 0 && ph2 > 0 && pw2 <= 3000 && ph2 <= 2200 &&
                (abs(pw2 - cw2) > 2 || abs(ph2 - ch2) > 2))
                layoutToWindow(pw2, ph2);
        }
        return 0;
    }

    case WM_APP_PARAM:
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        const int x = mx(l), y = my(l);
        if (drag_ >= 0 && sink_) {
            const bool fine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            double v;
            if (dragMode_ == 1) {
                const double span = fine ? (double)dragSpan_ * 4.0
                                         : (double)dragSpan_;
                v = dragVal_ + (double)(x - dragX_) / span;
            } else {
                const double span = fine ? 900.0 : 200.0;
                v = dragVal_ + (double)(dragY_ - y) / span;
            }
            setValue(drag_, v);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        const int id = hitTest(x, y);
        if (id != hot_) {
            hot_ = id;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        SetFocus(h);
        const int x = mx(l), y = my(l);
        const int id = hitTest(x, y);
        if (id == -100) {
            if (sink_ && !sink_->randomizeTables()) diceRetry_ = 25;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (id < 0 || !sink_) return 0;

        if (kParams[id].kind == kKnob) {
            drag_ = id;
            dragX_ = x;
            dragY_ = y;
            dragVal_ = sink_->getNormalized(id);
            if (isModAmt(id)) {
                dragMode_ = 1;
                dragSpan_ = std::max(24, cell_[id].w);
            } else {
                dragMode_ = 0;
            }
            SetCapture(h);
            sink_->onBeginEdit(id);
        } else if (kParams[id].kind == kSwitch) {
            const double nv = (sink_->getNormalized(id) >= 0.5) ? 0.0 : 1.0;
            sink_->onBeginEdit(id);
            sink_->onPerformEdit(id, nv);
            sink_->onEndEdit(id);
        } else {
            const int steps = kParams[id].steps;
            int idx = (int)(mapParam(id, (float)sink_->getNormalized(id)) + 0.5f);
            idx = (idx + 1) % (steps + 1);
            sink_->onBeginEdit(id);
            sink_->onPerformEdit(id, (double)idx / (double)steps);
            sink_->onEndEdit(id);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (drag_ >= 0 && sink_) {
            sink_->onEndEdit(drag_);
            drag_ = -1;
            dragMode_ = 0;
            ReleaseCapture();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED: {
        // Losing capture mid-drag must close the host edit gesture, or the
        // automation lane stays "recording" forever.
        if (drag_ >= 0 && sink_) {
            sink_->onEndEdit(drag_);
            drag_ = -1;
            dragMode_ = 0;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT wpt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &wpt);
        const int x = (int)((wpt.x - offX_) / fitScale_);
        const int y = (int)((wpt.y - offY_) / fitScale_);
        const int id = hitTest(x, y);
        if (id < 0 || !sink_ || kParams[id].kind != kKnob) return 0;
        const int delta = GET_WHEEL_DELTA_WPARAM(w);
        const bool fine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const double step = fine ? 0.005 : 0.02;
        double v = sink_->getNormalized(id) + (delta > 0 ? step : -step);
        sink_->onBeginEdit(id);
        sink_->onPerformEdit(id, v);
        sink_->onEndEdit(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        const int id = hitTest(mx(l), my(l));
        if (id < 0 || !sink_ || kParams[id].kind != kKnob) return 0;
        sink_->onBeginEdit(id);
        sink_->onPerformEdit(id, (double)kParams[id].defNorm);
        sink_->onEndEdit(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (w == 'D' || w == 'd') {
            if (sink_ && !sink_->randomizeTables()) diceRetry_ = 25;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(h, msg, w, l);
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        const int id = hitTest((int)((pt.x - offX_) / fitScale_),
                               (int)((pt.y - offY_) / fitScale_));
        SetCursor(LoadCursorW(nullptr, (id >= 0) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_SIZE: {
        const int w2 = (int)LOWORD(l), h2 = (int)HIWORD(l);
        if (w2 > 0 && h2 > 0 && (w2 != surfW_ || h2 != surfH_))
            layoutToWindow(w2, h2);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_SHOWWINDOW:
    case WM_WINDOWPOSCHANGED:
    case WM_ACTIVATE:
        InvalidateRect(h, nullptr, FALSE);
        {
            HDC d2 = GetDC(h);
            if (d2) { paint(d2); ReleaseDC(h, d2); }
        }
        return 0;

    case WM_CLOSE:
        return 0;

    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

} // namespace

// ---------------------------------------------------------------------------
IPlugView* createNoiseForgeEditor(IParamSink* sink) {
    return new NoiseForgeEditor(sink);
}

void notifyEditorParamChanged(IPlugView* view, int id) {
    if (!view) return;
    static_cast<NoiseForgeEditor*>(view)->onParamChanged(id);
}

} // namespace nf


