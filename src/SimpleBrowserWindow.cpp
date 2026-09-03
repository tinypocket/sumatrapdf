/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Dpi.h"
#include "base/ScopedWin.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/WebView.h"

#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Http.h"
#include "base/UITask.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "Commands.h"
#include "SumatraConfig.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "MainWindow.h"
#include "SumatraPDF.h"
#include "Rail.h"
#include "Theme.h"
#include "Translations.h"
#include "BrowserUrlUtil.h"
#include "wingui/Anim.h"

#include "SimpleBrowserWindow.h"

constexpr int kNavRowPadding = 6;
constexpr int kNavBtnGap = 4;

static LRESULT CALLBACK UrlStaticSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*idSubclass*/,
                                              DWORD_PTR /*refData*/) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SetCurrentUrl(SimpleBrowserWindow* w, Str url) {
    if (!w || !w->hwndUrl) {
        return;
    }
    HwndSetText(w->hwndUrl, url);
}

static void UpdateNavButtons(SimpleBrowserWindow* w) {
    if (!w || !w->webView) {
        return;
    }
    if (w->btnBack) {
        w->btnBack->SetIsEnabled(w->webView->CanGoBack());
    }
    if (w->btnForward) {
        w->btnForward->SetIsEnabled(w->webView->CanGoForward());
    }
}

static void LayoutControls(SimpleBrowserWindow* w) {
    if (!w || !w->hwnd || !w->btnBack || !w->btnForward || !w->hwndUrl) {
        return;
    }

    Rect rc = HwndClientRect(w->hwnd);
    int pad = DpiScale(w->hwnd, kNavRowPadding);
    int gap = DpiScale(w->hwnd, kNavBtnGap);
    int y = pad;
    int x = pad;

    Size backSize = w->btnBack->GetIdealSize();
    Size fwdSize = w->btnForward->GetIdealSize();
    int rowH = backSize.dy;
    rowH = std::max(fwdSize.dy, rowH);

    MoveWindow(w->btnBack->hwnd, x, y, backSize.dx, backSize.dy, TRUE);
    x += backSize.dx + gap;
    MoveWindow(w->btnForward->hwnd, x, y, fwdSize.dx, fwdSize.dy, TRUE);
    x += fwdSize.dx + gap;

    int urlX = x;
    int urlDx = rc.dx - urlX - pad;
    urlDx = std::max(urlDx, 0);
    int urlDy = FontDyPx(w->hwnd, w->hFont);
    if (urlDy <= 0) {
        urlDy = rowH;
    }
    int urlY = y + ((rowH - urlDy) / 2);
    MoveWindow(w->hwndUrl, urlX, urlY, urlDx, urlDy, TRUE);

    int navRowDy = rowH + (2 * pad);
    int webDy = rc.dy - navRowDy - pad;
    webDy = std::max(webDy, 0);
    int webDx = rc.dx - (2 * pad);
    webDx = std::max(webDx, 0);
    if (w->webView) {
        w->webView->SetBounds({pad, navRowDy, webDx, webDy});
        w->webView->UpdateWebviewSize();
    }
}

static void OnBack(SimpleBrowserWindow* w) {
    if (w && w->webView) {
        w->webView->GoBack();
    }
}

static void OnForward(SimpleBrowserWindow* w) {
    if (w && w->webView) {
        w->webView->GoForward();
    }
}

// an absolute http(s)/mailto URL is "non-internal": it points outside the
// content we serve from our virtual host (UrlForWebViewEvent strips the host
// prefix off internal pages, so those arrive as a bare path without a scheme)
static bool IsExternalWebUrl(Str url) {
    return str::StartsWithI(url, StrL("http://")) || str::StartsWithI(url, StrL("https://")) ||
           str::StartsWithI(url, StrL("mailto:"));
}

static bool NavigationStarting(void* ctx, Str url, bool newWindow) {
    auto* w = (SimpleBrowserWindow*)ctx;
    if (!w) {
        return true;
    }
    // When we host internal content (the manual) from a virtual host, its
    // non-internal links are rendered with target="_blank", which arrives here as
    // a new-window request. Open those (and any external in-window navigation) in
    // the user's default browser instead of the in-app webview. A plain browser
    // window (no virtual host) keeps normal in-window navigation.
    bool servesInternalContent = w->webView && len(w->webView->resourceUriPrefix) > 0;
    if (newWindow || (servesInternalContent && IsExternalWebUrl(url))) {
        SumatraLaunchBrowser(url);
        return false;
    }
    SetCurrentUrl(w, url);
    return true;
}

static void NavigationCompleted(void* ctx, Str url, bool success) {
    auto* w = (SimpleBrowserWindow*)ctx;
    if (!w || !success) {
        return;
    }
    SetCurrentUrl(w, url);
    UpdateNavButtons(w);
    if (!w->webViewFocusSet && w->webView) {
        w->webView->Focus();
        w->webViewFocusSet = true;
    }
}

static void HistoryChanged(void* ctx, bool canGoBack, bool canGoForward) {
    auto* w = (SimpleBrowserWindow*)ctx;
    if (!w) {
        return;
    }
    if (w->btnBack) {
        w->btnBack->SetIsEnabled(canGoBack);
    }
    if (w->btnForward) {
        w->btnForward->SetIsEnabled(canGoForward);
    }
}

static int ResolveAccelCmd(void* /*user*/, u16 vk, bool ctrl, bool shift, bool alt) {
    if (vk == 'W' && ctrl && !shift && !alt) {
        return CmdClose;
    }
    // Esc closes documentation (WebView has focus; PreTranslate alone is not enough)
    if (vk == VK_ESCAPE && !ctrl && !shift && !alt) {
        return CmdClose;
    }
    return 0;
}

SimpleBrowserWindow::~SimpleBrowserWindow() {
    delete btnBack;
    delete btnForward;
    delete webView;
}

bool SimpleBrowserWindow::PreTranslateMessage(MSG& msg) {
    // When focus is on chrome (Back/Forward/URL), Esc is not handled by WebView.
    if ((msg.message == WM_KEYDOWN || msg.message == WM_CHAR) && msg.wParam == VK_ESCAPE) {
        Close();
        return true;
    }
    return false;
}

LRESULT SimpleBrowserWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_SETFOCUS) {
        if (webView) {
            webView->Focus();
        }
        return 0;
    }
    if (msg == WM_SIZE) {
        LayoutControls(this);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == CmdClose) {
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC && (HWND)lparam == hwndUrl) {
        HDC hdc = (HDC)wparam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        HBRUSH br = BackgroundBrush();
        if (!br) {
            br = (HBRUSH)GetStockObject(WHITE_BRUSH);
        }
        return (LRESULT)br;
    }
    return WndProcDefault(hwnd, msg, wparam, lparam);
}

HWND SimpleBrowserWindow::Create(const SimpleBrowserCreateArgs& args) {
    HWND frameHwnd = nullptr;
    {
        CreateCustomArgs cargs;
        cargs.pos = args.pos;
        if (cargs.pos.IsZero()) {
            cargs.pos = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT};
        }
        cargs.title = args.title;
        if (!cargs.title) {
            cargs.title = "Browser Window";
        }
        HMODULE h = GetModuleHandleW(nullptr);
        WCHAR* iconName = MAKEINTRESOURCEW(GetAppIconID());
        cargs.icon = LoadIconW(h, iconName);
        // TODO: if set, navigate to url doesn't work
        // args.visible = false;
        frameHwnd = CreateCustom(cargs);
        ReportIf(!frameHwnd);
    }

    hFont = GetDefaultGuiFont();

    {
        Button::CreateArgs bargs;
        bargs.parent = frameHwnd;
        bargs.font = hFont;
        bargs.text = _TRA("Back");
        btnBack = new Button();
        btnBack->Create(bargs);
        btnBack->onClick = MkFunc0<SimpleBrowserWindow>(OnBack, this);
        btnBack->SetIsEnabled(false);
    }
    {
        Button::CreateArgs bargs;
        bargs.parent = frameHwnd;
        bargs.font = hFont;
        bargs.text = _TRA("Forward");
        btnForward = new Button();
        btnForward->Create(bargs);
        btnForward->onClick = MkFunc0<SimpleBrowserWindow>(OnForward, this);
        btnForward->SetIsEnabled(false);
    }
    {
        HINSTANCE inst = GetInstance();
        hwndUrl = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS, 0, 0, 0, 0,
                                  frameHwnd, nullptr, inst, nullptr);
        SendMessageW(hwndUrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        SetWindowSubclass(hwndUrl, UrlStaticSubclassProc, NextSubclassId(), 0);
    }

    {
        webView = new WebviewWnd();
        Str dataDir = args.dataDir;
        if (!dataDir) {
            dataDir = GetWebViewDataDirTemp();
        }
        webView->dataDir = str::Dup(dataDir);
        webView->resourceProvider = args.resourceProvider;
        wstr::Free(webView->resourceUriPrefix);
        webView->resourceUriPrefix = wstr::Dup(args.resourceUriPrefix);
        webView->events.ctx = this;
        webView->events.navigationStarting = NavigationStarting;
        webView->events.navigationCompleted = NavigationCompleted;
        webView->events.historyChanged = HistoryChanged;
        webView->events.resolveAccelCmd = ResolveAccelCmd;
        webView->forwardAppAccelerators = true;

        CreateWebViewArgs cargs;
        cargs.parent = frameHwnd;
        cargs.pos = HwndClientRect(frameHwnd);
        if (!webView->Create(cargs)) {
            return nullptr;
        }
        webView->SetIsVisible(true);
    }

    SetCurrentUrl(this, args.url);
    LayoutControls(this);

    // important to call this after hooking up onSize to ensure
    // first layout is triggered
    webView->Navigate(args.url);
    SetIsVisible(true);
    if (webView) {
        webView->Focus();
    }
    return frameHwnd;
}

SimpleBrowserWindow* SimpleBrowserWindowCreate(const SimpleBrowserCreateArgs& args) {
    if (!HasWebView()) {
        return nullptr;
    }
    auto* res = new SimpleBrowserWindow();
    auto* hwnd = res->Create(args);
    ReportIfFast(!hwnd);
    if (!hwnd) {
        delete res;
        return nullptr;
    }
    return res;
}

// ---------------------------------------------------------------------------
// In-product embedded web browser (TouchView::Web, the rail's globe icon).
// Reuses the WebView2 plumbing above but lives inside the frame's content area
// (children of hwndFrame drawn over the canvas) instead of its own window.
// ---------------------------------------------------------------------------

// --- design metrics --------------------------------------------------------
// All values are LOGICAL px from the "SumatraPDF Touch Redesign v3" spec and
// are DpiScale()d at use. The chrome is three stacked rows (tab strip,
// favorites bar, nav row) painted by a single owner-drawn window, TbChromeWnd,
// which also does its own hit-testing: system Button controls can't be made to
// look like the design's rounded pills.
constexpr int kTbTabsRowDy = 40; // tab strip row
constexpr int kTbFavRowDy = 36;  // favorites bar (only when there are favorites)
constexpr int kTbNavRowDy = 56;  // nav row

constexpr int kTbTabsPadX = 12;
constexpr int kTbTabsGap = 4;
constexpr int kTbTabDy = 32;
constexpr int kTbTabMinDx = 110;
constexpr int kTbTabMaxDx = 180;
constexpr int kTbTabRadius = 8;
constexpr int kTbTabPadLeft = 12;
constexpr int kTbTabPadRight = 8;
constexpr int kTbTabInnerGap = 8;
constexpr int kTbTabCloseDx = 18;
constexpr int kTbNewTabDx = 32;

constexpr int kTbFavPadX = 16;
constexpr int kTbFavGap = 18;

constexpr int kTbNavPadX = 16;
constexpr int kTbNavGap = 10;
constexpr int kTbNavBtnDy = 34;
constexpr int kTbNavBtnPadX = 14;
constexpr int kTbNavRadius = 8;
constexpr int kTbUrlDy = 36;
constexpr int kTbUrlPadX = 14;
constexpr int kTbIconBtnDx = 34;

// font sizes (logical px), also from the spec
constexpr int kTbFontTab = 13;
constexpr int kTbFontFav = 13;
constexpr int kTbFontBtn = 13;
constexpr int kTbFontUrl = 14;
constexpr int kTbFontMenu = 14;
constexpr int kTbFontDlgTitle = 17;

// each tab is a live WebView2 control (its own renderer process), and the strip
// stops being readable well before this many anyway
constexpr int kTbMaxTabs = 10;

// "..." menu
constexpr int kTbMenuMinDx = 180;
constexpr int kTbMenuDx = 300;
constexpr int kTbMenuItemDy = 38; // 14px text + 9px padding top/bottom
constexpr int kTbMenuSepDy = 9;
constexpr int kTbMenuPad = 6;
constexpr int kTbMenuRadius = 10;
constexpr int kTbMenuItemRadius = 6;
constexpr int kTbMenuItemPadX = 12;

// "Manage favorites" modal
constexpr int kTbFavMgrDx = 480; // room for a name plus four 28px buttons
constexpr int kTbFavMgrMaxDy = 520;
constexpr int kTbFavMgrRadius = 14;
constexpr int kTbFavMgrHeaderDy = 63; // 20 + 17px line + 20, rounded
constexpr int kTbFavMgrFooterDy = 64; // 14 + 36 + 14
constexpr int kTbFavMgrHeadPadX = 24; // header / footer horizontal padding
constexpr int kTbFavMgrListPadX = 16; // list area horizontal padding
constexpr int kTbFavMgrListPadY = 10; // list area vertical padding
constexpr int kTbFavMgrRowDy = 44;    // 8 + 28 + 8
constexpr int kTbFavMgrRowPad = 8;
constexpr int kTbFavMgrRowGap = 10;
constexpr int kTbFavMgrRowRadius = 8;
constexpr int kTbFavMgrBtnDx = 28;
constexpr int kTbFavMgrBtnRadius = 7;
constexpr int kTbFavMgrGripDx = 14;
constexpr int kTbFavMgrEmptyDy = 80; // 30 padding + a line + 30
constexpr int kTbFavMgrDoneDy = 36;
constexpr int kTbFavMgrDonePadX = 18;
// a press has to travel this far (logical px) before it counts as a drag
// rather than a click, so tapping a row still works on a shaky finger
constexpr int kTbDragSlop = 6;

// --- page load progress bar -----------------------------------------------
// A thin accent bar along the bottom edge of the nav row, like Chrome/Edge.
// WebView2 reports no percentage, so it eases quickly to kTbProgCreepTo while
// the page loads (a cubic ease-out is fast at the start and crawls at the end -
// exactly the "nearly there" feel a browser bar has), then jumps to 100% and
// fades out when the navigation ends.
constexpr int kTbProgDy = 3;
constexpr int kTbProgCreepMs = 2400;
constexpr int kTbProgFinishMs = 180;
constexpr int kTbProgFadeMs = 280;
constexpr float kTbProgCreepTo = 0.85f;
constexpr UINT_PTR kTbProgTimerId = 71;
// hover cross-fade / press sink, only while ElaborateAnimations is on
constexpr UINT_PTR kTbHoverTimerId = 72;
// how deep a pressed chrome button sinks, matching the top bar
constexpr int kTbPressInset = 2;

// nav-row escape hatch shown when the page turns out to be a document
#define kTbOpenDocLabel "Open in SumatraPDF"

struct TouchBrowser;
struct TbChromeWnd;
struct TbMenuWnd;
struct TbFavMgrWnd;
struct TbScrimWnd;

// One browser tab: its own WebView2 control plus the state the tab strip draws
// for it. `this` is the WebViewEvents ctx of its own webview, so the navigation
// callbacks know which tab they belong to.
struct TbTab {
    TouchBrowser* tb = nullptr;
    WebviewWnd* webView = nullptr;
    Str url;   // last committed URL; shown in the URL bar while active
    Str title; // document title, empty until the page reports one
    // WebView2 ignores a Navigate issued before the control has a non-zero size
    // and a first layout, so every tab defers its first navigation to
    // LayoutTouchWebView (mirrors SimpleBrowserWindow::Create). Until then the
    // page it should open is parked here.
    Str pendingUrl;
    bool didInitialNav = false;
    // a top-level navigation is in flight; drives the load progress bar
    bool loading = false;
    // The page on screen is really a document (its response said so). Set from
    // the Content-Type of the main document, which is the only thing that knows
    // when the URL has no ".pdf" in it. While set, the nav row offers
    // "Open in SumatraPDF".
    bool isDocPage = false;
    Str docPageUrl;
    Str docPageExt;
    // URL of the last automatic hand-off, so a page that redirects straight
    // back to the document can't put us in a takeover loop
    Str lastTakeoverUrl;
    double lastTakeoverMs = 0.0;
};

struct TouchBrowser {
    MainWindow* win = nullptr;
    Vec<TbTab*> tabs;
    // index into `tabs`; only this tab's webview is visible. Kept in range by
    // TbActivateTab, and `tabs` is never left empty while the browser exists.
    int activeTab = 0;
    // the whole chrome (all three rows) is this one owner-drawn child of
    // hwndFrame; the URL field is a real EDIT hosted inside it
    TbChromeWnd* chrome = nullptr;
    HFONT hFont = nullptr;
    // mirrors of the active webview's history state, so the nav row can paint
    // Back / Fwd enabled or disabled without querying WebView2 while painting
    bool canGoBack = false;
    bool canGoForward = false;
    // touch popups, created on first use and owned by the browser. They are
    // top-level WS_POPUP windows, so unlike the chrome they are not children of
    // hwndFrame and WebView2 can't cover them.
    TbMenuWnd* menuWnd = nullptr;
    TbFavMgrWnd* favMgr = nullptr;
    TbScrimWnd* scrim = nullptr;
};

static Str TouchBrowserHomeUrl() {
    Str url = gGlobalPrefs->browserHomePage;
    if (!url) {
        url = StrL("https://www.google.com");
    }
    return url;
}

// --- shared drawing helpers ------------------------------------------------
// GDI's RoundRect can't do "rounded top corners only" (the tabs) and aliases
// badly at these radii, so the chrome draws its shapes with GDI+ paths. Each
// helper scopes its own Gdiplus::Graphics: GDI+ caches HDC state, so it must be
// gone again before the GDI text calls that follow run on the same DC.

// DpiScale for fractional values (pen widths, icon geometry)
static float TbScaleF(HWND hwnd, float v) {
    return (float)DpiScale(hwnd, 10000) * v / 10000.0f;
}

static void TbRoundRectPath(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float rad) {
    float d = rad * 2;
    d = std::min(d, std::min(r.Width, r.Height));
    if (d <= 0.5f) {
        p.AddRectangle(r);
        p.CloseFigure();
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static Gdiplus::RectF TbRectF(const Rect& r) {
    // half-pixel inset so a 1px pen lands on the pixel grid instead of straddling it
    return Gdiplus::RectF((float)r.x + 0.5f, (float)r.y + 0.5f, (float)r.dx - 1.0f, (float)r.dy - 1.0f);
}

// rounded filled rect with an optional 1px border
static void TbFillRounded(HDC hdc, const Rect& r, float rad, COLORREF fill, COLORREF border = kColorUnset,
                          float borderW = 1.0f) {
    if (r.dx <= 0 || r.dy <= 0) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath p;
    TbRoundRectPath(p, TbRectF(r), rad);
    if (fill != kColorUnset) {
        Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(fill));
        g.FillPath(&br, &p);
    }
    if (border != kColorUnset) {
        Gdiplus::Pen pen(GdiRgbFromCOLORREF(border), borderW);
        g.DrawPath(&pen, &p);
    }
}

// A tab: rounded TOP corners only, and a border that skips the bottom edge, so
// the active tab merges into the row below it.
static void TbFillTab(HDC hdc, const Rect& r, float rad, COLORREF fill, COLORREF border) {
    if (r.dx <= 0 || r.dy <= 0) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rf = TbRectF(r);
    float d = std::min(rad * 2, std::min(rf.Width, rf.Height));
    float x0 = rf.X;
    float x1 = rf.X + rf.Width;
    float y0 = rf.Y;
    float y1 = rf.Y + rf.Height;
    {
        Gdiplus::GraphicsPath fillPath;
        fillPath.AddArc(x0, y0, d, d, 180, 90);
        fillPath.AddArc(x1 - d, y0, d, d, 270, 90);
        fillPath.AddLine(x1, y1, x0, y1);
        fillPath.CloseFigure();
        Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(fill));
        g.FillPath(&br, &fillPath);
    }
    if (border == kColorUnset) {
        return;
    }
    // open path: left edge up, both top corners, right edge down. No bottom.
    Gdiplus::GraphicsPath edge;
    edge.AddLine(x0, y1, x0, y0 + d / 2);
    edge.AddArc(x0, y0, d, d, 180, 90);
    edge.AddArc(x1 - d, y0, d, d, 270, 90);
    edge.AddLine(x1, y0 + d / 2, x1, y1);
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(border), 1.0f);
    g.DrawPath(&pen, &edge);
}

// the tab's close affordance: an 18px circle with a small "x" in it. Drawn, not
// typed, so it doesn't depend on the UI font (and can't come out as mojibake).
static void TbDrawCloseGlyph(HDC hdc, HWND hwnd, const Rect& box, COLORREF col, COLORREF circleBg) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (circleBg != kColorUnset) {
        Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(circleBg));
        g.FillEllipse(&br, (float)box.x, (float)box.y, (float)box.dx, (float)box.dy);
    }
    float cx = (float)box.x + (float)box.dx / 2;
    float cy = (float)box.y + (float)box.dy / 2;
    float a = TbScaleF(hwnd, 3.2f);
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), TbScaleF(hwnd, 1.3f));
    g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
    g.DrawLine(&pen, cx + a, cy - a, cx - a, cy + a);
}

// the new-tab "+": two 15px strokes, not a text glyph
static void TbDrawPlus(HDC hdc, HWND hwnd, const Rect& box, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (float)box.x + (float)box.dx / 2;
    float cy = (float)box.y + (float)box.dy / 2;
    float a = TbScaleF(hwnd, 7.5f);
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), TbScaleF(hwnd, 1.8f));
    g.DrawLine(&pen, cx - a, cy, cx + a, cy);
    g.DrawLine(&pen, cx, cy - a, cx, cy + a);
}

// the overflow button: three horizontal dots (r=1.8 at x=5,12,19 of a 24 box)
static void TbDrawDots(HDC hdc, HWND hwnd, const Rect& box, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(col));
    float unit = (float)DpiScale(hwnd, 24) / 24.0f;
    float x0 = (float)box.x + (float)box.dx / 2 - 12.0f * unit;
    float cy = (float)box.y + (float)box.dy / 2;
    float r = 1.8f * unit;
    const float cxs[3] = {5.0f, 12.0f, 19.0f};
    for (float c : cxs) {
        float cx = x0 + c * unit;
        g.FillEllipse(&br, cx - r, cy - r, r * 2, r * 2);
    }
}

// a chevron for the move-up / move-down buttons
static void TbDrawChevron(HDC hdc, HWND hwnd, const Rect& box, COLORREF col, bool up) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (float)box.x + (float)box.dx / 2;
    float cy = (float)box.y + (float)box.dy / 2;
    float w = TbScaleF(hwnd, 4.5f);
    float h = TbScaleF(hwnd, 2.5f);
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), TbScaleF(hwnd, 1.6f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (up) {
        g.DrawLine(&pen, cx - w, cy + h, cx, cy - h);
        g.DrawLine(&pen, cx, cy - h, cx + w, cy + h);
    } else {
        g.DrawLine(&pen, cx - w, cy - h, cx, cy + h);
        g.DrawLine(&pen, cx, cy + h, cx + w, cy - h);
    }
}

// the delete button's trash can
static void TbDrawTrash(HDC hdc, HWND hwnd, const Rect& box, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float u = (float)DpiScale(hwnd, 1000) / 1000.0f;
    float cx = (float)box.x + (float)box.dx / 2;
    float cy = (float)box.y + (float)box.dy / 2;
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), TbScaleF(hwnd, 1.4f));
    // lid
    g.DrawLine(&pen, cx - 5.5f * u, cy - 3.5f * u, cx + 5.5f * u, cy - 3.5f * u);
    // handle
    g.DrawLine(&pen, cx - 2.0f * u, cy - 5.5f * u, cx + 2.0f * u, cy - 5.5f * u);
    g.DrawLine(&pen, cx - 2.0f * u, cy - 5.5f * u, cx - 2.0f * u, cy - 3.5f * u);
    g.DrawLine(&pen, cx + 2.0f * u, cy - 5.5f * u, cx + 2.0f * u, cy - 3.5f * u);
    // body
    g.DrawLine(&pen, cx - 4.0f * u, cy - 3.0f * u, cx - 3.4f * u, cy + 5.5f * u);
    g.DrawLine(&pen, cx + 4.0f * u, cy - 3.0f * u, cx + 3.4f * u, cy + 5.5f * u);
    g.DrawLine(&pen, cx - 3.4f * u, cy + 5.5f * u, cx + 3.4f * u, cy + 5.5f * u);
}

// the rename button's pencil, drawn as a 45-degree body with a pointed tip.
// A path rather than a glyph: the codebase keeps every string literal ASCII, so
// icon characters are out.
static void TbDrawPencil(HDC hdc, HWND hwnd, const Rect& box, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float u = (float)DpiScale(hwnd, 1000) / 1000.0f;
    float cx = (float)box.x + (float)box.dx / 2;
    float cy = (float)box.y + (float)box.dy / 2;
    // unit vectors along the pencil (pointing up-right) and across it
    const float k = 0.70711f;
    float dx = k, dy = -k; // along
    float px = k, py = k;  // across
    float halfLen = 6.2f * u;
    float halfW = 2.2f * u;
    float collar = 2.4f * u; // distance from the tip to where the body starts
    auto pt = [&](float along, float across) {
        return Gdiplus::PointF(cx + dx * along + px * across, cy + dy * along + py * across);
    };
    Gdiplus::PointF tip = pt(-halfLen, 0);
    Gdiplus::PointF c1 = pt(-halfLen + collar, halfW);
    Gdiplus::PointF c2 = pt(-halfLen + collar, -halfW);
    Gdiplus::PointF e1 = pt(halfLen, halfW);
    Gdiplus::PointF e2 = pt(halfLen, -halfW);
    Gdiplus::GraphicsPath body;
    body.AddLine(tip, c1);
    body.AddLine(c1, e1);
    body.AddLine(e1, e2);
    body.AddLine(e2, c2);
    body.AddLine(c2, tip);
    body.CloseFigure();
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), TbScaleF(hwnd, 1.3f));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &body);
    // the collar line separating the tip from the body
    g.DrawLine(&pen, c1, c2);
    // and the ferrule near the far end
    Gdiplus::PointF f1 = pt(halfLen - 2.2f * u, halfW);
    Gdiplus::PointF f2 = pt(halfLen - 2.2f * u, -halfW);
    g.DrawLine(&pen, f1, f2);
}

// the 6-dot grab handle that marks a favorites row as draggable
static void TbDrawGrip(HDC hdc, HWND hwnd, const Rect& box, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(GdiRgbFromCOLORREF(col));
    float unit = (float)DpiScale(hwnd, 14) / 14.0f;
    float x0 = (float)box.x + (float)box.dx / 2 - 7.0f * unit;
    float y0 = (float)box.y + (float)box.dy / 2 - 7.0f * unit;
    float r = 1.3f * unit;
    const float cxs[2] = {4.0f, 10.0f};
    const float cys[3] = {3.0f, 7.0f, 11.0f};
    for (float c : cxs) {
        for (float ry : cys) {
            g.FillEllipse(&br, x0 + c * unit - r, y0 + ry * unit - r, r * 2, r * 2);
        }
    }
}

// a URL points at a document we can open when its LAST path segment names a
// supported file type. The engine-free parsing (query/fragment stripping,
// last-segment + extension extraction) lives in TouchBrowserUrlFileType so the
// unit tests exercise the exact same logic; here we add the IsSupportedFileType
// check (which pulls in the engine layer). Only the last segment is inspected so
// host dots (www.google.com) and extension-less page paths are never mistaken
// for documents - otherwise navigationStarting would cancel ordinary browsing.
static bool TouchBrowserUrlIsDoc(Str url, Str* extOut) {
    FileType ft = FileType::Unknown;
    Str ext;
    if (!TouchBrowserUrlFileType(url, &ft, &ext)) {
        return false;
    }
    // NOT IsSupportedFileType: the engine also opens .html/.txt/images, so that
    // would hijack ordinary web links (a link to page.html opened as a document
    // tab instead of being browsed). Only real "download me" document formats.
    if (!TouchBrowserFileTypeIsDownloadableDoc(ft)) {
        return false;
    }
    if (extOut) {
        *extOut = ext;
    }
    return true;
}

// the file name a URL points at (query/fragment stripped, last path segment)
static TempStr TbUrlFileNameTemp(Str url) {
    Str name = url;
    int cut = str::IndexOfChar(name, '?');
    if (cut >= 0) {
        name = Str(name.s, cut);
    }
    cut = str::IndexOfChar(name, '#');
    if (cut >= 0) {
        name = Str(name.s, cut);
    }
    int slash = str::LastIndexOfChar(name, '/');
    if (slash >= 0) {
        name = Str(name.s + slash + 1, name.len - slash - 1);
    }
    return name ? str::DupTemp(name) : TempStr{};
}

// The user's Downloads folder; documents opened from the browser are saved
// there (like a normal browser) instead of a temp file, so they persist and
// have a real name.
static TempStr TbDownloadsDirTemp() {
    WCHAR* pathW = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pathW);
    if (FAILED(hr) || !pathW) {
        CoTaskMemFree(pathW);
        return {};
    }
    TempStr res = ToUtf8Temp(pathW);
    CoTaskMemFree(pathW);
    return res;
}

// remember what we downloaded so "Clean up downloaded files" can remove exactly
// those and nothing else
static void TbRecordDownload(Str path) {
    if (!path) {
        return;
    }
    if (!gGlobalPrefs->browserDownloads) {
        gGlobalPrefs->browserDownloads = new Vec<Str>();
    }
    Vec<Str>* v = gGlobalPrefs->browserDownloads;
    for (int i = 0; i < len(*v); i++) {
        if (str::EqI((*v)[i], path)) {
            return;
        }
    }
    v->Append(str::Dup(path));
    SaveSettings();
}

struct TbDocDownload {
    Str url;
    Str destPath;
    // the file name from the URL; the download lands in a unique temp file, so
    // without this the tab would be labelled e.g. "sum3236.tmp.pdf"
    Str displayName;
    MainWindow* win = nullptr;
};

static void TbDocDownloadFinish(TbDocDownload* d) {
    if (file::Exists(d->destPath)) {
        TbRecordDownload(d->destPath);
    }
    if (IsMainWindowValid(d->win) && file::Exists(d->destPath)) {
        LoadArgs args(d->destPath, d->win);
        if (d->displayName) {
            args.SetDisplayName(d->displayName);
        }
        LoadDocument(&args);
        SetTouchView(d->win, TouchView::Doc);
    }
    str::Free(d->url);
    str::Free(d->destPath);
    str::Free(d->displayName);
    delete d;
}

static void TbDocDownloadAsync(TbDocDownload* d) {
    constexpr i64 kMaxWebDocSize = 256LL * 1024 * 1024;
    HttpGetToFile(d->url, d->destPath, {}, kMaxWebDocSize);
    uitask::Post(MkFunc0<TbDocDownload>(TbDocDownloadFinish, d), "TbDocDownloadFinish");
}

// Downloads `url` into the Downloads folder and opens it as a SumatraPDF tab.
// `ext` is the extension the file should end up with ("" = whatever the URL
// already says); a content-type detected document usually has no extension in
// its URL at all, and SumatraPDF picks its engine from the file name.
static void TbStartDocDownload(MainWindow* win, Str url, Str ext) {
    if (!url) {
        return;
    }
    auto* d = new TbDocDownload();
    d->win = win;
    d->url = str::Dup(url);
    TempStr dir = TbDownloadsDirTemp();
    TempStr fileName = TbUrlFileNameTemp(url);
    if (ext && !str::EndsWithI(fileName, ext)) {
        Str base = str::IsEmptyOrWhiteSpace(fileName) ? StrL("document") : Str(fileName);
        fileName = str::JoinTemp(base, ext);
    }
    if (dir && fileName) {
        TempStr want = path::JoinTemp(dir, fileName);
        d->destPath = str::Dup(MakeUniqueFilePathTemp(want));
    } else {
        TempStr base = GetTempFilePathTemp("sumatra-web");
        d->destPath = str::Dup(str::JoinTemp(base, ext ? ext : StrL(".dat")));
    }
    if (fileName) {
        d->displayName = str::Dup(fileName);
    }
    RunAsync(MkFunc0<TbDocDownload>(TbDocDownloadAsync, d), "TbDocDownloadAsync");
}

static void TbSetChildrenVisible(TouchBrowser* tb, bool show);
static void TbActivateTab(TouchBrowser* tb, int idx);
// opens `url` in a new browser tab, from the message loop rather than inline
static void TbRequestNewTab(MainWindow* win, Str url);
// repaint the chrome (a tab title changed, history changed, ...)
static void TbRedrawChrome(TouchBrowser* tb);
// repaint AND re-measure: the row set or the tab count changed. The favorites
// bar appears and disappears with the favorites, which changes the chrome's
// height, so the frame has to lay out again too.
static void TbRelayoutChrome(TouchBrowser* tb);
// screen rect of the "..." button, which the popups anchor to
static Rect TbMenuAnchorScreenRect(TouchBrowser* tb);
static void TbShowFavMgr(TouchBrowser* tb);
static void TbShowMenu(TouchBrowser* tb);
static void TbOnNewTab(TouchBrowser* tb);
static void TbCloseTab(TbTab* t);
// a tab started / finished loading: drives the nav row's progress bar, which
// only ever reflects the ACTIVE tab
static void TbSetTabLoading(TbTab* t, bool loading);
// the tab is no longer showing a document, so the "Open in SumatraPDF" button
// goes away
static void TbClearDocPage(TbTab* t);

static TbTab* TbActiveTab(TouchBrowser* tb) {
    if (tb->activeTab < 0 || tb->activeTab >= len(tb->tabs)) {
        return nullptr;
    }
    return tb->tabs[tb->activeTab];
}

static WebviewWnd* TbActiveWebView(TouchBrowser* tb) {
    TbTab* t = TbActiveTab(tb);
    return t ? t->webView : nullptr;
}

static bool TbIsActiveTab(TbTab* t) {
    return t == TbActiveTab(t->tb);
}

// navigationStarting: intercept links to documents so they open as tabs in
// SumatraPDF+ instead of navigating the webview; everything else proceeds.
// Also fired (with newWindow) for target=_blank / window.open, which the
// webview has already declined to handle - we turn those into browser tabs.
static bool TbNavigationStarting(void* ctx, Str url, bool newWindow) {
    auto* tab = (TbTab*)ctx;
    TouchBrowser* tb = tab->tb;
    Str ext;
    if (TouchBrowserUrlIsDoc(url, &ext)) {
        // save into Downloads under the URL's own file name
        TbStartDocDownload(tb->win, url, ext);
        return false; // cancel the webview navigation
    }
    if (newWindow) {
        TbRequestNewTab(tb->win, url);
        return false;
    }
    // this tab is leaving whatever it was showing, document or not
    TbClearDocPage(tab);
    TbSetTabLoading(tab, true);
    // This tab is really leaving its page, so drop the title it had: the strip
    // falls back to the host until the new page reports one. Done here and not
    // in navigationCompleted because DocumentTitleChanged arrives first (the
    // title is known as soon as the document is parsed) and clearing later
    // would throw the new title away. Navigations we cancelled above returned
    // before this, so they keep the title of the page still on screen.
    str::FreePtr(&tab->title);
    TbRedrawChrome(tb);
    return true;
}

// the nav row always reflects the ACTIVE tab, so background tabs never write to it
static void TbSyncUrlBar(TouchBrowser* tb);

static void TbUpdateNavButtons(TouchBrowser* tb) {
    WebviewWnd* wv = TbActiveWebView(tb);
    tb->canGoBack = wv && wv->CanGoBack();
    tb->canGoForward = wv && wv->CanGoForward();
    TbRedrawChrome(tb);
}

static void TbNavigationCompleted(void* ctx, Str url, bool /*success*/) {
    auto* tab = (TbTab*)ctx;
    str::ReplaceWithCopy(&tab->url, url);
    // both success and failure land here - including a navigation WebView2
    // cancelled - so the progress bar always gets to finish
    TbSetTabLoading(tab, false);
    // navigationStarting dropped the old title, and WebView2 only raises
    // documentTitleChanged when the title CHANGES - so a reload (or a
    // back/forward to a page whose title equals the one we just discarded)
    // would leave the tab and any favorite made from it labelled by host only
    if (str::IsEmptyOrWhiteSpace(tab->title) && tab->webView) {
        TempStr t = tab->webView->GetDocumentTitle();
        if (!str::IsEmptyOrWhiteSpace(t)) {
            str::ReplaceWithCopy(&tab->title, t);
        }
    }
    TbRedrawChrome(tab->tb);
    if (TbIsActiveTab(tab)) {
        TbSyncUrlBar(tab->tb);
        TbUpdateNavButtons(tab->tb);
    }
}

// --- documents behind an extension-less URL ---------------------------------
// TouchBrowserUrlIsDoc only reads the URL, so a PDF served from /download?id=42
// or an extension-less route slipped through and WebView2 rendered it in Edge's
// built-in PDF viewer. The main document's Content-Type is the authoritative
// answer and arrives before the page renders, so the browser hands the URL to
// the same download-and-open path and takes the webview back off the document.

// how long a page has to be away before the same URL may be auto-handed-off
// again. A site that redirects its own back-navigation straight to the document
// would otherwise bounce forever; after this guard fires the user still gets the
// "Open in SumatraPDF" button.
constexpr double kTbTakeoverGuardMs = 8000.0;

struct TbDocTakeoverReq {
    MainWindow* win = nullptr;
    TbTab* tab = nullptr;
    Str url;
    Str ext;
};

// Runs from the message loop: the detection fires inside a WebView2 event
// handler, and neither navigating the control nor starting a download belongs
// in there. `tab` may be gone by now, so it is re-validated against the live
// browser first (same contract as TbCloseTabNow).
static void TbDocTakeoverNow(TbDocTakeoverReq* req) {
    MainWindow* win = req->win;
    TbTab* tab = req->tab;
    Str url = req->url;
    Str ext = req->ext;
    bool valid = IsMainWindowValid(win) && win->touchBrowser && win->touchBrowser->tabs.Find(tab) >= 0;
    if (valid) {
        tab->isDocPage = true;
        str::ReplaceWithCopy(&tab->docPageUrl, url);
        str::ReplaceWithCopy(&tab->docPageExt, ext);
        double now = AnimNowMs();
        bool sameAsLast = tab->lastTakeoverUrl && str::EqI(tab->lastTakeoverUrl, url) &&
                          (now - tab->lastTakeoverMs) < kTbTakeoverGuardMs;
        if (!sameAsLast) {
            str::ReplaceWithCopy(&tab->lastTakeoverUrl, url);
            tab->lastTakeoverMs = now;
            TbStartDocDownload(win, url, ext);
            // ...and get out of Edge's viewer, back to the page the link was on
            WebviewWnd* wv = tab->webView;
            if (wv) {
                if (wv->CanGoBack()) {
                    wv->GoBack();
                } else {
                    wv->Navigate(TouchBrowserHomeUrl());
                }
            }
        }
        TbRelayoutChrome(win->touchBrowser);
    }
    str::Free(req->url);
    str::Free(req->ext);
    delete req;
}

static void TbMainDocumentResponse(void* ctx, Str url, Str contentType) {
    auto* tab = (TbTab*)ctx;
    FileType ft = TouchBrowserFileTypeFromContentType(contentType);
    if (ft == FileType::Unknown || !TouchBrowserFileTypeIsDownloadableDoc(ft)) {
        return; // ordinary web content: keep browsing
    }
    auto* req = new TbDocTakeoverReq();
    req->win = tab->tb->win;
    req->tab = tab;
    req->url = str::Dup(url);
    req->ext = str::Dup(TouchBrowserExtForFileType(ft));
    uitask::Post(MkFunc0<TbDocTakeoverReq>(TbDocTakeoverNow, req), "TbDocTakeoverNow");
}

// the escape hatch: opens whatever the active tab is showing as a document,
// used when the automatic hand-off did not fire
static void TbOpenPageAsDoc(TouchBrowser* tb) {
    TbTab* act = TbActiveTab(tb);
    if (!act) {
        return;
    }
    Str url = act->docPageUrl ? act->docPageUrl : act->url;
    if (!url) {
        return;
    }
    Str ext = act->docPageExt ? act->docPageExt : StrL(".pdf");
    TbStartDocDownload(tb->win, url, ext);
}

static void TbDocumentTitleChanged(void* ctx, Str title) {
    auto* tab = (TbTab*)ctx;
    str::ReplaceWithCopy(&tab->title, title);
    TbRedrawChrome(tab->tb);
}

static void TbHistoryChanged(void* ctx, bool canBack, bool canFwd) {
    auto* tab = (TbTab*)ctx;
    if (!TbIsActiveTab(tab)) {
        return;
    }
    TouchBrowser* tb = tab->tb;
    tb->canGoBack = canBack;
    tb->canGoForward = canFwd;
    TbRedrawChrome(tb);
}

static void TbOnBack(TouchBrowser* tb) {
    WebviewWnd* wv = TbActiveWebView(tb);
    if (wv) {
        wv->GoBack();
    }
}
static void TbOnForward(TouchBrowser* tb) {
    WebviewWnd* wv = TbActiveWebView(tb);
    if (wv) {
        wv->GoForward();
    }
}
static void TbOnHome(TouchBrowser* tb) {
    WebviewWnd* wv = TbActiveWebView(tb);
    if (wv) {
        wv->Navigate(TouchBrowserHomeUrl());
    }
}
// "i": explain where documents opened from the browser end up
static void TbOnInfo(TouchBrowser* tb) {
    TempStr dir = TbDownloadsDirTemp();
    Str where = dir ? Str(dir) : StrL("(Downloads folder not found)");
    TempStr msg =
        fmt("Documents you open from the browser (PDF, EPUB, MOBI, CBZ, DjVu, XPS, CHM) are "
            "downloaded and saved to your Downloads folder:\n\n%s\n\n"
            "They stay there until you delete them. Use the menu button next to this one and "
            "choose 'Clean up downloaded files' to remove the ones SumatraPDF+ downloaded.",
            where);
    MsgBox(tb->win ? tb->win->hwndFrame : nullptr, msg, StrL("Downloads"), MB_OK | MB_ICONINFORMATION);
}

// delete exactly the files this browser downloaded (tracked in prefs)
static void TbCleanupDownloads(TouchBrowser* tb) {
    HWND parent = tb->win ? tb->win->hwndFrame : nullptr;
    Vec<Str>* v = gGlobalPrefs->browserDownloads;
    int nExisting = 0;
    for (int i = 0; v && i < len(*v); i++) {
        if (file::Exists((*v)[i])) {
            nExisting++;
        }
    }
    if (nExisting == 0) {
        MsgBox(parent, StrL("No downloaded files to clean up."), StrL("Clean up downloads"),
               MB_OK | MB_ICONINFORMATION);
        if (v) {
            for (Str p2 : *v) {
                str::Free(p2);
            }
            v->Reset();
            SaveSettings();
        }
        return;
    }
    TempStr ask = fmt("Delete %d file%s downloaded by SumatraPDF+ from your Downloads folder?", nExisting,
                      nExisting == 1 ? StrL("") : StrL("s"));
    int res = MsgBox(parent, ask, StrL("Clean up downloads"), MB_YESNO | MB_ICONQUESTION);
    if (res != IDYES) {
        return;
    }
    int nDeleted = 0;
    for (int i = 0; i < len(*v); i++) {
        Str fp = (*v)[i];
        if (file::Exists(fp) && file::Delete(fp)) {
            nDeleted++;
        }
    }
    for (Str p2 : *v) {
        str::Free(p2);
    }
    v->Reset();
    SaveSettings();
    MsgBox(parent, fmt("Deleted %d file%s.", nDeleted, nDeleted == 1 ? StrL("") : StrL("s")),
           StrL("Clean up downloads"), MB_OK | MB_ICONINFORMATION);
}

// make the page in the active tab the browser's home page
static void TbSetHomePage(TouchBrowser* tb) {
    TbTab* at = TbActiveTab(tb);
    Str url = at ? at->url : Str{};
    if (!url) {
        return;
    }
    str::ReplaceWithCopy(&gGlobalPrefs->browserHomePage, url);
    SaveSettings();
    MsgBox(tb->win ? tb->win->hwndFrame : nullptr, fmt("Home page set to:\n\n%s", url), StrL("Home page"),
           MB_OK | MB_ICONINFORMATION);
}

// --- favorites -------------------------------------------------------------

static int TbFavCount() {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    return bm ? len(*bm) : 0;
}

static Str TbFavAt(int idx) {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    if (!bm || idx < 0 || idx >= len(*bm)) {
        return {};
    }
    return (*bm)[idx];
}

// --- favorite display names ------------------------------------------------
// A favorite is a URL in browserBookmarks plus a display name at the SAME index
// in browserFavoriteTitles. Two parallel arrays rather than one array of pairs
// because browserBookmarks already exists and is written by older builds; a
// settings file that predates titles simply has a short (or absent) title list,
// which every read here tolerates.
//
// The name is never stored empty. SerializeUtf8StringArray writes an empty
// string as nothing at all, and the parser then skips it - one empty title
// would silently shift every later title onto the wrong URL. So a favorite with
// no page title is stored under its host label instead.

static Vec<Str>* TbFavTitlesVec() {
    if (!gGlobalPrefs->browserFavoriteTitles) {
        gGlobalPrefs->browserFavoriteTitles = new Vec<Str>();
    }
    return gGlobalPrefs->browserFavoriteTitles;
}

// read-only and index-safe: painting must never mutate prefs
static Str TbFavTitleAt(int idx) {
    Vec<Str>* t = gGlobalPrefs->browserFavoriteTitles;
    if (!t || idx < 0 || idx >= len(*t)) {
        return {};
    }
    return (*t)[idx];
}

// what a favorites chip and a manager row say: the stored name, falling back to
// the URL's host when there is none (old settings file, or a page with no title)
static TempStr TbFavLabel(int idx) {
    Str title = TbFavTitleAt(idx);
    if (!str::IsEmptyOrWhiteSpace(title)) {
        return str::DupTemp(title);
    }
    return TbChipLabel(TbFavAt(idx));
}

// the name to STORE for (title, url); guaranteed non-empty (see above)
static TempStr TbFavStoreTitle(Str title, Str url) {
    if (!str::IsEmptyOrWhiteSpace(title)) {
        return str::DupTemp(title);
    }
    TempStr host = TbChipLabel(url);
    if (!str::IsEmptyOrWhiteSpace(host)) {
        return host;
    }
    if (!str::IsEmptyOrWhiteSpace(url)) {
        return str::DupTemp(url);
    }
    return str::DupTemp(StrL("Favorite"));
}

// Brings the title array to exactly len(bookmarks) entries, all non-empty, so
// add / delete / move afterwards can treat the two arrays as one. Every mutation
// calls this FIRST; that is what keeps them from drifting apart even if a hand-
// edited settings file arrives with the wrong number of titles.
static void TbFavSyncTitles() {
    int n = TbFavCount();
    Vec<Str>* t = TbFavTitlesVec();
    while (len(*t) > n) {
        str::Free(t->Pop());
    }
    while (len(*t) < n) {
        t->Append(str::Dup(TbFavStoreTitle({}, TbFavAt(len(*t)))));
    }
    for (int i = 0; i < n; i++) {
        if (str::IsEmptyOrWhiteSpace((*t)[i])) {
            str::Free((*t)[i]);
            (*t)[i] = str::Dup(TbFavStoreTitle({}, TbFavAt(i)));
        }
    }
}

// after any change to gGlobalPrefs->browserBookmarks: persist it and put the
// favorites bar back in sync with it
static void TbFavRefresh(TouchBrowser* tb) {
    SaveSettings();
    TbRelayoutChrome(tb);
}

static void TbFavDelete(TouchBrowser* tb, int idx) {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    if (!bm || idx < 0 || idx >= len(*bm)) {
        return;
    }
    TbFavSyncTitles();
    Vec<Str>* ti = TbFavTitlesVec();
    str::Free((*bm)[idx]);
    bm->RemoveAt(idx);
    str::Free((*ti)[idx]);
    ti->RemoveAt(idx);
    TbFavRefresh(tb);
}

static void TbFavMove(TouchBrowser* tb, int from, int to) {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    if (!bm) {
        return;
    }
    int n = len(*bm);
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return;
    }
    TbFavSyncTitles();
    Vec<Str>* ti = TbFavTitlesVec();
    Str s = bm->PopAt(from);
    bm->InsertAt(to, s);
    Str t = ti->PopAt(from);
    ti->InsertAt(to, t);
    TbFavRefresh(tb);
}

// rename from the manager's pencil button. An empty name resets the favorite to
// its host label rather than storing nothing.
static void TbFavRename(TouchBrowser* tb, int idx, Str name) {
    if (idx < 0 || idx >= TbFavCount()) {
        return;
    }
    TbFavSyncTitles();
    Vec<Str>* ti = TbFavTitlesVec();
    TempStr want = TbFavStoreTitle(name, TbFavAt(idx));
    if (str::Eq((*ti)[idx], want)) {
        return;
    }
    str::ReplaceWithCopy(&(*ti)[idx], want);
    TbFavRefresh(tb);
}

// the favorites bar and the nav row's "Favorite" button navigate the active tab
static void TbOnFavClick(TouchBrowser* tb, int idx) {
    Str url = TbFavAt(idx);
    WebviewWnd* wv = TbActiveWebView(tb);
    if (wv && url) {
        wv->Navigate(url);
    }
}

// is the page in the active tab already a favorite?
static bool TbCurrentIsFav(TouchBrowser* tb) {
    TbTab* act = TbActiveTab(tb);
    Str url = act ? act->url : Str();
    if (!url) {
        return false;
    }
    int n = TbFavCount();
    for (int i = 0; i < n; i++) {
        if (str::EqI(TbFavAt(i), url)) {
            return true;
        }
    }
    return false;
}

// --- touch popups ----------------------------------------------------------
// The "..." menu and the favorites manager are custom WS_POPUP windows rather
// than TrackPopupMenu / a dialog: system menu metrics give ~20px rows, which
// are unusable with a finger, and neither can be made to look like the design.
// Each gets its own window class so it can carry CS_DROPSHADOW without
// affecting every other Wnd, and a rounded window region so the corners are
// really clipped rather than merely painted round.

static Kind kindTbMenu = "tbMenu";
static Kind kindTbFavMgr = "tbFavMgr";
static Kind kindTbScrim = "tbScrim";
static Kind kindTbChrome = "tbChrome";

static void TbAddDropShadow(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    LONG_PTR st = GetClassLongPtrW(hwnd, GCL_STYLE);
    SetClassLongPtrW(hwnd, GCL_STYLE, st | CS_DROPSHADOW);
}

static void TbSetRoundedRegion(HWND hwnd, int dx, int dy, int radius) {
    if (!hwnd || dx <= 0 || dy <= 0) {
        return;
    }
    HRGN rgn = CreateRoundRectRgn(0, 0, dx + 1, dy + 1, radius * 2, radius * 2);
    SetWindowRgn(hwnd, rgn, TRUE); // the window owns the region now
}

// relative luminance test, used to pick the darker of two theme colors
static int TbLuminance(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

// the modal scrim's tint. #1c1a17 on Touch Paper, but a scrim has to DARKEN in
// every theme, so take whichever of the theme's text / window colors is darker
// (text on a light theme, the background on a dark one).
static COLORREF TbScrimColor() {
    COLORREF a = ThemeWindowTextColor();
    COLORREF b = ThemeWindowBackgroundColor();
    return TbLuminance(a) <= TbLuminance(b) ? a : b;
}

// --- the modal scrim -------------------------------------------------------
// A layered popup covering the whole frame at 35% alpha. It is what makes the
// favorites manager modal: it swallows every click meant for the window below,
// and a click on it closes the dialog.

static void TbHideFavMgr(TouchBrowser* tb);

struct TbScrimWnd : Wnd {
    TbScrimWnd();
    bool Create(TouchBrowser*);
    void ShowOverFrame();
    void Hide();
    void OnPaint(HDC, PAINTSTRUCT*) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    TouchBrowser* tb = nullptr;
};

TbScrimWnd::TbScrimWnd() {
    kind = kindTbScrim;
}

bool TbScrimWnd::Create(TouchBrowser* browser) {
    tb = browser;
    CreateCustomArgs args;
    args.className = L"SumatraTouchBrowserScrim";
    args.visible = false;
    args.style = WS_POPUP;
    // NOACTIVATE so clicking the scrim doesn't deactivate (and so close) the
    // dialog before the scrim's own click handler runs
    args.exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    args.pos = {0, 0, 10, 10};
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)tb->win->hwndFrame);
    SetLayeredWindowAttributes(hwnd, 0, 89, LWA_ALPHA); // .35 * 255
    return true;
}

void TbScrimWnd::ShowOverFrame() {
    Rect fr = HwndWindowRect(tb->win->hwndFrame);
    SetWindowPos(hwnd, HWND_TOP, fr.x, fr.y, fr.dx, fr.dy, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    HwndInvalidate(hwnd, true);
}

void TbScrimWnd::Hide() {
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void TbScrimWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    HdcFillRect(hdc, ToRect(ps->rcPaint), TbScrimColor());
}

LRESULT TbScrimWnd::WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        TbHideFavMgr(tb);
        return 0;
    }
    return WndProcDefault(hw, msg, wp, lp);
}

// --- favorites manager -----------------------------------------------------

enum class TbFavPart {
    None,
    Row,
    Rename,
    Up,
    Down,
    Delete
};

struct TbFavMgrWnd : Wnd {
    TbFavMgrWnd();
    ~TbFavMgrWnd() override;
    bool Create(TouchBrowser*);
    void ShowAt();
    void Hide();
    void OnPaint(HDC, PAINTSTRUCT*) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    int HeaderDy() const;
    int FooterDy() const;
    int RowDy() const;
    Rect ListRect() const;
    Rect RowRect(int idx) const;
    Rect BtnRect(int idx, TbFavPart part) const;
    Rect NameRect(int idx) const;
    Rect DoneRect() const;
    int RowAt(Point pt) const;
    TbFavPart PartAt(Point pt, int* idxOut) const;
    int MaxScrollY() const;

    // in-place rename: a real EDIT parked over the row's name, so the caret,
    // selection, IME and clipboard all come for free
    void BeginRename(int idx);
    void CommitRename();
    void CancelRename();

    TouchBrowser* tb = nullptr;
    // the row the left button went down on, and which part of it (a press on a
    // button suppresses dragging)
    int pressedIdx = -1;
    TbFavPart pressedPart = TbFavPart::None;
    Point pressPt;
    bool dragging = false;
    int dragTo = -1;
    int hotIdx = -1;
    TbFavPart hotPart = TbFavPart::None;
    bool doneHot = false;
    bool donePressed = false;
    bool tracking = false;
    int scrollY = 0;
    HWND hwndEdit = nullptr;
    // row being renamed, or -1. `editClosing` keeps the EN_KILLFOCUS that
    // hiding the EDIT provokes from re-entering the commit.
    int editIdx = -1;
    bool editClosing = false;
    HBRUSH editBrush = nullptr;
    COLORREF editBrushColor = kColorUnset;
};

TbFavMgrWnd::TbFavMgrWnd() {
    kind = kindTbFavMgr;
}

TbFavMgrWnd::~TbFavMgrWnd() {
    if (editBrush) {
        DeleteObject(editBrush);
    }
    // ~Wnd destroys hwnd, which would take the EDIT with it, but the order is
    // explicit here so the subclass never sees a half-torn-down owner
    if (hwndEdit) {
        DestroyWindow(hwndEdit);
        hwndEdit = nullptr;
    }
}

bool TbFavMgrWnd::Create(TouchBrowser* browser) {
    tb = browser;
    CreateCustomArgs args;
    args.className = L"SumatraTouchBrowserFavMgr";
    args.visible = false;
    args.style = WS_POPUP;
    args.exStyle = WS_EX_TOOLWINDOW;
    args.pos = {0, 0, 10, 10};
    args.bgColor = ThemeWindowControlBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)tb->win->hwndFrame);
    TbAddDropShadow(hwnd);
    return true;
}

int TbFavMgrWnd::HeaderDy() const {
    return DpiScale(hwnd, kTbFavMgrHeaderDy);
}
int TbFavMgrWnd::FooterDy() const {
    return DpiScale(hwnd, kTbFavMgrFooterDy);
}
int TbFavMgrWnd::RowDy() const {
    return DpiScale(hwnd, kTbFavMgrRowDy);
}

Rect TbFavMgrWnd::ListRect() const {
    Rect rc = HwndClientRect(hwnd);
    int top = HeaderDy();
    int dy = std::max(0, rc.dy - top - FooterDy());
    return {0, top, rc.dx, dy};
}

Rect TbFavMgrWnd::RowRect(int idx) const {
    Rect list = ListRect();
    int padX = DpiScale(hwnd, kTbFavMgrListPadX);
    int padY = DpiScale(hwnd, kTbFavMgrListPadY);
    int y = list.y + padY - scrollY + idx * RowDy();
    return {list.x + padX, y, std::max(0, list.dx - 2 * padX), RowDy()};
}

Rect TbFavMgrWnd::BtnRect(int idx, TbFavPart part) const {
    Rect r = RowRect(idx);
    int pad = DpiScale(hwnd, kTbFavMgrRowPad);
    int gap = DpiScale(hwnd, kTbFavMgrRowGap);
    int d = DpiScale(hwnd, kTbFavMgrBtnDx);
    int right = r.x + r.dx - pad;
    int y = r.y + (r.dy - d) / 2;
    switch (part) {
        case TbFavPart::Delete:
            return {right - d, y, d, d};
        case TbFavPart::Down:
            return {right - 2 * d - gap, y, d, d};
        case TbFavPart::Up:
            return {right - 3 * d - 2 * gap, y, d, d};
        case TbFavPart::Rename:
            return {right - 4 * d - 3 * gap, y, d, d};
        default:
            return {};
    }
}

// where a row's name is drawn - and where the rename EDIT is parked
Rect TbFavMgrWnd::NameRect(int idx) const {
    Rect r = RowRect(idx);
    int pad = DpiScale(hwnd, kTbFavMgrRowPad);
    int gap = DpiScale(hwnd, kTbFavMgrRowGap);
    int gripDx = DpiScale(hwnd, kTbFavMgrGripDx);
    int x = r.x + pad + gripDx + gap;
    Rect firstBtn = BtnRect(idx, TbFavPart::Rename);
    return {x, r.y, std::max(0, firstBtn.x - gap - x), r.dy};
}

Rect TbFavMgrWnd::DoneRect() const {
    Rect rc = HwndClientRect(hwnd);
    int padX = DpiScale(hwnd, kTbFavMgrHeadPadX);
    int dy = DpiScale(hwnd, kTbFavMgrDoneDy);
    int dx = DpiScale(hwnd, 40 + 2 * kTbFavMgrDonePadX); // "Done" at 14px + padding
    int y = rc.dy - FooterDy() + (FooterDy() - dy) / 2;
    return {rc.dx - padX - dx, y, dx, dy};
}

int TbFavMgrWnd::MaxScrollY() const {
    int n = TbFavCount();
    int padY = DpiScale(hwnd, kTbFavMgrListPadY);
    int contentDy = (n > 0 ? n * RowDy() : DpiScale(hwnd, kTbFavMgrEmptyDy)) + 2 * padY;
    return std::max(0, contentDy - ListRect().dy);
}

int TbFavMgrWnd::RowAt(Point pt) const {
    if (!ListRect().Contains(pt)) {
        return -1;
    }
    int n = TbFavCount();
    for (int i = 0; i < n; i++) {
        if (RowRect(i).Contains(pt)) {
            return i;
        }
    }
    return -1;
}

TbFavPart TbFavMgrWnd::PartAt(Point pt, int* idxOut) const {
    int idx = RowAt(pt);
    *idxOut = idx;
    if (idx < 0) {
        return TbFavPart::None;
    }
    if (BtnRect(idx, TbFavPart::Delete).Contains(pt)) {
        return TbFavPart::Delete;
    }
    if (BtnRect(idx, TbFavPart::Down).Contains(pt)) {
        return TbFavPart::Down;
    }
    if (BtnRect(idx, TbFavPart::Up).Contains(pt)) {
        return TbFavPart::Up;
    }
    if (BtnRect(idx, TbFavPart::Rename).Contains(pt)) {
        return TbFavPart::Rename;
    }
    return TbFavPart::Row;
}

// Enter commits, Esc cancels; losing focus commits too (handled via
// EN_KILLFOCUS in the manager's WndProc), which is what a click anywhere else
// in the dialog does.
static LRESULT CALLBACK TbFavEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* mgr = (TbFavMgrWnd*)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (mgr) {
            mgr->CommitRename();
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (mgr) {
            mgr->CancelRename();
        }
        return 0;
    }
    // Enter / Esc still arrive as control characters, which the default handler
    // would beep at (MessageBeep on an EDIT that has no ES_MULTILINE)
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_CHAR && wp == 1) {
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void TbFavMgrWnd::BeginRename(int idx) {
    if (idx < 0 || idx >= TbFavCount()) {
        return;
    }
    if (editIdx == idx) {
        return;
    }
    CommitRename(); // a rename already open on another row
    if (!hwndEdit) {
        hwndEdit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, nullptr,
                                   GetInstance(), nullptr);
        if (!hwndEdit) {
            return;
        }
        SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)tb->hFont, TRUE);
        SetWindowSubclass(hwndEdit, TbFavEditProc, NextSubclassId(), (DWORD_PTR)this);
    }
    editIdx = idx;
    Rect r = NameRect(idx);
    int inset = DpiScale(hwnd, 4);
    MoveWindow(hwndEdit, r.x, r.y + inset, std::max(0, r.dx), std::max(0, r.dy - 2 * inset), TRUE);
    HwndSetText(hwndEdit, TbFavLabel(idx));
    ShowWindow(hwndEdit, SW_SHOW);
    SetFocus(hwndEdit);
    SendMessageW(hwndEdit, EM_SETSEL, 0, -1);
    HwndInvalidate(hwnd, false);
}

void TbFavMgrWnd::CommitRename() {
    if (editIdx < 0 || editClosing) {
        return;
    }
    editClosing = true;
    int idx = editIdx;
    TempStr name = hwndEdit ? HwndGetTextTemp(hwndEdit) : TempStr();
    editIdx = -1;
    if (hwndEdit) {
        ShowWindow(hwndEdit, SW_HIDE);
    }
    editClosing = false;
    // TbFavRename saves the settings and rebuilds the favorites bar; it never
    // touches this window, so running it from here is safe
    TbFavRename(tb, idx, name);
    HwndInvalidate(hwnd, false);
}

void TbFavMgrWnd::CancelRename() {
    if (editIdx < 0) {
        return;
    }
    editClosing = true;
    editIdx = -1;
    if (hwndEdit) {
        ShowWindow(hwndEdit, SW_HIDE);
    }
    editClosing = false;
    HwndInvalidate(hwnd, false);
}

void TbFavMgrWnd::Hide() {
    CancelRename();
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    pressedIdx = -1;
    pressedPart = TbFavPart::None;
    dragging = false;
    dragTo = -1;
    hotIdx = -1;
    hotPart = TbFavPart::None;
    doneHot = false;
    donePressed = false;
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void TbFavMgrWnd::ShowAt() {
    CancelRename();
    pressedIdx = -1;
    pressedPart = TbFavPart::None;
    dragging = false;
    dragTo = -1;
    hotIdx = -1;
    hotPart = TbFavPart::None;
    scrollY = 0;

    int n = TbFavCount();
    int padY = DpiScale(hwnd, kTbFavMgrListPadY);
    int dx = DpiScale(hwnd, kTbFavMgrDx);
    int contentDy = (n > 0 ? n * RowDy() : DpiScale(hwnd, kTbFavMgrEmptyDy)) + 2 * padY;
    int dy = HeaderDy() + contentDy + FooterDy();
    dy = std::min(dy, DpiScale(hwnd, kTbFavMgrMaxDy));

    // centred over the frame, like a modal dialog
    Rect fr = HwndWindowRect(tb->win->hwndFrame);
    int x = fr.x + (fr.dx - dx) / 2;
    int y = fr.y + (fr.dy - dy) / 2;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromWindow(tb->win->hwndFrame, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &mi);
    Rect work = ToRect(mi.rcWork);
    x = std::clamp(x, work.x, std::max(work.x, work.x + work.dx - dx));
    y = std::clamp(y, work.y, std::max(work.y, work.y + work.dy - dy));

    SetWindowPos(hwnd, HWND_TOP, x, y, dx, dy, SWP_SHOWWINDOW);
    TbSetRoundedRegion(hwnd, dx, dy, DpiScale(hwnd, kTbFavMgrRadius));
    SetForegroundWindow(hwnd);
    HwndInvalidate(hwnd, true);
}

void TbFavMgrWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    Rect rc = HwndClientRect(hwnd);
    COLORREF panel = ThemeWindowControlBackgroundColor();
    HdcFillRect(hdc, ToRect(ps->rcPaint), panel);
    SetBkMode(hdc, TRANSPARENT);

    int headPadX = DpiScale(hwnd, kTbFavMgrHeadPadX);
    int headerDy = HeaderDy();
    // header
    {
        Rect r{headPadX, 0, std::max(0, rc.dx - 2 * headPadX), headerDy};
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, StrL("Manage favorites"), r,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kTbFontDlgTitle, FW_SEMIBOLD));
        HdcFillRect(hdc, Rect{0, headerDy - 1, rc.dx, 1}, ThemeEdgeColor());
    }

    Rect list = ListRect();
    int n = TbFavCount();
    if (n == 0) {
        Rect r = list;
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("No favorites yet."), r,
                    DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kTbFontUrl));
    } else {
        // the list scrolls, so nothing may spill into the header or the footer
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, list.x, list.y, list.x + list.dx, list.y + list.dy);
        int pad = DpiScale(hwnd, kTbFavMgrRowPad);
        int gap = DpiScale(hwnd, kTbFavMgrRowGap);
        int gripDx = DpiScale(hwnd, kTbFavMgrGripDx);
        float radius = (float)DpiScale(hwnd, kTbFavMgrRowRadius);
        for (int i = 0; i < n; i++) {
            Rect r = RowRect(i);
            if (r.y > list.y + list.dy || r.y + r.dy < list.y) {
                continue;
            }
            bool isDragged = dragging && i == pressedIdx;
            if (i == hotIdx || isDragged) {
                TbFillRounded(hdc, r, radius, ThemeHotBackgroundColor());
            }
            HdcFillRect(hdc, Rect{r.x, r.y + r.dy - 1, r.dx, 1}, ThemeEdgeColor());

            Rect grip{r.x + pad, r.y + (r.dy - gripDx) / 2, gripDx, gripDx};
            TbDrawGrip(hdc, hwnd, grip, ThemeWindowDarkerTextColor());

            Rect ren = BtnRect(i, TbFavPart::Rename);
            Rect up = BtnRect(i, TbFavPart::Up);
            Rect down = BtnRect(i, TbFavPart::Down);
            Rect del = BtnRect(i, TbFavPart::Delete);
            float btnRadius = (float)DpiScale(hwnd, kTbFavMgrBtnRadius);
            auto btnBg = [&](TbFavPart part) {
                bool hot = (i == hotIdx && hotPart == part);
                return hot ? ThemeDisabledEdgeColor() : ThemeTouchSurfaceColor();
            };
            bool canUp = i > 0;
            bool canDown = i < n - 1;
            TbFillRounded(hdc, ren, btnRadius, btnBg(TbFavPart::Rename), ThemeEdgeColor());
            TbFillRounded(hdc, up, btnRadius, btnBg(TbFavPart::Up), ThemeEdgeColor());
            TbFillRounded(hdc, down, btnRadius, btnBg(TbFavPart::Down), ThemeEdgeColor());
            TbFillRounded(hdc, del, btnRadius, btnBg(TbFavPart::Delete), ThemeEdgeColor());
            TbDrawPencil(hdc, hwnd, ren, ThemeWindowDarkerTextColor());
            TbDrawChevron(hdc, hwnd, up, canUp ? ThemeWindowDarkerTextColor() : ThemeWindowTextDisabledColor(), true);
            TbDrawChevron(hdc, hwnd, down, canDown ? ThemeWindowDarkerTextColor() : ThemeWindowTextDisabledColor(),
                          false);
            TbDrawTrash(hdc, hwnd, del, ThemeWindowLinkColor());

            // the row being renamed shows the EDIT instead of its label
            if (i != editIdx) {
                Rect name = NameRect(i);
                SetTextColor(hdc, ThemeWindowTextColor());
                HdcDrawText(hdc, TbFavLabel(i), name,
                            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX,
                            HdcGetUiFont(hdc, kTbFontUrl));
            }
        }
        // insertion marker: where the dragged row would land on mouse-up
        if (dragging && dragTo >= 0 && dragTo != pressedIdx) {
            Rect r = RowRect(dragTo);
            int y = (dragTo > pressedIdx) ? (r.y + r.dy) : r.y;
            HdcFillRect(hdc, Rect{r.x, y - DpiScale(hwnd, 1), r.dx, DpiScale(hwnd, 2)}, ThemeWindowLinkColor());
        }
        RestoreDC(hdc, saved);
    }

    // footer
    {
        int footTop = rc.dy - FooterDy();
        HdcFillRect(hdc, Rect{0, footTop, rc.dx, 1}, ThemeEdgeColor());
        Rect done = DoneRect();
        COLORREF bg = donePressed ? ThemeEdgeColor() : (doneHot ? ThemeHotBackgroundColor() : ThemeDisabledEdgeColor());
        TbFillRounded(hdc, done, (float)DpiScale(hwnd, kTbNavRadius), bg);
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, StrL("Done"), done, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kTbFontUrl, FW_SEMIBOLD));
    }
}

LRESULT TbFavMgrWnd::WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_COMMAND && (HWND)lp == hwndEdit && HIWORD(wp) == EN_KILLFOCUS) {
        // clicking anywhere else in the dialog commits, the way an in-place
        // rename does everywhere else
        CommitRename();
        return 0;
    }
    if (msg == WM_CTLCOLOREDIT && (HWND)lp == hwndEdit) {
        HDC dc = (HDC)wp;
        COLORREF bg = ThemeTextFieldColor();
        if (!editBrush || editBrushColor != bg) {
            if (editBrush) {
                DeleteObject(editBrush);
            }
            editBrush = CreateSolidBrush(bg);
            editBrushColor = bg;
        }
        SetBkColor(dc, bg);
        SetTextColor(dc, ThemeWindowTextColor());
        return (LRESULT)editBrush;
    }
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int next = std::clamp(scrollY - delta / 2, 0, MaxScrollY());
        if (next != scrollY) {
            // the EDIT is parked at absolute coordinates, so it would be left
            // floating over the wrong row
            CommitRename();
            scrollY = next;
            HwndInvalidate(hw, false);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (DoneRect().Contains(pt)) {
            donePressed = true;
            SetCapture(hw);
            HwndInvalidate(hw, false);
            return 0;
        }
        int row = -1;
        TbFavPart part = PartAt(pt, &row);
        pressedIdx = row;
        pressedPart = part;
        pressPt = pt;
        dragging = false;
        dragTo = row;
        if (row >= 0) {
            SetCapture(hw);
        }
        HwndInvalidate(hw, false);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (donePressed) {
            return 0;
        }
        if (pressedIdx >= 0 && GetCapture() == hw) {
            // only a press on the row body starts a drag, and only after the
            // slop, so a tap on a button is never a reorder
            if (!dragging && pressedPart == TbFavPart::Row && std::abs(pt.y - pressPt.y) > DpiScale(hw, kTbDragSlop)) {
                dragging = true;
            }
            if (dragging) {
                CommitRename();
                int n = TbFavCount();
                Rect list = ListRect();
                int padY = DpiScale(hw, kTbFavMgrListPadY);
                int idx = (pt.y - list.y - padY + scrollY) / std::max(1, RowDy());
                dragTo = std::clamp(idx, 0, std::max(0, n - 1));
                HwndInvalidate(hw, false);
            }
            return 0;
        }
        int row = -1;
        TbFavPart part = PartAt(pt, &row);
        bool done = DoneRect().Contains(pt);
        if (row != hotIdx || part != hotPart || done != doneHot) {
            hotIdx = row;
            hotPart = part;
            doneHot = done;
            HwndInvalidate(hw, false);
        }
        if (!tracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hw;
            TrackMouseEvent(&tme);
            tracking = true;
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        tracking = false;
        if (hotIdx != -1 || doneHot) {
            hotIdx = -1;
            hotPart = TbFavPart::None;
            doneHot = false;
            HwndInvalidate(hw, false);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool wasDone = donePressed;
        int idx = pressedIdx;
        TbFavPart part = pressedPart;
        bool wasDrag = dragging;
        int to = dragTo;
        donePressed = false;
        pressedIdx = -1;
        pressedPart = TbFavPart::None;
        dragging = false;
        dragTo = -1;
        if (GetCapture() == hw) {
            ReleaseCapture();
        }
        if (wasDone) {
            if (DoneRect().Contains(pt)) {
                TbHideFavMgr(tb);
                return 0;
            }
            HwndInvalidate(hw, false);
            return 0;
        }
        if (idx < 0) {
            return 0;
        }
        if (wasDrag) {
            if (to >= 0 && to != idx) {
                TbFavMove(tb, idx, to);
            }
        } else if (part != TbFavPart::Row && part != TbFavPart::None) {
            // only a press AND release on the same button fires it
            if (!BtnRect(idx, part).Contains(pt)) {
                HwndInvalidate(hw, true);
                return 0;
            }
            int n = TbFavCount();
            if (part == TbFavPart::Rename) {
                BeginRename(idx);
                return 0;
            }
            // any reorder / delete moves rows out from under an open EDIT
            CommitRename();
            if (part == TbFavPart::Delete) {
                TbFavDelete(tb, idx);
            } else if (part == TbFavPart::Up && idx > 0) {
                TbFavMove(tb, idx, idx - 1);
            } else if (part == TbFavPart::Down && idx < n - 1) {
                TbFavMove(tb, idx, idx + 1);
            }
            scrollY = std::clamp(scrollY, 0, MaxScrollY());
        }
        HwndInvalidate(hw, true);
        return 0;
    }
    if (msg == WM_CAPTURECHANGED) {
        pressedIdx = -1;
        pressedPart = TbFavPart::None;
        donePressed = false;
        dragging = false;
        dragTo = -1;
        HwndInvalidate(hw, false);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        TbHideFavMgr(tb);
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        // the scrim is WS_EX_NOACTIVATE, so anything taking activation from us
        // is really outside the modal and the dialog should go away
        TbHideFavMgr(tb);
        return 0;
    }
    return WndProcDefault(hw, msg, wp, lp);
}

static void TbShowFavMgr(TouchBrowser* tb) {
    if (!tb->scrim) {
        auto* s = new TbScrimWnd();
        if (s->Create(tb)) {
            tb->scrim = s;
        } else {
            delete s;
        }
    }
    if (!tb->favMgr) {
        auto* w = new TbFavMgrWnd();
        if (!w->Create(tb)) {
            delete w;
            return;
        }
        tb->favMgr = w;
    }
    if (tb->scrim) {
        tb->scrim->ShowOverFrame();
    }
    tb->favMgr->ShowAt();
}

static void TbHideFavMgr(TouchBrowser* tb) {
    if (tb->favMgr) {
        tb->favMgr->Hide();
    }
    if (tb->scrim) {
        tb->scrim->Hide();
    }
}

// --- the "..." menu --------------------------------------------------------

constexpr int kTbCmdNone = 0;
constexpr int kTbCmdManageFavs = 1;
constexpr int kTbCmdSetHome = 2;
constexpr int kTbCmdCleanup = 3;
constexpr int kTbCmdInfo = 4;

struct TbMenuItemDef {
    const char* label; // nullptr = separator
    int cmd;
};

static const TbMenuItemDef gTbMenuItems[] = {
    {"Manage favorites...", kTbCmdManageFavs},
    {"Set this page as home page", kTbCmdSetHome},
    {nullptr, kTbCmdNone},
    {"Clean up downloaded files...", kTbCmdCleanup},
    {"Where do downloads go?", kTbCmdInfo},
};
constexpr int kTbMenuItemCount = (int)dimof(gTbMenuItems);

// A menu command may put up a modal MsgBox, and the fav manager takes over the
// activation the closing menu just gave back, so commands run from the message
// loop rather than from inside the menu's WndProc. The browser is re-found via
// the window in case it went away in between.
struct TbMenuCmdReq {
    MainWindow* win = nullptr;
    int cmd = kTbCmdNone;
};

static void TbRunMenuCmd(TbMenuCmdReq* req) {
    MainWindow* win = req->win;
    int cmd = req->cmd;
    delete req;
    if (!IsMainWindowValid(win) || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    switch (cmd) {
        case kTbCmdManageFavs:
            TbShowFavMgr(tb);
            break;
        case kTbCmdSetHome:
            TbSetHomePage(tb);
            break;
        case kTbCmdCleanup:
            TbCleanupDownloads(tb);
            break;
        case kTbCmdInfo:
            TbOnInfo(tb);
            break;
        default:
            break;
    }
}

struct TbMenuWnd : Wnd {
    TbMenuWnd();
    bool Create(TouchBrowser*);
    void ShowAt();
    void Hide();
    void OnPaint(HDC, PAINTSTRUCT*) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    Rect ItemRect(int idx) const;
    int ItemAt(Point pt) const;
    int TotalDy() const;
    int WantedDx() const;

    TouchBrowser* tb = nullptr;
    int hotIdx = -1;
    int pressedIdx = -1;
    bool tracking = false;
};

TbMenuWnd::TbMenuWnd() {
    kind = kindTbMenu;
}

bool TbMenuWnd::Create(TouchBrowser* browser) {
    tb = browser;
    CreateCustomArgs args;
    args.className = L"SumatraTouchBrowserMenu";
    args.visible = false;
    args.style = WS_POPUP;
    args.exStyle = WS_EX_TOOLWINDOW;
    args.pos = {0, 0, 10, 10};
    args.bgColor = ThemeWindowControlBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)tb->win->hwndFrame);
    TbAddDropShadow(hwnd);
    return true;
}

Rect TbMenuWnd::ItemRect(int idx) const {
    int pad = DpiScale(hwnd, kTbMenuPad);
    int rowDy = DpiScale(hwnd, kTbMenuItemDy);
    int sepDy = DpiScale(hwnd, kTbMenuSepDy);
    int dx = HwndClientRect(hwnd).dx;
    int y = pad;
    for (int i = 0; i < idx; i++) {
        y += gTbMenuItems[i].label ? rowDy : sepDy;
    }
    int dy = gTbMenuItems[idx].label ? rowDy : sepDy;
    return {pad, y, std::max(0, dx - 2 * pad), dy};
}

int TbMenuWnd::TotalDy() const {
    int pad = DpiScale(hwnd, kTbMenuPad);
    Rect last = ItemRect(kTbMenuItemCount - 1);
    return last.y + last.dy + pad;
}

// min-width 180 per the design, but our menu carries longer labels than the
// mock, so grow to fit them (up to kTbMenuDx) instead of ellipsizing
int TbMenuWnd::WantedDx() const {
    HDC hdc = GetDC(hwnd);
    HFONT font = HdcGetUiFont(hdc, kTbFontMenu, FW_MEDIUM);
    int textDx = 0;
    for (int i = 0; i < kTbMenuItemCount; i++) {
        if (!gTbMenuItems[i].label) {
            continue;
        }
        Size sz = HdcMeasureText(hdc, Str(gTbMenuItems[i].label), DT_SINGLELINE | DT_NOPREFIX, font);
        textDx = std::max(textDx, sz.dx);
    }
    ReleaseDC(hwnd, hdc);
    int want = textDx + 2 * DpiScale(hwnd, kTbMenuPad) + 2 * DpiScale(hwnd, kTbMenuItemPadX);
    return std::clamp(want, DpiScale(hwnd, kTbMenuMinDx), DpiScale(hwnd, kTbMenuDx));
}

int TbMenuWnd::ItemAt(Point pt) const {
    for (int i = 0; i < kTbMenuItemCount; i++) {
        if (gTbMenuItems[i].label && ItemRect(i).Contains(pt)) {
            return i;
        }
    }
    return -1;
}

void TbMenuWnd::Hide() {
    hotIdx = -1;
    pressedIdx = -1;
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void TbMenuWnd::ShowAt() {
    hotIdx = -1;
    pressedIdx = -1;
    int dx = WantedDx();
    // TotalDy reads the client width only for the item rects' dx, so it is safe
    // to compute before the window has its final size
    int dy = TotalDy();
    // top-aligned under the button, right-aligned to it
    Rect ar = TbMenuAnchorScreenRect(tb);
    int x = ar.x + ar.dx - dx;
    int y = ar.y + ar.dy + DpiScale(hwnd, 4);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromPoint(POINT{ar.x, ar.y}, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &mi);
    Rect work = ToRect(mi.rcWork);
    x = std::clamp(x, work.x, std::max(work.x, work.x + work.dx - dx));
    if (y + dy > work.y + work.dy) {
        y = std::max(work.y, ar.y - dy - DpiScale(hwnd, 4));
    }
    SetWindowPos(hwnd, HWND_TOP, x, y, dx, dy, SWP_SHOWWINDOW);
    TbSetRoundedRegion(hwnd, dx, dy, DpiScale(hwnd, kTbMenuRadius));
    SetForegroundWindow(hwnd);
    HwndInvalidate(hwnd, true);
}

void TbMenuWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    Rect rc = HwndClientRect(hwnd);
    HdcFillRect(hdc, ToRect(ps->rcPaint), ThemeWindowControlBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);
    {
        // 1px border just inside the rounded region
        Rect border{0, 0, rc.dx, rc.dy};
        TbFillRounded(hdc, border, (float)DpiScale(hwnd, kTbMenuRadius), kColorUnset, ThemeEdgeColor());
    }
    float radius = (float)DpiScale(hwnd, kTbMenuItemRadius);
    int itemPadX = DpiScale(hwnd, kTbMenuItemPadX);
    for (int i = 0; i < kTbMenuItemCount; i++) {
        Rect r = ItemRect(i);
        if (!gTbMenuItems[i].label) {
            int y = r.y + r.dy / 2;
            HdcFillRect(hdc, Rect{r.x + DpiScale(hwnd, 8), y, std::max(0, r.dx - DpiScale(hwnd, 16)), 1},
                        ThemeEdgeColor());
            continue;
        }
        if (i == hotIdx || i == pressedIdx) {
            TbFillRounded(hdc, r, radius, ThemeHotBackgroundColor());
        }
        Rect text = r;
        text.x += itemPadX;
        text.dx = std::max(0, text.dx - 2 * itemPadX);
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, Str(gTbMenuItems[i].label), text,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kTbFontMenu, FW_MEDIUM));
    }
}

LRESULT TbMenuWnd::WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = ItemAt(pt);
        if (idx != hotIdx) {
            hotIdx = idx;
            HwndInvalidate(hw, false);
        }
        if (!tracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hw;
            TrackMouseEvent(&tme);
            tracking = true;
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        tracking = false;
        if (hotIdx != -1) {
            hotIdx = -1;
            HwndInvalidate(hw, false);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        pressedIdx = ItemAt(Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        HwndInvalidate(hw, false);
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        int idx = ItemAt(Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        int pressed = pressedIdx;
        pressedIdx = -1;
        // a tap that started on a different item (or outside) doesn't fire
        if (idx < 0 || (pressed >= 0 && pressed != idx)) {
            HwndInvalidate(hw, false);
            return 0;
        }
        Hide();
        auto* req = new TbMenuCmdReq();
        req->win = tb->win;
        req->cmd = gTbMenuItems[idx].cmd;
        uitask::Post(MkFunc0<TbMenuCmdReq>(TbRunMenuCmd, req), "TbRunMenuCmd");
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        Hide();
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        Hide();
        return 0;
    }
    return WndProcDefault(hw, msg, wp, lp);
}

// the "..." overflow button
static void TbShowMenu(TouchBrowser* tb) {
    if (!tb->menuWnd) {
        auto* w = new TbMenuWnd();
        if (!w->Create(tb)) {
            delete w;
            return;
        }
        tb->menuWnd = w;
    }
    tb->menuWnd->ShowAt();
}

// --- the browser chrome ----------------------------------------------------
// One owner-drawn child of hwndFrame paints all three rows and hit-tests them
// itself. System Buttons can't be made to look like the design's pills (the
// owner-draw path has a single app-wide palette), and N buttons also meant N
// windows to raise above WebView2 on every switch.

// the strip is narrow, so prefer the page title and fall back to the host name
static TempStr TbTabLabel(TbTab* t) {
    if (!str::IsEmptyOrWhiteSpace(t->title)) {
        return str::DupTemp(t->title);
    }
    if (t->url) {
        TempStr host = TbChipLabel(t->url);
        if (!str::IsEmptyOrWhiteSpace(host)) {
            return host;
        }
    }
    return str::DupTemp(StrL("New Tab"));
}

enum class TbPart {
    None,
    Tab,
    TabClose,
    NewTab,
    Fav,
    Back,
    Fwd,
    Home,
    OpenDoc,
    FavBtn,
    Info,
    Menu
};

// what the load progress bar is doing; Idle means it isn't on screen and
// its timer is stopped
enum class TbProgState {
    Idle,
    Loading,
    Finishing,
    Fading
};

struct TbHit {
    TbPart part = TbPart::None;
    int idx = -1;
};

static bool TbSameHit(const TbHit& a, const TbHit& b) {
    return a.part == b.part && a.idx == b.idx;
}

struct TbChromeWnd : Wnd {
    TbChromeWnd();
    ~TbChromeWnd() override;
    bool Create(TouchBrowser*);
    void OnPaint(HDC, PAINTSTRUCT*) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    void Layout(HDC hdc);
    void EnsureLayout();
    void Draw(HDC hdc);
    TbHit HitTest(Point pt) const;
    void Invoke(const TbHit&);
    void SyncUrlText();
    void UpdateTooltip(const TbHit&);

    Rect ProgressRect() const;
    void InvalidateProgress();
    void DrawProgress(HDC hdc);
    void ProgressStart();
    void ProgressFinish();
    void ProgressReset();
    void SyncProgressToActiveTab();
    void OnProgTick();

    TouchBrowser* tb = nullptr;
    HWND hwndUrl = nullptr;
    HFONT urlFont = nullptr;
    HBRUSH urlBrush = nullptr;
    COLORREF urlBrushColor = kColorUnset;
    Tooltip* tooltip = nullptr;
    bool tooltipUp = false;

    // layout, in client coords (already offset by the rows' scroll)
    Rect tabsRow, favRow, navRow;
    Vec<Rect> tabRects;
    Vec<Rect> tabCloseRects;
    Vec<Rect> favRects;
    Rect newTabRect, backRect, fwdRect, homeRect, urlRect, favBtnRect, infoRect, menuRect;
    // only laid out (non-empty) while the active tab is showing a document
    Rect openDocRect;
    Rect editRect;
    int tabScrollX = 0;
    int tabContentDx = 0;
    int favScrollX = 0;
    int favContentDx = 0;

    TbHit hot;
    TbHit pressed;

    AnimTimer progTimer;
    AnimVal progVal;  // 0..1 of the bar's width
    AnimVal progFade; // 1 while loading, eases to 0 after the bar hits 100%
    TbProgState progState = TbProgState::Idle;

    // --- ElaborateAnimations: hover cross-fade + press sink ---------------
    // Two hover values, not one: when the pointer moves from one button
    // straight to the next, the button being left has to fade out while the
    // new one fades in. A single value would make the old one snap.
    AnimTimer hoverTimer;
    TbHit animHot;     // the button hotIn applies to
    AnimVal hotIn;     // 0 -> 1 as the pointer arrives
    TbHit animPrevHot; // the button hotOut applies to
    AnimVal hotOut;    // 1 -> 0 as the pointer leaves
    TbHit animPressed;
    AnimVal pressVal;

    void SetHotAnimated(const TbHit& next);
    void SetPressedAnimated(const TbHit& next);
    void OnHoverTick();
    // 0..1 hover / press weight for one part, whatever the animation state
    float HoverAmount(TbPart part, int idx) const;
    float PressAmount(TbPart part, int idx) const;
};

// Enter in the URL field navigates (prefixing https:// when no scheme is typed)
static LRESULT CALLBACK TbUrlEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* tb = (TouchBrowser*)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        TempStr txt = HwndGetTextTemp(hwnd);
        WebviewWnd* wv = tb ? TbActiveWebView(tb) : nullptr;
        if (wv && !str::IsEmptyOrWhiteSpace(txt)) {
            Str url = txt;
            if (!str::StartsWithI(url, StrL("http://")) && !str::StartsWithI(url, StrL("https://"))) {
                url = str::JoinTemp(StrL("https://"), txt);
            }
            wv->Navigate(url);
        }
        return 0;
    }
    // A plain Win32 EDIT has no built-in Ctrl+A, and the frame's accelerators
    // would otherwise swallow it, so select-all has to be implemented here.
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    // ...and Ctrl+A still reaches the edit as a control character, which the
    // default handler would insert as a literal glyph
    if (msg == WM_CHAR && wp == 1) {
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

TbChromeWnd::TbChromeWnd() {
    kind = kindTbChrome;
}

TbChromeWnd::~TbChromeWnd() {
    // before ~Wnd tears the window down, so no timer outlives the object
    progTimer.Stop();
    hoverTimer.Stop();
    delete tooltip;
    if (urlBrush) {
        DeleteObject(urlBrush);
    }
    if (hwndUrl) {
        DestroyWindow(hwndUrl);
        hwndUrl = nullptr;
    }
}

bool TbChromeWnd::Create(TouchBrowser* browser) {
    tb = browser;
    CreateCustomArgs args;
    args.className = L"SumatraTouchBrowserChrome";
    args.parent = tb->win->hwndFrame;
    args.style = WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    args.visible = false;
    args.pos = {0, 0, 10, 10};
    args.bgColor = ThemeWindowControlBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    // The URL field stays a real EDIT (typing, Ctrl+A, Enter, the caret and
    // the IME all come for free); only its surround is drawn by us, so it has
    // no border of its own and lives inside the rounded rect we paint.
    HINSTANCE inst = GetInstance();
    hwndUrl = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, nullptr, inst,
                              nullptr);
    SendMessageW(hwndUrl, WM_SETFONT, (WPARAM)tb->hFont, TRUE);
    SetWindowSubclass(hwndUrl, TbUrlEditProc, NextSubclassId(), (DWORD_PTR)tb);
    progTimer.Init(hwnd, kTbProgTimerId);
    hoverTimer.Init(hwnd, kTbHoverTimerId);
    return true;
}

// --- load progress bar -----------------------------------------------------
// The strip is the bottom kTbProgDy pixels of the chrome, i.e. the bottom edge
// of the nav row. It is deliberately independent of Layout(): a progress frame
// must not have to re-measure every tab and favorite label.
Rect TbChromeWnd::ProgressRect() const {
    Rect rc = HwndClientRect(hwnd);
    int dy = std::min(DpiScale(hwnd, kTbProgDy), rc.dy);
    if (rc.dx <= 0 || dy <= 0) {
        return {};
    }
    return {0, rc.dy - dy, rc.dx, dy};
}

void TbChromeWnd::InvalidateProgress() {
    Rect r = ProgressRect();
    if (!r.IsEmpty()) {
        HwndInvalidateRect(hwnd, r, false);
    }
}

void TbChromeWnd::DrawProgress(HDC hdc) {
    Rect strip = ProgressRect();
    if (strip.IsEmpty()) {
        return;
    }
    // repaint what is underneath first: the chrome's panel plus the nav row's
    // 1px bottom border, which the strip covers
    HdcFillRect(hdc, strip, ThemeWindowControlBackgroundColor());
    HdcFillRect(hdc, Rect{strip.x, strip.y + strip.dy - 1, strip.dx, 1}, ThemeEdgeColor());
    if (progState == TbProgState::Idle) {
        return;
    }
    float f = limitValue(progVal.Value(), 0.0f, 1.0f);
    int dx = (int)((float)strip.dx * f + 0.5f);
    if (dx <= 0) {
        return;
    }
    // no alpha blending on a GDI fill, so the fade is a colour lerp back to the
    // panel the bar sits on - visually the same at this thickness
    float a = limitValue(progFade.Value(), 0.0f, 1.0f);
    COLORREF col = AnimLerpColor(ThemeWindowControlBackgroundColor(), ThemeWindowLinkColor(), a);
    HdcFillRect(hdc, Rect{strip.x, strip.y, dx, strip.dy}, col);
}

void TbChromeWnd::ProgressStart() {
    // the bar IS an animation; with animations off there is nothing to show
    if (!AnimEnabled()) {
        ProgressReset();
        return;
    }
    if (progState == TbProgState::Loading) {
        return; // a redirect in the same navigation: keep creeping, don't restart
    }
    progState = TbProgState::Loading;
    progVal.Set(0.0f);
    progVal.SetTarget(kTbProgCreepTo, kTbProgCreepMs);
    progFade.Set(1.0f);
    progTimer.Start();
    InvalidateProgress();
}

void TbChromeWnd::ProgressFinish() {
    if (progState == TbProgState::Idle || progState == TbProgState::Fading) {
        return;
    }
    progState = TbProgState::Finishing;
    progVal.SetTarget(1.0f, kTbProgFinishMs);
    progTimer.Start();
    InvalidateProgress();
}

void TbChromeWnd::ProgressReset() {
    progTimer.Stop();
    if (progState == TbProgState::Idle) {
        return;
    }
    progState = TbProgState::Idle;
    progVal.Set(0.0f);
    progFade.Set(0.0f);
    InvalidateProgress();
}

// the bar shows the ACTIVE tab only, so switching tabs adopts that tab's state
void TbChromeWnd::SyncProgressToActiveTab() {
    TbTab* act = TbActiveTab(tb);
    if (act && act->loading) {
        ProgressStart();
    } else {
        ProgressReset();
    }
}

// One frame. Invalidates only the 3px strip and stops the timer the moment
// nothing is moving - including while a slow page is still loading, where the
// bar simply rests at kTbProgCreepTo until the navigation ends.
// Hover and press are two stops on one colour path (rest -> hover -> pressed),
// the same idiom the rail and top bar use, so the whole chrome feels of a piece.
float TbChromeWnd::HoverAmount(TbPart part, int idx) const {
    if (!AnimElaborate()) {
        return (hot.part == part && hot.idx == idx) ? 1.0f : 0.0f;
    }
    if (animHot.part == part && animHot.idx == idx) {
        return hotIn.Value();
    }
    if (animPrevHot.part == part && animPrevHot.idx == idx) {
        return hotOut.Value();
    }
    // hot but never animated (e.g. the setting was switched on mid-hover)
    return (hot.part == part && hot.idx == idx) ? 1.0f : 0.0f;
}

float TbChromeWnd::PressAmount(TbPart part, int idx) const {
    bool isPressed = pressed.part == part && pressed.idx == idx;
    if (!AnimElaborate()) {
        return isPressed ? 1.0f : 0.0f;
    }
    if (animPressed.part == part && animPressed.idx == idx) {
        return pressVal.Value();
    }
    return isPressed ? 1.0f : 0.0f;
}

void TbChromeWnd::SetHotAnimated(const TbHit& next) {
    if (TbSameHit(hot, next)) {
        return;
    }
    if (AnimElaborate()) {
        // the button being left fades out from wherever it currently is, so
        // sweeping across a row leaves a trail rather than a series of snaps
        animPrevHot = hot;
        hotOut.Set(HoverAmount(hot.part, hot.idx));
        hotOut.SetTarget(0.0f, kAnimHoverMs);
        animHot = next;
        hotIn.Set(HoverAmount(next.part, next.idx));
        hotIn.SetTarget(next.part == TbPart::None ? 0.0f : 1.0f, kAnimHoverMs);
        hoverTimer.Start();
    }
    hot = next;
    UpdateTooltip(hot);
    HwndInvalidate(hwnd, false);
}

void TbChromeWnd::SetPressedAnimated(const TbHit& next) {
    if (TbSameHit(pressed, next)) {
        return;
    }
    if (AnimElaborate()) {
        bool down = next.part != TbPart::None;
        animPressed = down ? next : pressed;
        pressVal.Set(PressAmount(animPressed.part, animPressed.idx));
        pressVal.SetTarget(down ? 1.0f : 0.0f, down ? kAnimPressMs : kAnimPressReleaseMs);
        hoverTimer.Start();
    }
    pressed = next;
    HwndInvalidate(hwnd, false);
}

void TbChromeWnd::OnHoverTick() {
    bool moving = hotIn.IsAnimating() || hotOut.IsAnimating() || pressVal.IsAnimating();
    HwndInvalidate(hwnd, false);
    if (!moving) {
        hoverTimer.Stop();
    }
}

void TbChromeWnd::OnProgTick() {
    bool running = false;
    switch (progState) {
        case TbProgState::Loading:
            running = progVal.IsAnimating();
            break;
        case TbProgState::Finishing:
            running = progVal.IsAnimating();
            if (!running) {
                progState = TbProgState::Fading;
                progFade.SetTarget(0.0f, kTbProgFadeMs);
                running = progFade.IsAnimating();
            }
            break;
        case TbProgState::Fading:
            running = progFade.IsAnimating();
            if (!running) {
                ProgressReset();
                return;
            }
            break;
        default:
            break;
    }
    InvalidateProgress();
    if (!running) {
        progTimer.Stop();
    }
}

// how tall the chrome wants to be: the favorites bar only exists when there is
// at least one favorite
static int TbChromeDy(HWND hwnd) {
    int dy = DpiScale(hwnd, kTbTabsRowDy) + DpiScale(hwnd, kTbNavRowDy);
    if (TbFavCount() > 0) {
        dy += DpiScale(hwnd, kTbFavRowDy);
    }
    return dy;
}

void TbChromeWnd::SyncUrlText() {
    if (!hwndUrl) {
        return;
    }
    TbTab* t = TbActiveTab(tb);
    Str url = t ? t->url : Str();
    HwndSetText(hwndUrl, url ? url : StrL(""));
}

void TbChromeWnd::Layout(HDC hdc) {
    Rect rc = HwndClientRect(hwnd);
    tabRects.Reset();
    tabCloseRects.Reset();
    favRects.Reset();
    newTabRect = {};
    backRect = fwdRect = homeRect = urlRect = favBtnRect = infoRect = menuRect = {};
    openDocRect = {};
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }

    int tabsDy = DpiScale(hwnd, kTbTabsRowDy);
    int navDy = DpiScale(hwnd, kTbNavRowDy);
    bool hasFavs = TbFavCount() > 0;
    int favDy = hasFavs ? DpiScale(hwnd, kTbFavRowDy) : 0;
    tabsRow = {0, 0, rc.dx, tabsDy};
    favRow = {0, tabsDy, rc.dx, favDy};
    navRow = {0, tabsDy + favDy, rc.dx, navDy};

    HFONT fontBtn = HdcGetUiFont(hdc, kTbFontBtn, FW_MEDIUM);
    HFONT fontFav = HdcGetUiFont(hdc, kTbFontFav, FW_MEDIUM);

    // --- tab strip: bottom-aligned, horizontally scrollable
    {
        int padX = DpiScale(hwnd, kTbTabsPadX);
        int gap = DpiScale(hwnd, kTbTabsGap);
        int tabDy = DpiScale(hwnd, kTbTabDy);
        int minDx = DpiScale(hwnd, kTbTabMinDx);
        int maxDx = DpiScale(hwnd, kTbTabMaxDx);
        int newDx = DpiScale(hwnd, kTbNewTabDx);
        int closeDx = DpiScale(hwnd, kTbTabCloseDx);
        int padRight = DpiScale(hwnd, kTbTabPadRight);
        // the row's 1px bottom border is the line the tabs sit on; the active
        // tab is drawn one pixel taller so it merges with the row below
        int tabTop = tabsRow.dy - 1 - tabDy;
        int nTabs = len(tb->tabs);
        int avail = std::max(0, rc.dx - 2 * padX);
        int tabDx = maxDx;
        if (nTabs > 0) {
            int perTab = (avail - newDx - nTabs * gap) / nTabs;
            tabDx = std::clamp(perTab, minDx, maxDx);
        }
        tabContentDx = nTabs * (tabDx + gap) + newDx;
        int maxScroll = std::max(0, tabContentDx - avail);
        tabScrollX = std::clamp(tabScrollX, 0, maxScroll);
        int x = padX - tabScrollX;
        for (int i = 0; i < nTabs; i++) {
            Rect r{x, tabTop, tabDx, tabDy};
            tabRects.Append(r);
            Rect close{r.x + r.dx - padRight - closeDx, r.y + (r.dy - closeDx) / 2, closeDx, closeDx};
            tabCloseRects.Append(close);
            x += tabDx + gap;
        }
        newTabRect = {x, tabsRow.dy - 1 - newDx, newDx, newDx};
    }

    // --- favorites bar: plain accent-coloured text, no chrome
    if (hasFavs) {
        int padX = DpiScale(hwnd, kTbFavPadX);
        int gap = DpiScale(hwnd, kTbFavGap);
        int n = TbFavCount();
        int x = padX - favScrollX;
        int total = padX;
        for (int i = 0; i < n; i++) {
            TempStr label = TbFavLabel(i);
            Size sz = HdcMeasureText(hdc, label, DT_SINGLELINE | DT_NOPREFIX, fontFav);
            favRects.Append(Rect{x, favRow.y, sz.dx, favRow.dy});
            x += sz.dx + gap;
            total += sz.dx + gap;
        }
        favContentDx = total;
        int maxScroll = std::max(0, favContentDx - rc.dx);
        int clamped = std::clamp(favScrollX, 0, maxScroll);
        if (clamped != favScrollX) {
            int delta = favScrollX - clamped;
            favScrollX = clamped;
            for (Rect& r : favRects) {
                r.x += delta;
            }
        }
    } else {
        favContentDx = 0;
        favScrollX = 0;
    }

    // --- nav row
    {
        int padX = DpiScale(hwnd, kTbNavPadX);
        int gap = DpiScale(hwnd, kTbNavGap);
        int btnDy = DpiScale(hwnd, kTbNavBtnDy);
        int btnPadX = DpiScale(hwnd, kTbNavBtnPadX);
        int iconDx = DpiScale(hwnd, kTbIconBtnDx);
        int urlDy = DpiScale(hwnd, kTbUrlDy);
        int btnY = navRow.y + (navRow.dy - btnDy) / 2;
        auto textBtn = [&](Str s, int x) {
            Size sz = HdcMeasureText(hdc, s, DT_SINGLELINE | DT_NOPREFIX, fontBtn);
            return Rect{x, btnY, sz.dx + 2 * btnPadX, btnDy};
        };
        int x = padX;
        backRect = textBtn(StrL("Back"), x);
        x += backRect.dx + gap;
        fwdRect = textBtn(StrL("Fwd"), x);
        x += fwdRect.dx + gap;
        homeRect = textBtn(StrL("Home"), x);
        x += homeRect.dx + gap;

        int right = rc.dx - padX;
        menuRect = {right - iconDx, btnY, iconDx, iconDx};
        infoRect = {menuRect.x - gap - iconDx, btnY, iconDx, iconDx};
        Size favSz = HdcMeasureText(hdc, StrL("Favorite"), DT_SINGLELINE | DT_NOPREFIX, fontBtn);
        int favBtnDx = favSz.dx + 2 * btnPadX;
        favBtnRect = {infoRect.x - gap - favBtnDx, btnY, favBtnDx, btnDy};

        // "Open in SumatraPDF" only exists while the page really is a document,
        // and takes its width out of the URL field
        int leftOfUrl = favBtnRect.x;
        TbTab* act = TbActiveTab(tb);
        if (act && act->isDocPage) {
            Size sz = HdcMeasureText(hdc, StrL(kTbOpenDocLabel), DT_SINGLELINE | DT_NOPREFIX, fontBtn);
            int dx = sz.dx + 2 * btnPadX;
            openDocRect = {favBtnRect.x - gap - dx, btnY, dx, btnDy};
            leftOfUrl = openDocRect.x;
        }

        int urlX = x;
        int urlDx = std::max(0, leftOfUrl - gap - urlX);
        urlRect = {urlX, navRow.y + (navRow.dy - urlDy) / 2, urlDx, urlDy};
    }

    // the EDIT itself sits inside the drawn URL pill
    if (hwndUrl) {
        HFONT wantFont = HdcGetUiFont(hdc, kTbFontUrl);
        if (wantFont != urlFont) {
            urlFont = wantFont;
            SendMessageW(hwndUrl, WM_SETFONT, (WPARAM)urlFont, TRUE);
        }
        int padX = DpiScale(hwnd, kTbUrlPadX);
        int editDy = FontDyPx(hwnd, urlFont);
        if (editDy <= 0 || editDy > urlRect.dy) {
            editDy = urlRect.dy;
        }
        Rect r{urlRect.x + padX, urlRect.y + (urlRect.dy - editDy) / 2, std::max(0, urlRect.dx - 2 * padX), editDy};
        // guard against re-entering WM_PAINT from a MoveWindow that changes nothing
        if (r != editRect) {
            editRect = r;
            MoveWindow(hwndUrl, r.x, r.y, r.dx, r.dy, TRUE);
        }
    }
}

void TbChromeWnd::EnsureLayout() {
    if (!hwnd) {
        return;
    }
    HDC hdc = GetDC(hwnd);
    Layout(hdc);
    ReleaseDC(hwnd, hdc);
}

void TbChromeWnd::Draw(HDC hdc) {
    Rect rc = HwndClientRect(hwnd);
    COLORREF panel = ThemeWindowControlBackgroundColor();
    COLORREF edge = ThemeEdgeColor();
    COLORREF hotBg = ThemeHotBackgroundColor();
    COLORREF text = ThemeWindowTextColor();
    COLORREF muted = ThemeWindowDarkerTextColor();
    COLORREF accent = ThemeWindowLinkColor();
    HdcFillRect(hdc, rc, panel);
    SetBkMode(hdc, TRANSPARENT);

    Layout(hdc);

    // rest -> hover -> pressed as one colour path. With ElaborateAnimations off
    // the weights are hard 0 or 1, which reproduces the original snap exactly.
    auto btnBg = [&](TbPart part, int idx) -> COLORREF {
        float h = HoverAmount(part, idx);
        float pr = PressAmount(part, idx);
        COLORREF c = hotBg;
        if (h > 0.0f) {
            c = AnimLerpColor(hotBg, ThemeDisabledEdgeColor(), h);
        }
        if (pr > 0.0f) {
            c = AnimLerpColor(c, ThemeEdgeColor(), pr);
        }
        return c;
    };

    // --- row 1: tab strip
    {
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, tabsRow.x, tabsRow.y, tabsRow.x + tabsRow.dx, tabsRow.y + tabsRow.dy);
        HdcFillRect(hdc, Rect{tabsRow.x, tabsRow.y + tabsRow.dy - 1, tabsRow.dx, 1}, edge);
        float radius = (float)DpiScale(hwnd, kTbTabRadius);
        int padLeft = DpiScale(hwnd, kTbTabPadLeft);
        int innerGap = DpiScale(hwnd, kTbTabInnerGap);
        HFONT fontActive = HdcGetUiFont(hdc, kTbFontTab, FW_SEMIBOLD);
        HFONT fontIdle = HdcGetUiFont(hdc, kTbFontTab);
        for (int i = 0; i < len(tabRects); i++) {
            Rect r = tabRects[i];
            bool isActive = (i == tb->activeTab);
            bool isHot = (hot.part == TbPart::Tab && hot.idx == i);
            Rect fillR = r;
            if (isActive) {
                // one pixel taller, so its body covers the row's bottom border
                // and it merges with the page below
                fillR.dy += 1;
            }
            COLORREF bg = isActive ? panel : (isHot ? ThemeTouchSurfaceColor() : hotBg);
            TbFillTab(hdc, fillR, radius, bg, edge);

            Rect close = tabCloseRects[i];
            Rect label{r.x + padLeft, r.y, std::max(0, close.x - innerGap - (r.x + padLeft)), r.dy};
            SetTextColor(hdc, text);
            HdcDrawText(hdc, TbTabLabel(tb->tabs[i]), label,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX,
                        isActive ? fontActive : fontIdle);
            bool closeHot = (hot.part == TbPart::TabClose && hot.idx == i);
            TbDrawCloseGlyph(hdc, hwnd, close, muted, closeHot ? ThemeEdgeColor() : kColorUnset);
        }
        bool newHot = (hot.part == TbPart::NewTab);
        if (newHot) {
            TbFillRounded(hdc, newTabRect, (float)DpiScale(hwnd, kTbTabRadius), hotBg);
        }
        TbDrawPlus(hdc, hwnd, newTabRect, muted);
        RestoreDC(hdc, saved);
    }

    // --- row 2: favorites bar (plain accent text, no pills)
    if (favRow.dy > 0) {
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, favRow.x, favRow.y, favRow.x + favRow.dx, favRow.y + favRow.dy);
        HdcFillRect(hdc, Rect{favRow.x, favRow.y + favRow.dy - 1, favRow.dx, 1}, edge);
        HFONT font = HdcGetUiFont(hdc, kTbFontFav, FW_MEDIUM);
        for (int i = 0; i < len(favRects); i++) {
            Rect r = favRects[i];
            bool isHot = (hot.part == TbPart::Fav && hot.idx == i);
            SetTextColor(hdc, accent);
            uint flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX;
            HdcDrawText(hdc, TbFavLabel(i), r, flags, font);
            if (isHot) {
                // hover: underline, the only affordance plain text can carry
                HdcFillRect(hdc, Rect{r.x, r.y + r.dy - DpiScale(hwnd, 9), r.dx, 1}, accent);
            }
        }
        RestoreDC(hdc, saved);
    }

    // --- row 3: nav row
    {
        HdcFillRect(hdc, Rect{navRow.x, navRow.y + navRow.dy - 1, navRow.dx, 1}, edge);
        float radius = (float)DpiScale(hwnd, kTbNavRadius);
        HFONT font = HdcGetUiFont(hdc, kTbFontBtn, FW_MEDIUM);
        uint flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX;

        TbFillRounded(hdc, backRect, radius, btnBg(TbPart::Back, -1));
        SetTextColor(hdc, tb->canGoBack ? text : ThemeWindowTextDisabledColor());
        HdcDrawText(hdc, StrL("Back"), backRect, flags, font);

        TbFillRounded(hdc, fwdRect, radius, btnBg(TbPart::Fwd, -1));
        SetTextColor(hdc, tb->canGoForward ? text : ThemeWindowTextDisabledColor());
        HdcDrawText(hdc, StrL("Fwd"), fwdRect, flags, font);

        TbFillRounded(hdc, homeRect, radius, btnBg(TbPart::Home, -1));
        SetTextColor(hdc, text);
        HdcDrawText(hdc, StrL("Home"), homeRect, flags, font);

        // the URL field's surround; the EDIT itself is a child window on top
        TbFillRounded(hdc, urlRect, radius, ThemeTextFieldColor(), edge);

        if (!openDocRect.IsEmpty()) {
            // accent surface, not the plain pill: this is the one control on the
            // row that the user is being invited to press
            COLORREF accentBg, accentFg;
            ThemeAccentSurfaceColors(&accentBg, &accentFg);
            bool odHot = (hot.part == TbPart::OpenDoc);
            bool odPressed = (pressed.part == TbPart::OpenDoc);
            COLORREF bg = odPressed ? ThemeEdgeColor() : (odHot ? ThemeHotEdgeColor() : accentBg);
            TbFillRounded(hdc, openDocRect, radius, bg, ThemeEdgeColor());
            SetTextColor(hdc, accentFg);
            HdcDrawText(hdc, StrL(kTbOpenDocLabel), openDocRect, flags, font);
        }

        TbFillRounded(hdc, favBtnRect, radius, btnBg(TbPart::FavBtn, -1));
        // the button is a toggle, so it says which way it is pointing by
        // colouring its label with the accent once the page is a favorite
        SetTextColor(hdc, TbCurrentIsFav(tb) ? accent : text);
        HdcDrawText(hdc, StrL("Favorite"), favBtnRect, flags, font);

        TbFillRounded(hdc, infoRect, radius, btnBg(TbPart::Info, -1));
        SetTextColor(hdc, muted);
        HdcDrawText(hdc, StrL("i"), infoRect, flags, HdcGetUiFont(hdc, kTbFontBtn, FW_SEMIBOLD));

        TbFillRounded(hdc, menuRect, radius, btnBg(TbPart::Menu, -1));
        TbDrawDots(hdc, hwnd, menuRect, muted);
    }

    // last, so it sits on top of the nav row's bottom border
    DrawProgress(hdc);
}

void TbChromeWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    Rect rc = HwndClientRect(hwnd);
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }
    // A progress frame only ever invalidates the 3px strip. Redrawing the whole
    // chrome (which re-measures every tab and favorite label) 60 times a second
    // for it would be absurd, so that case paints just the strip.
    Rect prog = ProgressRect();
    Rect paintClip = ToRect(ps->rcPaint);
    if (progState != TbProgState::Idle && !prog.IsEmpty() && paintClip.y >= prog.y) {
        DrawProgress(hdc);
        return;
    }
    // double-buffered: the whole chrome repaints on every hover change
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.dx, rc.dy);
    HGDIOBJ prev = SelectObject(memDc, bmp);
    Draw(memDc);
    Rect clip = ToRect(ps->rcPaint);
    BitBlt(hdc, clip.x, clip.y, clip.dx, clip.dy, memDc, clip.x, clip.y, SRCCOPY);
    SelectObject(memDc, prev);
    DeleteObject(bmp);
    DeleteDC(memDc);
}

TbHit TbChromeWnd::HitTest(Point pt) const {
    if (tabsRow.Contains(pt)) {
        for (int i = 0; i < len(tabCloseRects); i++) {
            if (tabCloseRects[i].Contains(pt)) {
                return {TbPart::TabClose, i};
            }
        }
        for (int i = 0; i < len(tabRects); i++) {
            if (tabRects[i].Contains(pt)) {
                return {TbPart::Tab, i};
            }
        }
        if (newTabRect.Contains(pt)) {
            return {TbPart::NewTab, -1};
        }
        return {};
    }
    if (favRow.dy > 0 && favRow.Contains(pt)) {
        for (int i = 0; i < len(favRects); i++) {
            if (favRects[i].Contains(pt)) {
                return {TbPart::Fav, i};
            }
        }
        return {};
    }
    if (backRect.Contains(pt)) {
        return {TbPart::Back, -1};
    }
    if (fwdRect.Contains(pt)) {
        return {TbPart::Fwd, -1};
    }
    if (homeRect.Contains(pt)) {
        return {TbPart::Home, -1};
    }
    if (!openDocRect.IsEmpty() && openDocRect.Contains(pt)) {
        return {TbPart::OpenDoc, -1};
    }
    if (favBtnRect.Contains(pt)) {
        return {TbPart::FavBtn, -1};
    }
    if (infoRect.Contains(pt)) {
        return {TbPart::Info, -1};
    }
    if (menuRect.Contains(pt)) {
        return {TbPart::Menu, -1};
    }
    return {};
}

void TbChromeWnd::Invoke(const TbHit& h) {
    switch (h.part) {
        case TbPart::Tab:
            if (h.idx != tb->activeTab) {
                TbActivateTab(tb, h.idx);
            }
            break;
        case TbPart::TabClose:
            if (h.idx >= 0 && h.idx < len(tb->tabs)) {
                TbCloseTab(tb->tabs[h.idx]);
            }
            break;
        case TbPart::NewTab:
            TbOnNewTab(tb);
            break;
        case TbPart::Fav:
            TbOnFavClick(tb, h.idx);
            break;
        case TbPart::Back:
            TbOnBack(tb);
            break;
        case TbPart::Fwd:
            TbOnForward(tb);
            break;
        case TbPart::Home:
            TbOnHome(tb);
            break;
        case TbPart::OpenDoc:
            TbOpenPageAsDoc(tb);
            break;
        case TbPart::FavBtn:
            TouchWebToggleBookmark(tb->win);
            break;
        case TbPart::Info:
            TbOnInfo(tb);
            break;
        case TbPart::Menu:
            TbShowMenu(tb);
            break;
        default:
            break;
    }
}

void TbChromeWnd::UpdateTooltip(const TbHit& h) {
    bool want = (h.part == TbPart::Info);
    if (want == tooltipUp) {
        return;
    }
    tooltipUp = want;
    if (!want) {
        if (tooltip) {
            tooltip->Delete();
        }
        return;
    }
    if (!tooltip) {
        Tooltip::CreateArgs targs;
        targs.parent = hwnd;
        targs.font = tb->hFont;
        tooltip = new Tooltip();
        tooltip->Create(targs);
    }
    tooltip->SetSingle(StrL("Page info"), infoRect, false);
}

LRESULT TbChromeWnd::WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_TIMER && wp == kTbHoverTimerId) {
        OnHoverTick();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTbProgTimerId) {
        OnProgTick();
        return 0;
    }
    if (msg == WM_SIZE) {
        EnsureLayout();
        HwndInvalidate(hw, false);
        return 0;
    }
    if (msg == WM_CTLCOLOREDIT && (HWND)lp == hwndUrl) {
        HDC dc = (HDC)wp;
        COLORREF bg = ThemeTouchSurfaceColor();
        if (!urlBrush || urlBrushColor != bg) {
            if (urlBrush) {
                DeleteObject(urlBrush);
            }
            urlBrush = CreateSolidBrush(bg);
            urlBrushColor = bg;
        }
        SetBkColor(dc, bg);
        SetTextColor(dc, ThemeWindowTextColor());
        return (LRESULT)urlBrush;
    }
    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TbHit h = HitTest(pt);
        SetHotAnimated(h);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hw;
        TrackMouseEvent(&tme);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        if (hot.part != TbPart::None) {
            SetHotAnimated({});
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        // the tab strip and the favorites bar scroll horizontally
        POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hw, &sp);
        Point pt{sp.x, sp.y};
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = DpiScale(hw, 40) * delta / WHEEL_DELTA;
        if (tabsRow.Contains(pt)) {
            int maxScroll = std::max(0, tabContentDx - HwndClientRect(hw).dx);
            int next = std::clamp(tabScrollX - step, 0, maxScroll);
            if (next != tabScrollX) {
                tabScrollX = next;
                HwndInvalidate(hw, false);
            }
            return 0;
        }
        if (favRow.dy > 0 && favRow.Contains(pt)) {
            int maxScroll = std::max(0, favContentDx - HwndClientRect(hw).dx);
            int next = std::clamp(favScrollX - step, 0, maxScroll);
            if (next != favScrollX) {
                favScrollX = next;
                HwndInvalidate(hw, false);
            }
            return 0;
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TbHit down = HitTest(pt);
        SetPressedAnimated(down);
        if (down.part != TbPart::None) {
            SetCapture(hw);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TbHit was = pressed;
        SetPressedAnimated({});
        if (GetCapture() == hw) {
            ReleaseCapture();
        }
        if (was.part == TbPart::None) {
            return 0;
        }
        TbHit now = HitTest(pt);
        if (TbSameHit(was, now)) {
            Invoke(was);
        }
        return 0;
    }
    if (msg == WM_CAPTURECHANGED) {
        if (pressed.part != TbPart::None) {
            SetPressedAnimated({});
        }
        return 0;
    }
    return WndProcDefault(hw, msg, wp, lp);
}

static void TbRedrawChrome(TouchBrowser* tb) {
    if (tb && tb->chrome && tb->chrome->hwnd) {
        HwndInvalidate(tb->chrome->hwnd, false);
    }
}

static void TbRelayoutChrome(TouchBrowser* tb) {
    if (!tb || !tb->chrome || !tb->chrome->hwnd) {
        return;
    }
    tb->chrome->EnsureLayout();
    HwndInvalidate(tb->chrome->hwnd, false);
    if (tb->win && tb->win->touchView == TouchView::Web) {
        // the favorites bar appears / disappears with the favorites, which
        // changes how much room is left for the webview
        ScheduleUiUpdate(tb->win, kUiForceRelayout);
    }
}

static void TbSyncUrlBar(TouchBrowser* tb) {
    if (tb->chrome) {
        tb->chrome->SyncUrlText();
    }
}

static void TbSetTabLoading(TbTab* t, bool loading) {
    t->loading = loading;
    TbChromeWnd* chrome = t->tb ? t->tb->chrome : nullptr;
    if (!chrome || !TbIsActiveTab(t)) {
        return; // a background tab never writes to the nav row
    }
    if (loading) {
        chrome->ProgressStart();
    } else {
        chrome->ProgressFinish();
    }
}

static void TbClearDocPage(TbTab* t) {
    if (!t->isDocPage) {
        return;
    }
    t->isDocPage = false;
    str::FreePtr(&t->docPageUrl);
    str::FreePtr(&t->docPageExt);
    // the nav row loses a button, so the URL field grows back: re-measure
    TbRelayoutChrome(t->tb);
}

static Rect TbMenuAnchorScreenRect(TouchBrowser* tb) {
    if (!tb->chrome || !tb->chrome->hwnd) {
        return HwndWindowRect(tb->win->hwndFrame);
    }
    Rect r = tb->chrome->menuRect;
    POINT origin{0, 0};
    ClientToScreen(tb->chrome->hwnd, &origin);
    return {origin.x + r.x, origin.y + r.y, r.dx, r.dy};
}

// --- tabs ------------------------------------------------------------------

static void TbDestroyTab(TbTab* t) {
    delete t->webView;
    str::Free(t->url);
    str::Free(t->title);
    str::Free(t->pendingUrl);
    str::Free(t->docPageUrl);
    str::Free(t->docPageExt);
    str::Free(t->lastTakeoverUrl);
    delete t;
}

// Closing a tab tears down a WebView2 control from inside the chrome's own
// click handler, so the work is posted back to the message loop. The request
// identifies the browser by its window, so a browser torn down in the meantime
// is detected before `tab` (which would then be dangling) is ever dereferenced.
struct TbCloseTabReq {
    MainWindow* win = nullptr;
    TbTab* tab = nullptr;
};

static void TbCloseTabNow(TbCloseTabReq* req) {
    MainWindow* win = req->win;
    TbTab* tab = req->tab;
    delete req;
    if (!IsMainWindowValid(win) || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    int idx = tb->tabs.Find(tab);
    if (idx < 0) {
        return; // already closed
    }
    if (len(tb->tabs) == 1) {
        // never leave the browser without a live webview: the last tab is
        // recycled back to the home page instead of being closed
        if (tab->webView) {
            tab->webView->Navigate(TouchBrowserHomeUrl());
        }
        return;
    }
    tb->tabs.RemoveAt(idx);
    TbDestroyTab(tab);
    // closing a tab left of the active one shifts it down; closing the active
    // one hands over to the tab that slid into its slot (clamped for the last)
    int active = tb->activeTab;
    if (idx < active) {
        active--;
    }
    TbActivateTab(tb, active);
}

static void TbCloseTab(TbTab* t) {
    auto* req = new TbCloseTabReq();
    req->win = t->tb->win;
    req->tab = t;
    uitask::Post(MkFunc0<TbCloseTabReq>(TbCloseTabNow, req), "TbCloseTabNow");
}

// Creates a tab and its webview. Like SimpleBrowserWindow the control is created
// at a real size (not 0x0), and `url` is parked in pendingUrl because WebView2
// drops a Navigate issued before the control has bounds and a first layout;
// LayoutTouchWebView issues it. Returns nullptr when the strip is full.
static TbTab* TbCreateTab(TouchBrowser* tb, Str url) {
    if (len(tb->tabs) >= kTbMaxTabs) {
        return nullptr;
    }
    HWND frame = tb->win->hwndFrame;
    auto* t = new TbTab();
    t->tb = tb;
    t->pendingUrl = str::Dup(url);

    t->webView = new WebviewWnd();
    t->webView->dataDir = str::Dup(GetWebViewDataDirTemp());
    t->webView->enableAutofill = true;
    t->webView->enableBrowserChrome = true;
    t->webView->events.ctx = t;
    t->webView->events.navigationStarting = TbNavigationStarting;
    t->webView->events.navigationCompleted = TbNavigationCompleted;
    t->webView->events.historyChanged = TbHistoryChanged;
    t->webView->events.documentTitleChanged = TbDocumentTitleChanged;
    t->webView->events.mainDocumentResponse = TbMainDocumentResponse;
    t->webView->forwardAppAccelerators = true;
    CreateWebViewArgs cargs;
    cargs.parent = frame;
    cargs.pos = HwndClientRect(frame);
    if (!t->webView->Create(cargs)) {
        delete t->webView;
        t->webView = nullptr;
    }
    tb->tabs.Append(t);
    TbRedrawChrome(tb);
    return t;
}

static void TbActivateTab(TouchBrowser* tb, int idx) {
    int n = len(tb->tabs);
    if (n == 0) {
        tb->activeTab = 0;
        return;
    }
    tb->activeTab = limitValue(idx, 0, n - 1);
    TbSyncUrlBar(tb);
    TbUpdateNavButtons(tb);
    if (tb->chrome) {
        // the progress bar and the "Open in SumatraPDF" button both describe the
        // ACTIVE tab, so both follow the switch
        tb->chrome->SyncProgressToActiveTab();
        tb->chrome->EnsureLayout();
    }
    if (tb->win->touchView == TouchView::Web) {
        // redo the show/hide + z-order dance for the new set of controls, then
        // relayout (which also issues a newly activated tab's deferred Navigate)
        TbSetChildrenVisible(tb, true);
        ScheduleUiUpdate(tb->win, kUiForceRelayout);
        // Put the caret on the page the way a browser does after a tab switch.
        WebviewWnd* wv = TbActiveWebView(tb);
        if (wv) {
            wv->Focus();
        }
    }
    TbRedrawChrome(tb);
}

static void TbOpenNewTab(TouchBrowser* tb, Str url) {
    if (!TbCreateTab(tb, url)) {
        return;
    }
    TbActivateTab(tb, len(tb->tabs) - 1);
}

static void TbOnNewTab(TouchBrowser* tb) {
    TbOpenNewTab(tb, TouchBrowserHomeUrl());
}

struct TbNewTabReq {
    MainWindow* win = nullptr;
    Str url;
};

static void TbOpenNewTabNow(TbNewTabReq* req) {
    if (IsMainWindowValid(req->win) && req->win->touchBrowser) {
        TbOpenNewTab(req->win->touchBrowser, req->url);
    }
    str::Free(req->url);
    delete req;
}

// creating a WebView2 control from inside a WebView2 event callback is asking
// for re-entrancy trouble, so target=_blank links open their tab from the
// message loop instead
static void TbRequestNewTab(MainWindow* win, Str url) {
    auto* req = new TbNewTabReq();
    req->win = win;
    req->url = str::Dup(url);
    uitask::Post(MkFunc0<TbNewTabReq>(TbOpenNewTabNow, req), "TbOpenNewTabNow");
}

static TouchBrowser* CreateTouchBrowser(MainWindow* win) {
    if (!HasWebView()) {
        return nullptr;
    }
    auto* tb = new TouchBrowser();
    tb->win = win;
    tb->hFont = GetDefaultGuiFont();

    auto* chrome = new TbChromeWnd();
    if (!chrome->Create(tb)) {
        delete chrome;
        delete tb;
        return nullptr;
    }
    tb->chrome = chrome;

    TbCreateTab(tb, TouchBrowserHomeUrl());
    tb->activeTab = 0;
    return tb;
}

static void TbSetChildrenVisible(TouchBrowser* tb, bool show) {
    if (!show) {
        // the popups are top-level windows, so leaving the browser would
        // otherwise leave them floating over the document view
        if (tb->menuWnd) {
            tb->menuWnd->Hide();
        }
        TbHideFavMgr(tb);
    }
    // Hide the canvas while the web view is up: it covers the same content area
    // and would paint the Home page through/around the browser chrome. Done here
    // (not only in RelayoutFrame) so it takes effect at switch time.
    if (tb->win && tb->win->hwndCanvas) {
        HwndSetVisible(tb->win->hwndCanvas, !show);
    }
    HWND hwndChrome = tb->chrome ? tb->chrome->hwnd : nullptr;
    if (hwndChrome) {
        ShowWindow(hwndChrome, show ? SW_SHOW : SW_HIDE);
    }
    for (int i = 0; i < len(tb->tabs); i++) {
        WebviewWnd* wv = tb->tabs[i]->webView;
        if (!wv) {
            continue;
        }
        // only the active tab is on screen; the others keep running hidden
        bool isVisible = show && (i == tb->activeTab);
        wv->SetIsVisible(isVisible);
        wv->SetControllerVisible(isVisible);
        // The canvas is a sibling that covers the same content area and sits
        // above us in z-order, so it would paint the Home page over the page
        // content. Raise the webview (both have WS_CLIPSIBLINGS) when shown.
        if (isVisible && wv->hwnd) {
            SetWindowPos(wv->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if (show && hwndChrome) {
        // ...but then the chrome must go above the webview, or WebView2 (which
        // is topmost and briefly covers the whole content area before the first
        // layout) eats clicks meant for it - that is what made the buttons
        // "sometimes stop working".
        SetWindowPos(hwndChrome, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ShowTouchWebView(MainWindow* win, bool show) {
    if (!win) {
        return;
    }
    if (show && !win->touchBrowser) {
        win->touchBrowser = CreateTouchBrowser(win);
    }
    if (!win->touchBrowser) {
        return;
    }
    TbSetChildrenVisible(win->touchBrowser, show);
    // RelayoutFrame's cached layout snapshot records touchView but not whether
    // the browser exists, so the relayout that ran while switching (before the
    // browser was created) would make the next one a no-op - and the canvas
    // would never be hidden. Drop the cache so the next relayout really runs.
    win->uiState.layout = {};
}

void LayoutTouchWebView(MainWindow* win, Rect rc) {
    if (!win || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    TbChromeWnd* chrome = tb->chrome;
    int chromeDy = 0;
    if (chrome && chrome->hwnd) {
        chromeDy = std::min(TbChromeDy(chrome->hwnd), std::max(0, rc.dy));
        MoveWindow(chrome->hwnd, rc.x, rc.y, rc.dx, chromeDy, TRUE);
    }

    int webTop = rc.y + chromeDy;
    Rect webRc{rc.x, webTop, rc.dx, std::max(0, rc.y + rc.dy - webTop)};
    // every tab gets the bounds, so switching to one doesn't show a stale size
    for (TbTab* t : tb->tabs) {
        if (t->webView) {
            t->webView->SetBounds(webRc);
            t->webView->UpdateWebviewSize();
        }
    }
    // ...but the deferred first Navigate only runs for the tab that is actually
    // on screen: a hidden WebView2 control can swallow it. A background tab
    // therefore loads when it is first activated - TbActivateTab forces a
    // relayout, which brings us back here with the tab visible.
    TbTab* act = TbActiveTab(tb);
    if (act && act->webView && !act->didInitialNav && !webRc.IsEmpty()) {
        act->didInitialNav = true;
        TempStr url = str::DupTemp(act->pendingUrl ? act->pendingUrl : TouchBrowserHomeUrl());
        str::FreePtr(&act->pendingUrl);
        act->webView->Navigate(url);
    }
}

void TouchWebGoHome(MainWindow* win) {
    if (win && win->touchBrowser) {
        TbOnHome(win->touchBrowser);
    }
}

void TouchWebToggleBookmark(MainWindow* win) {
    if (!win || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    TbTab* act = TbActiveTab(tb);
    Str url = act ? act->url : Str();
    if (!url) {
        return;
    }
    if (!gGlobalPrefs->browserBookmarks) {
        gGlobalPrefs->browserBookmarks = new Vec<Str>();
    }
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    int found = -1;
    for (int i = 0; i < len(*bm); i++) {
        if (str::EqI((*bm)[i], url)) {
            found = i;
            break;
        }
    }
    // the display name array is index-parallel to the URL array, so it is
    // brought in sync before either is touched and updated in the same step
    TbFavSyncTitles();
    Vec<Str>* titles = TbFavTitlesVec();
    if (found >= 0) {
        str::Free((*bm)[found]);
        bm->RemoveAt(found);
        str::Free((*titles)[found]);
        titles->RemoveAt(found);
    } else {
        bm->Append(str::Dup(url));
        // the page title WebView2 reported, or its host when it has none
        titles->Append(str::Dup(TbFavStoreTitle(act->title, url)));
    }
    SaveSettings();
    TbRelayoutChrome(tb);
    if (win->touchView == TouchView::Web) {
        TbSetChildrenVisible(tb, true);
    }
}

void DestroyTouchWebView(MainWindow* win) {
    if (!win || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    // clear this first: a queued TbCloseTabNow / TbOpenNewTabNow that runs after
    // us must see the browser as gone rather than walk freed tabs
    win->touchBrowser = nullptr;
    for (TbTab* t : tb->tabs) {
        TbDestroyTab(t);
    }
    tb->tabs.Reset();
    delete tb->chrome;
    delete tb->menuWnd;
    delete tb->favMgr;
    delete tb->scrim;
    delete tb;
}
