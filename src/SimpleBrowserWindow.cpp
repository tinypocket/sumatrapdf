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
#include "BrowserUrlUtil.h"

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

constexpr int kTbNavDy = 40;  // navigation row height (logical px)
constexpr int kTbTabsDy = 32; // tab strip row height
constexpr int kTbBmDy = 34;   // bookmarks row height
constexpr int kTbPad = 6;
constexpr int kTbGap = 4;
constexpr int kTbBtnDx = 64;
// tab strip metrics. A tab is a label button plus an adjacent "x" button, so
// kTbTabMinDx has to stay wide enough for the close button and a few glyphs.
constexpr int kTbTabMaxDx = 160;
constexpr int kTbTabMinDx = 62;
constexpr int kTbTabCloseDx = 20;
constexpr int kTbNewTabDx = 28;
// each tab is a live WebView2 control (its own renderer process), and a
// plain-button strip stops being readable well before this many anyway
constexpr int kTbMaxTabs = 10;

struct TouchBrowser;

struct TbChip {
    TouchBrowser* tb = nullptr;
    int idx = 0;
    Button* btn = nullptr;
};

// One browser tab: its own WebView2 control plus the two buttons that stand for
// it in the tab strip (the label, which activates it, and an "x" that closes
// it). All of them are children of hwndFrame, like the rest of the browser
// chrome. `this` is the WebViewEvents ctx of its own webview, so the navigation
// callbacks know which tab they belong to.
struct TbTab {
    TouchBrowser* tb = nullptr;
    WebviewWnd* webView = nullptr;
    Button* btnLabel = nullptr;
    Button* btnClose = nullptr;
    Str url;   // last committed URL; shown in the URL bar while active
    Str title; // document title, empty until the page reports one
    // WebView2 ignores a Navigate issued before the control has a non-zero size
    // and a first layout, so every tab defers its first navigation to
    // LayoutTouchWebView (mirrors SimpleBrowserWindow::Create). Until then the
    // page it should open is parked here.
    Str pendingUrl;
    bool didInitialNav = false;
};

struct TouchBrowser {
    MainWindow* win = nullptr;
    Vec<TbTab*> tabs;
    // index into `tabs`; only this tab's webview is visible. Kept in range by
    // TbActivateTab, and `tabs` is never left empty while the browser exists.
    int activeTab = 0;
    Button* btnBack = nullptr;
    Button* btnForward = nullptr;
    Button* btnHome = nullptr;
    Button* btnStar = nullptr;
    Button* btnInfo = nullptr;
    Button* btnMenu = nullptr;
    Button* btnNewTab = nullptr;
    HWND hwndUrl = nullptr;
    HFONT hFont = nullptr;
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

static void TbSetChildrenVisible(TouchBrowser* tb, bool show);
static void TbActivateTab(TouchBrowser* tb, int idx);
// refreshes a tab's strip button from its title / URL
static void TbUpdateTabButton(TbTab*);
// opens `url` in a new browser tab, from the message loop rather than inline
static void TbRequestNewTab(MainWindow* win, Str url);

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
        auto* d = new TbDocDownload();
        d->win = tb->win;
        d->url = str::Dup(url);
        // save into Downloads under the URL's own file name; fall back to a
        // temp file only if the Downloads folder can't be resolved
        TempStr dir = TbDownloadsDirTemp();
        Str fileName = TbUrlFileNameTemp(url);
        if (dir && fileName) {
            TempStr want = path::JoinTemp(dir, fileName);
            d->destPath = str::Dup(MakeUniqueFilePathTemp(want));
        } else {
            TempStr base = GetTempFilePathTemp("sumatra-web");
            d->destPath = str::Dup(str::JoinTemp(base, ext));
        }
        if (fileName) {
            d->displayName = str::Dup(fileName);
        }
        RunAsync(MkFunc0<TbDocDownload>(TbDocDownloadAsync, d), "TbDocDownloadAsync");
        return false; // cancel the webview navigation
    }
    if (newWindow) {
        TbRequestNewTab(tb->win, url);
        return false;
    }
    // This tab is really leaving its page, so drop the title it had: the strip
    // falls back to the host until the new page reports one. Done here and not
    // in navigationCompleted because DocumentTitleChanged arrives first (the
    // title is known as soon as the document is parsed) and clearing later
    // would throw the new title away. Navigations we cancelled above returned
    // before this, so they keep the title of the page still on screen.
    str::FreePtr(&tab->title);
    TbUpdateTabButton(tab);
    return true;
}

// the nav row always reflects the ACTIVE tab, so background tabs never write to it
static void TbSyncUrlBar(TouchBrowser* tb) {
    if (!tb->hwndUrl) {
        return;
    }
    TbTab* t = TbActiveTab(tb);
    Str url = t ? t->url : Str();
    HwndSetText(tb->hwndUrl, url ? url : StrL(""));
}

static void TbUpdateNavButtons(TouchBrowser* tb) {
    WebviewWnd* wv = TbActiveWebView(tb);
    if (tb->btnBack) {
        tb->btnBack->SetIsEnabled(wv && wv->CanGoBack());
    }
    if (tb->btnForward) {
        tb->btnForward->SetIsEnabled(wv && wv->CanGoForward());
    }
}

static void TbNavigationCompleted(void* ctx, Str url, bool /*success*/) {
    auto* tab = (TbTab*)ctx;
    str::ReplaceWithCopy(&tab->url, url);
    TbUpdateTabButton(tab);
    if (TbIsActiveTab(tab)) {
        TbSyncUrlBar(tab->tb);
        TbUpdateNavButtons(tab->tb);
    }
}

static void TbDocumentTitleChanged(void* ctx, Str title) {
    auto* tab = (TbTab*)ctx;
    str::ReplaceWithCopy(&tab->title, title);
    TbUpdateTabButton(tab);
}

static void TbHistoryChanged(void* ctx, bool canBack, bool canFwd) {
    auto* tab = (TbTab*)ctx;
    if (!TbIsActiveTab(tab)) {
        return;
    }
    TouchBrowser* tb = tab->tb;
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
    TempStr msg = fmt(
        "Documents you open from the browser (PDF, EPUB, MOBI, CBZ, DjVu, XPS, CHM) are "
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
    MsgBox(parent, fmt("Deleted %d file%s.", nDeleted, nDeleted == 1 ? StrL("") : StrL("s")), StrL("Clean up downloads"),
           MB_OK | MB_ICONINFORMATION);
}

// the "..." overflow menu
static void TbOnMenu(TouchBrowser* tb) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    constexpr UINT kCmdCleanup = 1;
    constexpr UINT kCmdInfo = 2;
    AppendMenuW(menu, MF_STRING, kCmdCleanup, L"Clean up downloaded files...");
    AppendMenuW(menu, MF_STRING, kCmdInfo, L"Where do downloads go?");
    Rect r = tb->btnMenu ? HwndWindowRect(tb->btnMenu->hwnd) : Rect{};
    HWND parent = tb->win ? tb->win->hwndFrame : nullptr;
    UINT flags = TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    int cmd = (int)TrackPopupMenu(menu, flags, r.x + r.dx, r.y + r.dy, 0, parent, nullptr);
    DestroyMenu(menu);
    if (cmd == (int)kCmdCleanup) {
        TbCleanupDownloads(tb);
    } else if (cmd == (int)kCmdInfo) {
        TbOnInfo(tb);
    }
}

static void TbOnStar(TouchBrowser* tb) {
    TouchWebToggleBookmark(tb->win);
}
// favorites chips navigate the active tab, like typing in the URL bar does
static void TbOnChipClick(TbChip* c) {
    Vec<Str>* bm = gGlobalPrefs->browserBookmarks;
    WebviewWnd* wv = TbActiveWebView(c->tb);
    if (wv && bm && c->idx >= 0 && c->idx < len(*bm)) {
        wv->Navigate((*bm)[c->idx]);
    }
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

// --- tab strip -------------------------------------------------------------

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

static void TbUpdateTabButton(TbTab* t) {
    if (t->btnLabel) {
        t->btnLabel->SetText(TbTabLabel(t));
    }
}

// A plain Win32 push button has no "selected" look, and the owner-draw path
// (ButtonGetColors) has a single app-wide palette. Button::isDefault is the one
// per-button variation it offers - the palette's brighter edge - so that marks
// the active tab.
static void TbUpdateTabHighlight(TouchBrowser* tb) {
    for (int i = 0; i < len(tb->tabs); i++) {
        TbTab* t = tb->tabs[i];
        bool isActive = (i == tb->activeTab);
        if (t->btnLabel && t->btnLabel->isDefault != isActive) {
            t->btnLabel->isDefault = isActive;
            HwndScheduleRepaint(t->btnLabel->hwnd);
        }
    }
}

static void TbDestroyTab(TbTab* t) {
    delete t->btnLabel;
    delete t->btnClose;
    delete t->webView;
    str::Free(t->url);
    str::Free(t->title);
    str::Free(t->pendingUrl);
    delete t;
}

static void TbOnTabClick(TbTab* t) {
    TouchBrowser* tb = t->tb;
    int idx = tb->tabs.Find(t);
    if (idx >= 0 && idx != tb->activeTab) {
        TbActivateTab(tb, idx);
    }
}

// Closing a tab deletes the very Button whose click handler is running, so the
// work is posted back to the message loop. The request identifies the browser
// by its window, so a browser torn down in the meantime is detected before
// `tab` (which would then be dangling) is ever dereferenced.
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

static void TbOnTabClose(TbTab* t) {
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
    t->btnLabel = TbMakeButton(frame, tb->hFont, StrL("New Tab"), MkFunc0<TbTab>(TbOnTabClick, t));
    t->btnClose = TbMakeButton(frame, tb->hFont, StrL("x"), MkFunc0<TbTab>(TbOnTabClose, t));

    t->webView = new WebviewWnd();
    t->webView->dataDir = str::Dup(GetWebViewDataDirTemp());
    t->webView->enableAutofill = true;
    t->webView->enableBrowserChrome = true;
    t->webView->events.ctx = t;
    t->webView->events.navigationStarting = TbNavigationStarting;
    t->webView->events.navigationCompleted = TbNavigationCompleted;
    t->webView->events.historyChanged = TbHistoryChanged;
    t->webView->events.documentTitleChanged = TbDocumentTitleChanged;
    t->webView->forwardAppAccelerators = true;
    CreateWebViewArgs cargs;
    cargs.parent = frame;
    cargs.pos = HwndClientRect(frame);
    if (!t->webView->Create(cargs)) {
        delete t->webView;
        t->webView = nullptr;
    }
    tb->tabs.Append(t);
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
    TbUpdateTabHighlight(tb);
    if (tb->win->touchView == TouchView::Web) {
        // redo the show/hide + z-order dance for the new set of controls, then
        // relayout (which also issues a newly activated tab's deferred Navigate)
        TbSetChildrenVisible(tb, true);
        ScheduleUiUpdate(tb->win, kUiForceRelayout);
        // Put the caret on the page the way a browser does after a tab switch.
        // It also takes the focus ring off whichever button was clicked, which
        // matters here: a focused button gets the same brighter edge that marks
        // the active tab, so "+" would otherwise look like the selected tab.
        WebviewWnd* wv = TbActiveWebView(tb);
        if (wv) {
            wv->Focus();
        }
    }
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
    HWND frame = win->hwndFrame;

    tb->btnBack = TbMakeButton(frame, tb->hFont, StrL("Back"), MkFunc0<TouchBrowser>(TbOnBack, tb));
    tb->btnBack->SetIsEnabled(false);
    tb->btnForward = TbMakeButton(frame, tb->hFont, StrL("Fwd"), MkFunc0<TouchBrowser>(TbOnForward, tb));
    tb->btnForward->SetIsEnabled(false);
    tb->btnHome = TbMakeButton(frame, tb->hFont, StrL("Home"), MkFunc0<TouchBrowser>(TbOnHome, tb));
    tb->btnStar = TbMakeButton(frame, tb->hFont, StrL("Favorite"), MkFunc0<TouchBrowser>(TbOnStar, tb));
    tb->btnInfo = TbMakeButton(frame, tb->hFont, StrL("i"), MkFunc0<TouchBrowser>(TbOnInfo, tb));
    tb->btnMenu = TbMakeButton(frame, tb->hFont, StrL("..."), MkFunc0<TouchBrowser>(TbOnMenu, tb));
    tb->btnNewTab = TbMakeButton(frame, tb->hFont, StrL("+"), MkFunc0<TouchBrowser>(TbOnNewTab, tb));

    HINSTANCE inst = GetInstance();
    tb->hwndUrl = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                  frame, nullptr, inst, nullptr);
    SendMessageW(tb->hwndUrl, WM_SETFONT, (WPARAM)tb->hFont, TRUE);
    SetWindowSubclass(tb->hwndUrl, TbUrlEditProc, NextSubclassId(), (DWORD_PTR)tb);

    TbCreateTab(tb, TouchBrowserHomeUrl());
    tb->activeTab = 0;
    TbUpdateTabHighlight(tb);
    return tb;
}

static void TbShowWnd(HWND h, bool show) {
    if (h) {
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    }
}

static void TbSetChildrenVisible(TouchBrowser* tb, bool show) {
    // Hide the canvas while the web view is up: it covers the same content area
    // and would paint the Home page through/around the browser chrome. Done here
    // (not only in RelayoutFrame) so it takes effect at switch time.
    if (tb->win && tb->win->hwndCanvas) {
        HwndSetVisible(tb->win->hwndCanvas, !show);
    }
    TbShowWnd(tb->btnBack ? tb->btnBack->hwnd : nullptr, show);
    TbShowWnd(tb->btnForward ? tb->btnForward->hwnd : nullptr, show);
    TbShowWnd(tb->btnHome ? tb->btnHome->hwnd : nullptr, show);
    TbShowWnd(tb->btnStar ? tb->btnStar->hwnd : nullptr, show);
    TbShowWnd(tb->btnInfo ? tb->btnInfo->hwnd : nullptr, show);
    TbShowWnd(tb->btnMenu ? tb->btnMenu->hwnd : nullptr, show);
    TbShowWnd(tb->btnNewTab ? tb->btnNewTab->hwnd : nullptr, show);
    TbShowWnd(tb->hwndUrl, show);
    for (TbChip* c : tb->bmChips) {
        TbShowWnd(c->btn ? c->btn->hwnd : nullptr, show);
    }
    for (TbTab* t : tb->tabs) {
        TbShowWnd(t->btnLabel ? t->btnLabel->hwnd : nullptr, show);
        TbShowWnd(t->btnClose ? t->btnClose->hwnd : nullptr, show);
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
    if (show) {
        // ...but then the nav row, the tab strip and the favorites chips must go
        // above the webview, or WebView2 (which is topmost and briefly covers the
        // whole content area before the first layout) eats clicks meant for them -
        // that is what made the buttons "sometimes stop working".
        auto raise = [](HWND h) {
            if (h) {
                SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        };
        raise(tb->btnBack ? tb->btnBack->hwnd : nullptr);
        raise(tb->btnForward ? tb->btnForward->hwnd : nullptr);
        raise(tb->btnHome ? tb->btnHome->hwnd : nullptr);
        raise(tb->btnStar ? tb->btnStar->hwnd : nullptr);
        raise(tb->btnInfo ? tb->btnInfo->hwnd : nullptr);
        raise(tb->btnMenu ? tb->btnMenu->hwnd : nullptr);
        raise(tb->hwndUrl);
        for (TbTab* t : tb->tabs) {
            raise(t->btnLabel ? t->btnLabel->hwnd : nullptr);
            raise(t->btnClose ? t->btnClose->hwnd : nullptr);
        }
        raise(tb->btnNewTab ? tb->btnNewTab->hwnd : nullptr);
        for (TbChip* c : tb->bmChips) {
            raise(c->btn ? c->btn->hwnd : nullptr);
        }
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
    HWND frame = win->hwndFrame;
    int pad = DpiScale(frame, kTbPad);
    int gap = DpiScale(frame, kTbGap);
    int btnDx = DpiScale(frame, kTbBtnDx);
    int navDy = DpiScale(frame, kTbNavDy);
    int tabsDy = DpiScale(frame, kTbTabsDy);
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
    // right-aligned cluster: [Favorite] [i] [...]
    int miniDx = DpiScale(frame, 30);
    int menuX = rc.x + rc.dx - pad - miniDx;
    if (tb->btnMenu) {
        MoveWindow(tb->btnMenu->hwnd, menuX, y, miniDx, btnDy, TRUE);
    }
    int infoX = menuX - gap - miniDx;
    if (tb->btnInfo) {
        MoveWindow(tb->btnInfo->hwnd, infoX, y, miniDx, btnDy, TRUE);
    }
    int starDx = btnDx + DpiScale(frame, 30);
    int starX = infoX - gap - starDx;
    if (tb->btnStar) {
        MoveWindow(tb->btnStar->hwnd, starX, y, starDx, btnDy, TRUE);
    }
    if (tb->hwndUrl) {
        int urlX = x;
        int urlDx = std::max(0, starX - gap - urlX);
        MoveWindow(tb->hwndUrl, urlX, y, urlDx, btnDy, TRUE);
    }

    // tab strip: [label][x] per tab, then "+". Tabs share the row evenly up to
    // kTbTabMaxDx; a tab that would collide with "+" gets a zero-size rect so it
    // stays out of the way (and unclickable) instead of overlapping it.
    {
        int closeDx = DpiScale(frame, kTbTabCloseDx);
        int newDx = DpiScale(frame, kTbNewTabDx);
        int maxTabDx = DpiScale(frame, kTbTabMaxDx);
        int minTabDx = DpiScale(frame, kTbTabMinDx);
        int tabY = rc.y + navDy + DpiScale(frame, 2);
        int tabDy = std::max(0, tabsDy - DpiScale(frame, 5));
        int right = rc.x + rc.dx - pad;
        int tabsLeft = rc.x + pad;
        int nTabs = len(tb->tabs);
        int tabDx = maxTabDx;
        if (nTabs > 0) {
            int avail = std::max(0, right - tabsLeft - newDx - gap);
            tabDx = std::min(maxTabDx, (avail / nTabs) - gap);
        }
        tabDx = std::max(tabDx, minTabDx);
        int tx = tabsLeft;
        for (TbTab* t : tb->tabs) {
            if (!t->btnLabel || !t->btnClose) {
                continue;
            }
            bool fits = (tx + tabDx + gap + newDx) <= right;
            if (!fits) {
                MoveWindow(t->btnLabel->hwnd, tx, tabY, 0, 0, TRUE);
                MoveWindow(t->btnClose->hwnd, tx, tabY, 0, 0, TRUE);
                continue;
            }
            MoveWindow(t->btnLabel->hwnd, tx, tabY, tabDx - closeDx, tabDy, TRUE);
            MoveWindow(t->btnClose->hwnd, tx + tabDx - closeDx, tabY, closeDx, tabDy, TRUE);
            tx += tabDx + gap;
        }
        if (tb->btnNewTab) {
            int newX = std::min(tx, right - newDx);
            MoveWindow(tb->btnNewTab->hwnd, newX, tabY, newDx, tabDy, TRUE);
        }
    }

    // bookmarks row (chips)
    int bmY = rc.y + navDy + tabsDy;
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

    int webTop = rc.y + navDy + tabsDy + (hasChips ? bmDy : 0);
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
    // clear this first: a queued TbCloseTabNow / TbOpenNewTabNow that runs after
    // us must see the browser as gone rather than walk freed tabs
    win->touchBrowser = nullptr;
    TbDestroyChips(tb);
    for (TbTab* t : tb->tabs) {
        TbDestroyTab(t);
    }
    tb->tabs.Reset();
    delete tb->btnBack;
    delete tb->btnForward;
    delete tb->btnHome;
    delete tb->btnStar;
    delete tb->btnInfo;
    delete tb->btnMenu;
    delete tb->btnNewTab;
    if (tb->hwndUrl) {
        DestroyWindow(tb->hwndUrl);
    }
    delete tb;
}
