/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Pixmap.h"
#include "base/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/Anim.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Theme.h"
#include "TrimDialog.h"

// One sampled page, rendered once when the dialog opens.
struct TrimSample {
    int pageNo = 0;
    Pixmap* px = nullptr;
    Rect rect; // where it is drawn, in client coords
};

// The band a click lands in. The middle of a page is neither, so a stray tap in
// the text does nothing rather than cropping half the book away.
enum class TrimBand { None, Top, Bottom };

struct TrimDlgWnd : Wnd {
    ~TrimDlgWnd() override;
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    bool Create(MainWindow* win);
    void RenderSamples();
    void Layout();
    void FreeSamples();
    TrimBand BandAt(Point pt, float* fracOut) const;
    void SetBand(TrimBand band, float value);
    void UpdateAnimTimer();
    int SampleAt(Point pt) const;
    void EnsureLoupe(int sampleIdx);
    void DrawLoupe(HDC hdc);
    void FreeLoupe();

    MainWindow* win = nullptr;
    DisplayModel* dm = nullptr;
    Vec<TrimSample> samples;
    float trimTop = 0.0f;
    float trimBottom = 0.0f;
    // the page area inside a preview, shared by all of them (same y range)
    int previewTop = 0;
    int previewDy = 0;
    Rect btnApply;
    Rect btnClear;
    Rect btnCancel;
    int hotBtn = -1; // 0 apply, 1 clear, 2 cancel
    // true while what is shown came from the detector rather than from a tap
    bool suggested = false;
    // The band slides to its new depth rather than jumping, so it is obvious
    // that the crop is the same on every page. Top and bottom run on their own
    // clocks: setting the footer must not disturb a header still settling.
    AnimVal topAnim;
    AnimVal botAnim;
    int topMoves = 0;
    int botMoves = 0;
    // Dragging the line: while the finger is down the line tracks it exactly,
    // with no easing, or it would lag behind and feel broken.
    bool dragging = false;
    TrimBand dragBand = TrimBand::None;
    int dragSample = -1;
    Point dragPt;
    // A loupe over the line, the way a phone shows what is under your finger
    // while you place a cursor. Rendered at a higher zoom than the previews,
    // for the one page being dragged on, and kept until another page is used.
    Pixmap* loupePx = nullptr;
    int loupePage = 0;
    float loupeZoomRatio = 1.0f;
    bool done = false;
    bool applied = false;
};

constexpr int kTrimPad = 20;
constexpr int kTrimGap = 12;
constexpr int kTrimPreviewDx = 132;
constexpr int kTrimBtnDy = 34;
constexpr int kTrimFootDy = 112;
// a tap has to be clearly in the top or bottom third to mean anything
constexpr float kTrimDeadZoneLo = 0.42f;
constexpr float kTrimDeadZoneHi = 0.58f;
// nothing sensible crops away more than this much of a page
constexpr float kTrimMax = 0.40f;
// Slow enough the first few times that the reader sees the band sweep across
// all six pages at once; after that they know what it does and waiting for it
// would only be in the way.
constexpr int kTrimAnimSlowMs = 340;
constexpr int kTrimAnimFastMs = 90;
constexpr int kTrimSlowMoves = 3;
constexpr UINT_PTR kTrimAnimTimerId = 1;
// how much bigger than the preview the loupe shows the page
constexpr int kLoupeMag = 3;
constexpr int kLoupeDx = 190;
constexpr int kLoupeDy = 84;

TrimDlgWnd::~TrimDlgWnd() {
    FreeSamples();
    FreeLoupe();
}

void TrimDlgWnd::FreeLoupe() {
    FreePixmap(loupePx);
    loupePx = nullptr;
    loupePage = 0;
}

void TrimDlgWnd::FreeSamples() {
    for (TrimSample& s : samples) {
        FreePixmap(s.px);
        s.px = nullptr;
    }
    samples.Reset();
}

// Spread the samples through the document rather than taking the first few: the
// front matter of a book usually has no running header, and a preview of only
// those pages would be useless for setting one.
void TrimDlgWnd::RenderSamples() {
    FreeSamples();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    int nPages = engine->PageCount();
    if (nPages < 1) {
        return;
    }
    int want = std::clamp(nPages, 1, 6);
    for (int i = 0; i < want; i++) {
        // biased past the front matter when the document is long enough
        int pageNo = 1 + (int)(((i * 2 + 1) * (i64)nPages) / (want * 2));
        pageNo = std::clamp(pageNo, 1, nPages);
        RectF media = engine->PageMediabox(pageNo);
        if (media.dx <= 0) {
            continue;
        }
        float zoom = (float)DpiScale(hwnd, kTrimPreviewDx) / (float)media.dx;
        RenderPageArgs args(pageNo, zoom, 0);
        Pixmap* px = engine->RenderPage(args);
        if (!px) {
            continue;
        }
        TrimSample s;
        s.pageNo = pageNo;
        s.px = px;
        samples.Append(s);
    }
}

void TrimDlgWnd::Layout() {
    int pad = DpiScale(hwnd, kTrimPad);
    int gap = DpiScale(hwnd, kTrimGap);
    int maxDy = 0;
    for (TrimSample& s : samples) {
        maxDy = std::max(maxDy, s.px ? s.px->height : 0);
    }
    previewTop = pad + DpiScale(hwnd, 30);
    previewDy = maxDy;
    int x = pad;
    for (TrimSample& s : samples) {
        int dx = s.px ? s.px->width : 0;
        s.rect = Rect{x, previewTop, dx, s.px ? s.px->height : 0};
        x += dx + gap;
    }
    int totalDx = std::max(x - gap + pad, DpiScale(hwnd, 420));
    int totalDy = previewTop + previewDy + DpiScale(hwnd, kTrimFootDy);

    int btnDy = DpiScale(hwnd, kTrimBtnDy);
    int btnY = totalDy - pad - btnDy;
    int applyDx = DpiScale(hwnd, 150);
    int clearDx = DpiScale(hwnd, 110);
    int cancelDx = DpiScale(hwnd, 100);
    btnApply = Rect{totalDx - pad - applyDx, btnY, applyDx, btnDy};
    btnCancel = Rect{btnApply.x - gap - cancelDx, btnY, cancelDx, btnDy};
    btnClear = Rect{pad, btnY, clearDx, btnDy};

    Rect frame = HwndWindowRect(win->hwndFrame);
    int wx = frame.x + (frame.dx - totalDx) / 2;
    int wy = frame.y + (frame.dy - totalDy) / 3;
    MoveWindow(hwnd, wx, wy, totalDx, totalDy, TRUE);
}

bool TrimDlgWnd::Create(MainWindow* w) {
    win = w;
    dm = w->AsFixed();
    if (!dm) {
        return false;
    }
    trimTop = dm->manualTrimTop;
    trimBottom = dm->manualTrimBottom;
    float sugTop = 0.0f;
    float sugBottom = 0.0f;
    if (trimTop <= 0.0f && trimBottom <= 0.0f) {
        suggested = dm->SuggestHeaderFooterTrim(&sugTop, &sugBottom);
        trimTop = sugTop;
        trimBottom = sugBottom;
    }

    CreateCustomArgs args;
    args.visible = false;
    args.style = WS_POPUP | WS_DLGFRAME;
    args.pos = {0, 0, 100, 100};
    args.bgColor = ThemeMainWindowBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)win->hwndFrame);
    RenderSamples();
    Layout();
    // A trim already set is simply there; a fresh suggestion sweeps in, which
    // is the clearest way to say what the dialog is offering to do.
    if (suggested && AnimEnabled()) {
        topAnim.Set(0.0f);
        botAnim.Set(0.0f);
        float t = trimTop;
        float b = trimBottom;
        trimTop = 0.0f;
        trimBottom = 0.0f;
        SetBand(TrimBand::Top, t);
        SetBand(TrimBand::Bottom, b);
    } else {
        topAnim.Set(trimTop);
        botAnim.Set(trimBottom);
    }
    return true;
}

TrimBand TrimDlgWnd::BandAt(Point pt, float* fracOut) const {
    if (previewDy <= 0) {
        return TrimBand::None;
    }
    bool inAny = false;
    for (const TrimSample& s : samples) {
        if (pt.x >= s.rect.x && pt.x < s.rect.x + s.rect.dx) {
            inAny = true;
            break;
        }
    }
    if (!inAny || pt.y < previewTop || pt.y >= previewTop + previewDy) {
        return TrimBand::None;
    }
    float rel = (float)(pt.y - previewTop) / (float)previewDy;
    if (rel < kTrimDeadZoneLo) {
        *fracOut = std::min(rel, kTrimMax);
        return TrimBand::Top;
    }
    if (rel > kTrimDeadZoneHi) {
        *fracOut = std::min(1.0f - rel, kTrimMax);
        return TrimBand::Bottom;
    }
    return TrimBand::None;
}

int TrimDlgWnd::SampleAt(Point pt) const {
    for (int i = 0; i < len(samples); i++) {
        const Rect& r = samples[i].rect;
        if (pt.x >= r.x && pt.x < r.x + r.dx) {
            return i;
        }
    }
    return -1;
}

void TrimDlgWnd::EnsureLoupe(int sampleIdx) {
    if (sampleIdx < 0 || sampleIdx >= len(samples)) {
        return;
    }
    int pageNo = samples[sampleIdx].pageNo;
    if (loupePx && loupePage == pageNo) {
        return;
    }
    FreeLoupe();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    RectF media = engine->PageMediabox(pageNo);
    if (media.dx <= 0) {
        return;
    }
    float zoom = (float)(DpiScale(hwnd, kTrimPreviewDx) * kLoupeMag) / (float)media.dx;
    RenderPageArgs args(pageNo, zoom, 0);
    loupePx = engine->RenderPage(args);
    loupePage = pageNo;
}

// The loupe shows the page around the line at kLoupeMag times the preview, with
// the line drawn through it, so the reader can put the cut exactly between a
// header and the first line of text.
void TrimDlgWnd::DrawLoupe(HDC hdc) {
    if (!dragging || dragBand == TrimBand::None || !loupePx || dragSample < 0) {
        return;
    }
    if (dragSample >= len(samples)) {
        return;
    }
    const Rect& sr = samples[dragSample].rect;
    float frac = dragBand == TrimBand::Top ? topAnim.Value() : (1.0f - botAnim.Value());
    int lineY = sr.y + (int)((float)previewDy * (dragBand == TrimBand::Top ? topAnim.Value()
                                                                           : 1.0f - botAnim.Value()) +
                             0.5f);
    int dx = DpiScale(hwnd, kLoupeDx);
    int dy = DpiScale(hwnd, kLoupeDy);
    // above the line when there is room, below it otherwise, so the loupe never
    // covers the very thing being placed
    int y = lineY - dy - DpiScale(hwnd, 14);
    if (y < DpiScale(hwnd, 4)) {
        y = lineY + DpiScale(hwnd, 14);
    }
    int x = std::clamp(sr.x + sr.dx / 2 - dx / 2, DpiScale(hwnd, 4), HwndClientRect(hwnd).dx - dx - DpiScale(hwnd, 4));
    Rect loupe{x, y, dx, dy};

    // the same fraction down the higher-resolution render
    int srcCy = (int)((float)loupePx->height * frac + 0.5f);
    Rect src{std::max(0, loupePx->width / 2 - dx / 2), std::max(0, srcCy - dy / 2), dx, dy};
    src.dx = std::min(src.dx, loupePx->width - src.x);
    src.dy = std::min(src.dy, loupePx->height - src.y);
    if (src.dx <= 0 || src.dy <= 0) {
        return;
    }

    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(60, 0, 0, 0));
    gfx.FillRectangle(&shadow, loupe.x - 2, loupe.y - 2, loupe.dx + 4, loupe.dy + 4);
    HdcFillRect(hdc, loupe, ThemeControlBackgroundColor());
    BlitPixmapRegion(loupePx, hdc, Rect{loupe.x, loupe.y, src.dx, src.dy}, src);

    COLORREF guide = ThemeWindowLinkColor();
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(guide), (Gdiplus::REAL)std::max(1, DpiScale(hwnd, 2)));
    int cy = loupe.y + (srcCy - src.y);
    gfx.DrawLine(&pen, loupe.x, cy, loupe.x + loupe.dx, cy);
    Gdiplus::Pen border(GdiRgbFromCOLORREF(ThemeWindowDarkerTextColor()), 1.0f);
    gfx.DrawRectangle(&border, loupe.x, loupe.y, loupe.dx - 1, loupe.dy - 1);
}

void TrimDlgWnd::UpdateAnimTimer() {
    if (topAnim.IsAnimating() || botAnim.IsAnimating()) {
        SetTimer(hwnd, kTrimAnimTimerId, kAnimTickMs, nullptr);
    } else {
        KillTimer(hwnd, kTrimAnimTimerId);
    }
}

void TrimDlgWnd::SetBand(TrimBand band, float value) {
    AnimVal* anim = band == TrimBand::Top ? &topAnim : &botAnim;
    int* moves = band == TrimBand::Top ? &topMoves : &botMoves;
    if (band == TrimBand::Top) {
        trimTop = value;
    } else {
        trimBottom = value;
    }
    if (!AnimEnabled()) {
        anim->Set(value);
    } else {
        int durMs = (*moves < kTrimSlowMoves) ? kTrimAnimSlowMs : kTrimAnimFastMs;
        anim->SetTarget(value, durMs);
        (*moves)++;
        UpdateAnimTimer();
    }
    HwndInvalidate(hwnd, false);
}

static void DrawTrimButton(HDC hdc, const Rect& r, Str label, bool primary, bool hot) {
    COLORREF bg = primary ? ThemeWindowLinkColor() : ThemeControlBackgroundColor();
    COLORREF fg = primary ? RGB(255, 255, 255) : ThemeWindowTextColor();
    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(Gdiplus::Color(hot ? 255 : 225, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
    int d = std::min(r.dy, DpiScale(hdc, 16));
    Gdiplus::GraphicsPath path;
    path.AddArc(r.x, r.y, d, d, 180.0f, 90.0f);
    path.AddArc(r.x + r.dx - d, r.y, d, d, 270.0f, 90.0f);
    path.AddArc(r.x + r.dx - d, r.y + r.dy - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.x, r.y + r.dy - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    gfx.FillPath(&br, &path);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    HdcDrawText(hdc, label, r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));
}

void TrimDlgWnd::OnPaint(HDC hdc, PAINTSTRUCT*) {
    Rect rc = HwndClientRect(hwnd);
    HdcFillRect(hdc, rc, ThemeMainWindowBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);

    int pad = DpiScale(hwnd, kTrimPad);
    SetTextColor(hdc, ThemeWindowTextColor());
    Rect title{pad, pad, rc.dx - 2 * pad, DpiScale(hwnd, 24)};
    HdcDrawText(hdc, StrL("Tap in the top of a page to set the header, in the bottom to set the footer"), title,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 13, FW_MEDIUM));

    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int topDy = (int)((float)previewDy * topAnim.Value() + 0.5f);
    int botDy = (int)((float)previewDy * botAnim.Value() + 0.5f);
    COLORREF guide = ThemeWindowLinkColor();
    Gdiplus::SolidBrush scrim(Gdiplus::Color(150, 30, 28, 26));
    Gdiplus::Pen guidePen(GdiRgbFromCOLORREF(guide), (Gdiplus::REAL)std::max(1, DpiScale(hwnd, 2)));

    for (TrimSample& s : samples) {
        if (s.px) {
            BlitPixmap(s.px, hdc, s.rect);
        }
        Gdiplus::Pen edge(GdiRgbFromCOLORREF(ThemeWindowDarkerTextColor()), 1.0f);
        gfx.DrawRectangle(&edge, s.rect.x, s.rect.y, s.rect.dx - 1, s.rect.dy - 1);
        // the part that will be cropped, greyed out on every sample at once
        if (topDy > 0) {
            gfx.FillRectangle(&scrim, s.rect.x, s.rect.y, s.rect.dx, std::min(topDy, s.rect.dy));
            gfx.DrawLine(&guidePen, s.rect.x, s.rect.y + topDy, s.rect.x + s.rect.dx, s.rect.y + topDy);
        }
        if (botDy > 0) {
            int y = s.rect.y + s.rect.dy - botDy;
            gfx.FillRectangle(&scrim, s.rect.x, y, s.rect.dx, botDy);
            gfx.DrawLine(&guidePen, s.rect.x, y, s.rect.x + s.rect.dx, y);
        }
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        Rect lbl{s.rect.x, s.rect.y + s.rect.dy + DpiScale(hwnd, 4), s.rect.dx, DpiScale(hwnd, 16)};
        HdcDrawText(hdc, fmt("p. %d", s.pageNo), lbl, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 11));
    }

    TempStr summary;
    if (trimTop <= 0.0f && trimBottom <= 0.0f) {
        summary = str::DupTemp("Nothing trimmed yet - tap a page to start");
    } else {
        TempStr what = fmt("Trimming %d%% off the top and %d%% off the bottom of every page",
                           (int)(trimTop * 100.0f + 0.5f), (int)(trimBottom * 100.0f + 0.5f));
        summary = suggested ? fmt("Suggested: %s. Tap to adjust.", what) : what;
    }
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    Rect sum{pad, btnApply.y - DpiScale(hwnd, 30), rc.dx - 2 * pad, DpiScale(hwnd, 20)};
    HdcDrawText(hdc, summary, sum, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12));

    DrawLoupe(hdc);

    DrawTrimButton(hdc, btnClear, StrL("Clear"), false, hotBtn == 1);
    DrawTrimButton(hdc, btnCancel, StrL("Cancel"), false, hotBtn == 2);
    DrawTrimButton(hdc, btnApply, StrL("Apply to all pages"), true, hotBtn == 0);
}

LRESULT TrimDlgWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_TIMER && wparam == kTrimAnimTimerId) {
        HwndInvalidate(hwnd, false);
        UpdateAnimTimer();
        return 0;
    }
    if (msg == WM_MOUSEMOVE && dragging) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        dragPt = pt;
        int sample = SampleAt(pt);
        if (sample >= 0 && sample != dragSample) {
            dragSample = sample;
            EnsureLoupe(dragSample);
        }
        float rel = std::clamp((float)(pt.y - previewTop) / (float)std::max(1, previewDy), 0.0f, 1.0f);
        float value = dragBand == TrimBand::Top ? rel : 1.0f - rel;
        value = std::clamp(value, 0.0f, kTrimMax);
        // straight to the value, no easing: a line that lags the finger by even
        // a tenth of a second feels like it is fighting you
        if (dragBand == TrimBand::Top) {
            trimTop = value;
            topAnim.Set(value);
        } else {
            trimBottom = value;
            botAnim.Set(value);
        }
        HwndInvalidate(hwnd, false);
        return 0;
    }
    if (msg == WM_LBUTTONUP && dragging) {
        dragging = false;
        dragBand = TrimBand::None;
        ReleaseCapture();
        HwndInvalidate(hwnd, false);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int wasHot = hotBtn;
        hotBtn = -1;
        if (btnApply.Contains(pt)) {
            hotBtn = 0;
        } else if (btnClear.Contains(pt)) {
            hotBtn = 1;
        } else if (btnCancel.Contains(pt)) {
            hotBtn = 2;
        }
        if (wasHot != hotBtn) {
            HwndInvalidate(hwnd, false);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (btnApply.Contains(pt)) {
            applied = true;
            done = true;
            return 0;
        }
        if (btnCancel.Contains(pt)) {
            done = true;
            return 0;
        }
        if (btnClear.Contains(pt)) {
            suggested = false;
            SetBand(TrimBand::Top, 0.0f);
            SetBand(TrimBand::Bottom, 0.0f);
            return 0;
        }
        float frac = 0.0f;
        TrimBand band = BandAt(pt, &frac);
        if (band != TrimBand::None) {
            suggested = false;
            SetBand(band, frac);
            dragging = true;
            dragBand = band;
            dragSample = SampleAt(pt);
            dragPt = pt;
            EnsureLoupe(dragSample);
            SetCapture(hwnd);
            HwndInvalidate(hwnd, false);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if (wparam == VK_ESCAPE) {
            done = true;
            return 0;
        }
        if (wparam == VK_RETURN) {
            applied = true;
            done = true;
            return 0;
        }
    }
    if (msg == WM_CLOSE) {
        done = true;
        return 0;
    }
    return WndProcDefault(hwnd, msg, wparam, lparam);
}

bool ShowTrimHeaderFooterDialog(MainWindow* win) {
    if (!win || !win->AsFixed()) {
        return false;
    }
    TrimDlgWnd* dlg = new TrimDlgWnd();
    if (!dlg->Create(win)) {
        delete dlg;
        return false;
    }
    HWND hwndFrame = win->hwndFrame;
    EnableWindow(hwndFrame, FALSE);
    ShowWindow(dlg->hwnd, SW_SHOW);
    SetForegroundWindow(dlg->hwnd);
    HwndSetFocus(dlg->hwnd);

    // own modal loop: the dialog owns the interaction until it is dismissed
    MSG msg;
    while (!dlg->done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hwndFrame, TRUE);
    SetForegroundWindow(hwndFrame);

    bool applied = dlg->applied;
    float top = dlg->trimTop;
    float bottom = dlg->trimBottom;
    DisplayModel* dm = dlg->dm;
    delete dlg;

    if (!applied || !dm) {
        return false;
    }
    dm->SetManualTrim(top, bottom);
    // every page's laid-out height changes; keep the reader where they were
    ScrollState state = dm->GetScrollState();
    dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
    dm->SetScrollState(state);
    win->RedrawAll(true);
    SaveSettings();
    return true;
}
