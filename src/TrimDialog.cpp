/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Pixmap.h"
#include "base/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

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

TrimDlgWnd::~TrimDlgWnd() {
    FreeSamples();
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
    int topDy = (int)((float)previewDy * trimTop + 0.5f);
    int botDy = (int)((float)previewDy * trimBottom + 0.5f);
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
        summary = str::DupTemp("Nothing trimmed yet");
    } else {
        summary = fmt("Trimming %d%% off the top and %d%% off the bottom of every page",
                      (int)(trimTop * 100.0f + 0.5f), (int)(trimBottom * 100.0f + 0.5f));
    }
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    Rect sum{pad, btnApply.y - DpiScale(hwnd, 30), rc.dx - 2 * pad, DpiScale(hwnd, 20)};
    HdcDrawText(hdc, summary, sum, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, HdcGetUiFont(hdc, 12));

    DrawTrimButton(hdc, btnClear, StrL("Clear"), false, hotBtn == 1);
    DrawTrimButton(hdc, btnCancel, StrL("Cancel"), false, hotBtn == 2);
    DrawTrimButton(hdc, btnApply, StrL("Apply to all pages"), true, hotBtn == 0);
}

LRESULT TrimDlgWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
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
            trimTop = 0.0f;
            trimBottom = 0.0f;
            HwndInvalidate(hwnd, false);
            return 0;
        }
        float frac = 0.0f;
        TrimBand band = BandAt(pt, &frac);
        if (band == TrimBand::Top) {
            trimTop = frac;
        } else if (band == TrimBand::Bottom) {
            trimBottom = frac;
        }
        if (band != TrimBand::None) {
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
