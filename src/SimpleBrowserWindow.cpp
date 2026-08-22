/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Dpi.h"

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
#include "Translations.h"

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

constexpr int kTbNavDy = 40; // navigation row height (logical px)
constexpr int kTbBmDy = 34;  // bookmarks row height
constexpr int kTbPad = 6;
constexpr int kTbGap = 4;
constexpr int kTbBtnDx = 64;

struct TouchBrowser;

struct TbChip {
    TouchBrowser* tb = nullptr;
    int idx = 0;
    Button* btn = nullptr;
};

struct TouchBrowser {
    MainWindow* win = nullptr;
    WebviewWnd* webView = nullptr;
    Button* btnBack = nullptr;
    Button* btnForward = nullptr;
    Button* btnHome = nullptr;
    Button* btnStar = nullptr;
    HWND hwndUrl = nullptr;
    HFONT hFont = nullptr;
    Str currentUrl;
    Vec<TbChip*> bmChips;
    bool bmDirty = true;
};

static Str TouchBrowserHomeUrl() {
    Str url = gGlobalPrefs->browserHomePage;
    if (!url) {
        url = StrL("https://www.google.com");
    }
    return url;
}

// a URL points at a document we can open when its path has a supported
// extension (query string / fragment stripped first)
static bool TouchBrowserUrlIsDoc(Str url, Str* extOut) {
    if (!url) {
        return false;
    }
    Str s = url;
    int cut = str::IndexOfChar(s, '?');
    if (cut >= 0) {
        s = Str(s.s, cut);
    }
    cut = str::IndexOfChar(s, '#');
    if (cut >= 0) {
        s = Str(s.s, cut);
    }
    FileType ft = GuessFileTypeFromName(s);
    if (!IsSupportedFileType(ft, true)) {
        return false;
    }
    if (extOut) {
        int dot = str::LastIndexOfChar(s, '.');
        *extOut = (dot >= 0) ? str::DupTemp(Str(s.s + dot, s.len - dot)) : StrL(".dat");
    }
    return true;
}

struct TbDocDownload {
    Str url;
    Str destPath;
    MainWindow* win = nullptr;
};

static void TbDocDownloadFinish(TbDocDownload* d) {
    if (IsMainWindowValid(d->win) && file::Exists(d->destPath)) {
        LoadArgs args(d->destPath, d->win);
        LoadDocument(&args);
        SetTouchView(d->win, TouchView::Doc);
    }
    str::Free(d->url);
    str::Free(d->destPath);
    delete d;
}

static void TbDocDownloadAsync(TbDocDownload* d) {
    constexpr i64 kMaxWebDocSize = 256LL * 1024 * 1024;
    HttpGetToFile(d->url, d->destPath, {}, kMaxWebDocSize);
    uitask::Post(MkFunc0<TbDocDownload>(TbDocDownloadFinish, d), "TbDocDownloadFinish");
}

// navigationStarting: intercept links to documents so they open as tabs in
// SumatraPDF+ instead of navigating the webview; everything else proceeds.
static bool TbNavigationStarting(void* ctx, Str url, bool /*newWindow*/) {
    auto* tb = (TouchBrowser*)ctx;
    Str ext;
    if (TouchBrowserUrlIsDoc(url, &ext)) {
        auto* d = new TbDocDownload();
        d->win = tb->win;
        d->url = str::Dup(url);
        TempStr base = GetTempFilePathTemp("sumatra-web");
        d->destPath = str::Dup(str::JoinTemp(base, ext));
        RunAsync(MkFunc0<TbDocDownload>(TbDocDownloadAsync, d), "TbDocDownloadAsync");
        return false; // cancel the webview navigation
    }
    return true;
}

static void TbSetUrl(TouchBrowser* tb, Str url) {
    str::ReplaceWithCopy(&tb->currentUrl, url);
    if (tb->hwndUrl) {
        HwndSetText(tb->hwndUrl, url);
    }
}

static void TbUpdateNavButtons(TouchBrowser* tb) {
    if (tb->btnBack && tb->webView) {
        tb->btnBack->SetIsEnabled(tb->webView->CanGoBack());
    }
    if (tb->btnForward && tb->webView) {
        tb->btnForward->SetIsEnabled(tb->webView->CanGoForward());
    }
}

static void TbNavigationCompleted(void* ctx, Str url, bool /*success*/) {
    auto* tb = (TouchBrowser*)ctx;
    TbSetUrl(tb, url);
    TbUpdateNavButtons(tb);
}

static void TbHistoryChanged(void* ctx, bool canBack, bool canFwd) {
    auto* tb = (TouchBrowser*)ctx;
    if (tb->btnBack) {
        tb->btnBack->SetIsEnabled(canBack);
    }
    if (tb->btnForward) {
        tb->btnForward->SetIsEnabled(canFwd);
    }
}

// Enter in the URL field navigates (prefixing https:// when no scheme is typed)
static LRESULT CALLBACK TbUrlEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* tb = (TouchBrowser*)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        TempStr txt = HwndGetTextTemp(hwnd);
        if (tb && tb->webView && !str::IsEmptyOrWhiteSpace(txt)) {
            Str url = txt;
            if (!str::StartsWithI(url, StrL("http://")) && !str::StartsWithI(url, StrL("https://"))) {
                url = str::JoinTemp(StrL("https://"), txt);
            }
            tb->webView->Navigate(url);
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void TbOnBack(TouchBrowser* tb) {
    if (tb->webView) {
        tb->webView->GoBack();
    }
}
static void TbOnForward(TouchBrowser* tb) {
    if (tb->webView) {
        tb->webView->GoForward();
    }
}
static void TbOnHome(TouchBrowser* tb) {
    if (tb->webView) {
        tb->webView->Navigate(TouchBrowserHomeUrl());
    }
}
static void TbOnStar(TouchBrowser* tb) {
    TouchWebToggleBookmark(tb->win);
}
static void TbOnChipClick(TbChip* c) {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    if (c->tb->webView && bm && c->idx >= 0 && c->idx < len(*bm)) {
        c->tb->webView->Navigate((*bm)[c->idx]);
    }
}

static TempStr TbChipLabel(Str url) {
    // show the host (strip scheme + path) so chips stay short
    Str s = url;
    if (str::StartsWithI(s, StrL("https://"))) {
        s = Str(s.s + 8, s.len - 8);
    } else if (str::StartsWithI(s, StrL("http://"))) {
        s = Str(s.s + 7, s.len - 7);
    }
    int slash = str::IndexOfChar(s, '/');
    if (slash >= 0) {
        s = Str(s.s, slash);
    }
    if (str::StartsWithI(s, StrL("www."))) {
        s = Str(s.s + 4, s.len - 4);
    }
    return str::DupTemp(s);
}

static void TbDestroyChips(TouchBrowser* tb) {
    for (TbChip* c : tb->bmChips) {
        delete c->btn;
        delete c;
    }
    tb->bmChips.Reset();
}

static void TbRebuildChips(TouchBrowser* tb) {
    TbDestroyChips(tb);
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    if (bm) {
        for (int i = 0; i < len(*bm); i++) {
            auto* c = new TbChip();
            c->tb = tb;
            c->idx = i;
            Button::CreateArgs ba;
            ba.parent = tb->win->hwndFrame;
            ba.font = tb->hFont;
            ba.text = TbChipLabel((*bm)[i]);
            c->btn = new Button();
            c->btn->Create(ba);
            c->btn->onClick = MkFunc0<TbChip>(TbOnChipClick, c);
            tb->bmChips.Append(c);
        }
    }
    tb->bmDirty = false;
}

static Button* TbMakeButton(HWND frame, HFONT font, Str text, const Func0& onClick) {
    Button::CreateArgs ba;
    ba.parent = frame;
    ba.font = font;
    ba.text = text;
    auto* b = new Button();
    b->Create(ba);
    b->onClick = onClick;
    return b;
}

static TouchBrowser* CreateTouchBrowser(MainWindow* win) {
    if (!HasWebView()) {
        return nullptr;
    }
    auto* tb = new TouchBrowser();
    tb->win = win;
    tb->hFont = GetDefaultGuiFont();
    HWND frame = win->hwndFrame;

    tb->btnBack = TbMakeButton(frame, tb->hFont, StrL("Back"), MkFunc0<TouchBrowser>(TbOnBack, tb));
    tb->btnBack->SetIsEnabled(false);
    tb->btnForward = TbMakeButton(frame, tb->hFont, StrL("Fwd"), MkFunc0<TouchBrowser>(TbOnForward, tb));
    tb->btnForward->SetIsEnabled(false);
    tb->btnHome = TbMakeButton(frame, tb->hFont, StrL("Home"), MkFunc0<TouchBrowser>(TbOnHome, tb));
    tb->btnStar = TbMakeButton(frame, tb->hFont, StrL("Bookmark"), MkFunc0<TouchBrowser>(TbOnStar, tb));

    HINSTANCE inst = GetInstance();
    tb->hwndUrl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                  frame, nullptr, inst, nullptr);
    SendMessageW(tb->hwndUrl, WM_SETFONT, (WPARAM)tb->hFont, TRUE);
    SetWindowSubclass(tb->hwndUrl, TbUrlEditProc, NextSubclassId(), (DWORD_PTR)tb);

    tb->webView = new WebviewWnd();
    tb->webView->dataDir = str::Dup(GetWebViewDataDirTemp());
    tb->webView->enableAutofill = true;
    tb->webView->enableBrowserChrome = true;
    tb->webView->events.ctx = tb;
    tb->webView->events.navigationStarting = TbNavigationStarting;
    tb->webView->events.navigationCompleted = TbNavigationCompleted;
    tb->webView->events.historyChanged = TbHistoryChanged;
    tb->webView->forwardAppAccelerators = true;
    CreateWebViewArgs cargs;
    cargs.parent = frame;
    cargs.pos = Rect(0, 0, 0, 0);
    if (!tb->webView->Create(cargs)) {
        delete tb->webView;
        tb->webView = nullptr;
    } else {
        tb->webView->Navigate(TouchBrowserHomeUrl());
    }
    return tb;
}

static void TbShowWnd(HWND h, bool show) {
    if (h) {
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
}

static void TbSetChildrenVisible(TouchBrowser* tb, bool show) {
    TbShowWnd(tb->btnBack ? tb->btnBack->hwnd : nullptr, show);
    TbShowWnd(tb->btnForward ? tb->btnForward->hwnd : nullptr, show);
    TbShowWnd(tb->btnHome ? tb->btnHome->hwnd : nullptr, show);
    TbShowWnd(tb->btnStar ? tb->btnStar->hwnd : nullptr, show);
    TbShowWnd(tb->hwndUrl, show);
    for (TbChip* c : tb->bmChips) {
        TbShowWnd(c->btn ? c->btn->hwnd : nullptr, show);
    }
    if (tb->webView) {
        tb->webView->SetIsVisible(show);
        tb->webView->SetControllerVisible(show);
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
    if (show && win->touchBrowser->bmDirty) {
        TbRebuildChips(win->touchBrowser);
    }
    TbSetChildrenVisible(win->touchBrowser, show);
}

void LayoutTouchWebView(MainWindow* win, Rect rc) {
    if (!win || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    HWND frame = win->hwndFrame;
    int pad = DpiScale(frame, kTbPad);
    int gap = DpiScale(frame, kTbGap);
    int btnDx = DpiScale(frame, kTbBtnDx);
    int navDy = DpiScale(frame, kTbNavDy);
    int bmDy = DpiScale(frame, kTbBmDy);
    int btnDy = navDy - 2 * pad;

    int x = rc.x + pad;
    int y = rc.y + pad;
    auto place = [&](Button* b) {
        if (b) {
            MoveWindow(b->hwnd, x, y, btnDx, btnDy, TRUE);
            x += btnDx + gap;
        }
    };
    place(tb->btnBack);
    place(tb->btnForward);
    place(tb->btnHome);
    // Bookmark button sits at the right edge of the nav row
    int starDx = btnDx + DpiScale(frame, 30);
    int starX = rc.x + rc.dx - pad - starDx;
    if (tb->btnStar) {
        MoveWindow(tb->btnStar->hwnd, starX, y, starDx, btnDy, TRUE);
    }
    if (tb->hwndUrl) {
        int urlX = x;
        int urlDx = std::max(0, starX - gap - urlX);
        MoveWindow(tb->hwndUrl, urlX, y, urlDx, btnDy, TRUE);
    }

    // bookmarks row (chips)
    int bmY = rc.y + navDy;
    bool hasChips = len(tb->bmChips) > 0;
    if (hasChips) {
        int cx = rc.x + pad;
        int cy = bmY + DpiScale(frame, 3);
        int chipDy = bmDy - DpiScale(frame, 6);
        for (TbChip* c : tb->bmChips) {
            Size ideal = c->btn->GetIdealSize();
            int cdx = std::min(ideal.dx + DpiScale(frame, 12), DpiScale(frame, 160));
            if (cx + cdx > rc.x + rc.dx - pad) {
                break; // one row of chips; overflow is dropped
            }
            MoveWindow(c->btn->hwnd, cx, cy, cdx, chipDy, TRUE);
            cx += cdx + gap;
        }
    }

    int webTop = rc.y + navDy + (hasChips ? bmDy : 0);
    Rect webRc{rc.x, webTop, rc.dx, std::max(0, rc.y + rc.dy - webTop)};
    if (tb->webView) {
        tb->webView->SetBounds(webRc);
        tb->webView->UpdateWebviewSize();
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
    Str url = tb->currentUrl;
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
    if (found >= 0) {
        str::Free((*bm)[found]);
        bm->RemoveAt(found);
    } else {
        bm->Append(str::Dup(url));
    }
    TbRebuildChips(tb);
    SaveSettings();
    if (win->touchView == TouchView::Web) {
        TbSetChildrenVisible(tb, true);
        ScheduleUiUpdate(win, kUiForceRelayout);
    }
}

void DestroyTouchWebView(MainWindow* win) {
    if (!win || !win->touchBrowser) {
        return;
    }
    TouchBrowser* tb = win->touchBrowser;
    TbDestroyChips(tb);
    delete tb->btnBack;
    delete tb->btnForward;
    delete tb->btnHome;
    delete tb->btnStar;
    if (tb->hwndUrl) {
        DestroyWindow(tb->hwndUrl);
    }
    delete tb->webView;
    str::Free(tb->currentUrl);
    delete tb;
    win->touchBrowser = nullptr;
}
