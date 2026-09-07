/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Dpi.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/Anim.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "TouchMetrics.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "TableOfContents.h"
#include "Tabs.h"
#include "Commands.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "HomePage.h"
#include "Rail.h"
#include "TopBar.h"
#include "Theme.h"
#include "Translations.h"

static Kind kindRail = "rail";
static Kind kindSidebarToggle = "sidebarToggle";

struct RailItem {
    TbIcon icon;
    TouchPanelMode mode;
    TouchView view;
    int cmdId;
    // shown at the bottom of the rail instead of the top
    bool atBottom;
    bool readingMode;
    bool nonTabOnly = false;
};

constexpr int kRailDocumentPreview = -1;
// hovering the document switcher opens the preview strip after this delay
constexpr UINT_PTR kRailHoverTimerId = 4;
constexpr int kRailHoverDelayMs = 450;
// drives the hover / press / active-marker transitions; only runs while one of
// them is in flight
constexpr UINT_PTR kRailAnimTimerId = 5;
// How far along the normal -> pressed color path a plain hover sits, so hover
// and press are two stops on one path rather than two unrelated colors.
constexpr float kRailHoverEmphasis = 0.55f;
constexpr float kRailPressEmphasis = 1.0f - kRailHoverEmphasis;
// how deep a fully pressed button sinks
constexpr int kRailPressInset = 2;

static RailItem gRailItems[] = {
    {TbIcon::Document, TouchPanelMode::Bookmarks, TouchView::Doc, 0, false, true},
    {TbIcon::HomeList, TouchPanelMode::Bookmarks, TouchView::Doc, 0, false, false},
    {TbIcon::HomeThumbnails, TouchPanelMode::Thumbnails, TouchView::Doc, 0, false, false},
    {TbIcon::Search, TouchPanelMode::Search, TouchView::Doc, 0, false, false},
    {TbIcon::Annotation, TouchPanelMode::Annotations, TouchView::Doc, 0, false, false},
    {TbIcon::Attachment, TouchPanelMode::Attachments, TouchView::Doc, 0, false, false},
    // "PDF favorites": saved pages across EVERY document, grouped by file, so
    // a favorite in another PDF is one tap away (it opens that PDF at that
    // page). It reads across documents rather than describing the current one,
    // so it belongs to the bottom group with the Library and the browser, not
    // with the panels above that are about the open document.
    {TbIcon::Bookmark, TouchPanelMode::Favorites, TouchView::Doc, 0, true, false},
    // Recent used to be a rail item of its own; it's now the Library's first
    // sidebar row, so the Library is the single browsing destination
    {TbIcon::Library, TouchPanelMode::Bookmarks, TouchView::Library, 0, true, false},
    {TbIcon::Web, TouchPanelMode::Bookmarks, TouchView::Web, 0, true, false},
    {TbIcon::WindowStack, TouchPanelMode::Bookmarks, TouchView::Doc, kRailDocumentPreview, true, false},
};

constexpr int kRailItemsCount = (int)dimof(gRailItems);

static bool IsRailItemVisible(const RailItem& item) {
    return !item.nonTabOnly || !SettingsUseTabs();
}

struct SidebarToggleWnd : Wnd {
    SidebarToggleWnd();
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;
    bool Create(MainWindow*);

    MainWindow* win = nullptr;
    bool hot = false;
    bool trackingMouse = false;
};

struct RailWnd : Wnd {
    RailWnd();
    ~RailWnd() override;

    HWND Create(MainWindow*);
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    void RebuildImageLists();
    // index of the item at pt, or -1
    int ItemFromPoint(Point pt);
    Rect ItemRect(int idx);

    void SetHot(int idx);
    void SetPressed(int idx);
    void SyncActiveMarker(int activeIdx);
    Rect MarkerBandRect();
    void OnAnimTick();

    MainWindow* win = nullptr;
    // icons in the two colors a rail button can have. Rebuilt on theme change.
    HIMAGELIST imlNormal = nullptr;
    HIMAGELIST imlActive = nullptr;
    HIMAGELIST imlDisabled = nullptr;
    int iconDy = 0;
    int hotIdx = -1;
    bool trackingMouse = false;
    SidebarToggleWnd* collapseWnd = nullptr;

    AnimTimer animTimer;
    AnimVal hoverAnim[kRailItemsCount];
    AnimVal pressAnim[kRailItemsCount];
    // an item that finished animating still owes one last frame in its settled
    // state, so remember what we invalidated on the previous tick
    bool animDirty[kRailItemsCount]{};
    int pressedIdx = -1;
    // the accent marker slides from the item that had it to the one that has it
    AnimVal markerAnim;
    int markerFromIdx = -1;
    int markerToIdx = -1;
    bool markerKnown = false;
    bool markerDirty = false;
};

RailWnd::RailWnd() {
    kind = kindRail;
}

RailWnd::~RailWnd() {
    // before ~Wnd tears the window down, so no timer outlives the object
    animTimer.Stop();
    delete collapseWnd;
    if (imlNormal) {
        ImageList_Destroy(imlNormal);
    }
    if (imlActive) {
        ImageList_Destroy(imlActive);
    }
    if (imlDisabled) {
        ImageList_Destroy(imlDisabled);
    }
}

// background of the rail. A step back from the top bar (#f3f0eb vs #faf8f5
// in the design) so the rail and panel read as one surface beside it.
static COLORREF RailBgColor() {
    return ThemeHotBackgroundColor();
}

// background of the button whose panel is currently showing. In the Touch
// Paper theme this is the accent tint the redesign calls for; other themes get
// their notification highlight, which plays the same role.
static COLORREF RailActiveBgColor() {
    COLORREF bg, fg;
    ThemeAccentSurfaceColors(&bg, &fg);
    return bg;
}

static COLORREF RailActiveFgColor() {
    COLORREF bg, fg;
    ThemeAccentSurfaceColors(&bg, &fg);
    return fg;
}

static COLORREF RailFgColor() {
    return ThemeWindowDarkerTextColor();
}

// The far end of a button's hover/press path. A plain hover only travels
// kRailHoverEmphasis of the way here, which lands on the same step the
// un-animated hover used to paint.
static COLORREF RailEmphBgColor() {
    return AccentColor(RailBgColor(), 18);
}

static COLORREF RailActiveEmphBgColor() {
    return AccentColor(RailActiveBgColor(), 14);
}

void RailWnd::RebuildImageLists() {
    if (imlNormal) {
        ImageList_Destroy(imlNormal);
        imlNormal = nullptr;
    }
    if (imlActive) {
        ImageList_Destroy(imlActive);
        imlActive = nullptr;
    }
    if (imlDisabled) {
        ImageList_Destroy(imlDisabled);
        imlDisabled = nullptr;
    }
    iconDy = DpiScale(hwnd, kTopBarIconDy);
    imlNormal = BuildTintedToolbarImageList(iconDy, RailFgColor(), RailBgColor());
    imlActive = BuildTintedToolbarImageList(iconDy, RailActiveFgColor(), RailActiveBgColor());
    // document-only items are greyed out when no document is open
    imlDisabled = BuildTintedToolbarImageList(iconDy, ThemeWindowTextDisabledColor(), RailBgColor());
}

// true if this item's panel is the one currently showing
static bool IsRailItemActive(MainWindow* win, const RailItem& item) {
    if (!win || item.cmdId) {
        return false;
    }
    if (item.readingMode) {
        return win->touchView == TouchView::Doc && win->touchSidebarCollapsed;
    }
    if (win->touchView != item.view) {
        return false;
    }
    return item.view != TouchView::Doc ||
           (!win->touchSidebarCollapsed && win->uiState.tocVisible && win->touchPanelMode == item.mode);
}

// Is a real document open? NOT MainWindow::HasDocsLoaded(): that returns true
// when there are zero tabs, which is exactly the no-document case here, so the
// document-only rail icons never greyed out.
static bool RailHasDocument(MainWindow* win) {
    if (!win) {
        return false;
    }
    for (int i = 0; i < win->TabCount(); i++) {
        WindowTab* tab = win->GetTab(i);
        if (tab && !tab->IsAboutTab()) {
            return true;
        }
    }
    // tabless mode: no tabs at all, but a document may still be loaded
    return win->ctrl != nullptr;
}

static bool IsRailItemEnabled(MainWindow* win, const RailItem& item) {
    if (!win) {
        return false;
    }
    // Favorites works without a document (see SetTouchPanelMode)
    if (item.mode == TouchPanelMode::Favorites) {
        return true;
    }
    return item.cmdId || item.view != TouchView::Doc || RailHasDocument(win);
}

// The bookmarks tree is filled lazily, by SetSidebarVisibility, when the
// classic sidebar opens. The touch pane opens through the paths below without
// going that way, so a document loaded with the pane closed (the default now)
// showed an empty Bookmarks list when the pane was opened later.
static void EnsureTouchPaneLoaded(MainWindow* win) {
    if (win && win->IsDocLoaded() && win->ctrl && win->CurrentTab() && !win->tocLoaded) {
        LoadTocTree(win);
    }
}

void SetTouchSidebarCollapsed(MainWindow* win, bool collapsed) {
    if (!win || !IsTouchChrome(win) || win->touchView != TouchView::Doc || !win->HasDocsLoaded()) {
        return;
    }
    if (!collapsed) {
        EnsureTouchPaneLoaded(win);
    }
    win->touchSidebarCollapsed = collapsed;
    win->uiState.tocVisible = !collapsed;
    win->uiState.favVisible = false;
    // The pane's state is the user's, not the document's: it is kept globally
    // (TouchSidebarOpen) so the next document opens the way this one was left,
    // and a document is never allowed to pop it open just because it has
    // bookmarks. The tab mirrors it for the classic code paths that read
    // showToc (fullscreen, tab switches, FileState).
    gGlobalPrefs->touchSidebarOpen = !collapsed;
    WindowTab* tab = win->CurrentTab();
    if (tab && !tab->IsAboutTab()) {
        tab->showToc = !collapsed;
    }
    UpdateRailForWindow(win);
    ScheduleUiUpdate(win, kUiForceRelayout | kUiSidebarDirty | kUiToolbarDirty);
}

static void SetTouchHomeTabLabel(MainWindow* win, TouchView view) {
    Str label = StrL("Library");
    if (view == TouchView::Web) {
        label = StrL("Web");
    }
    for (int i = 0; i < win->TabCount(); i++) {
        WindowTab* tab = win->GetTab(i);
        if (tab && tab->IsAboutTab()) {
            tab->SetDisplayName(label);
            win->tabsCtrl->SetTextAndTooltip(i, label, Str{});
            return;
        }
    }
}

void SetTouchView(MainWindow* win, TouchView view) {
    if (!win || !IsTouchChrome(win)) {
        return;
    }
    // Home folded into the Library (its "Recent" sidebar row); nothing should
    // navigate to it any more
    if (view == TouchView::Home) {
        view = TouchView::Library;
    }
    TouchView oldView = win->touchView;
    if (oldView != view && oldView == TouchView::Library) {
        HomePageDestroySearch(win);
    }
    if (oldView != view && oldView == TouchView::Web) {
        ShowTouchWebView(win, false);
    }
    if (view == TouchView::Doc) {
        TouchView previous = win->touchView;
        win->touchView = TouchView::Doc;
        if (!SelectTouchDocumentTab(win)) {
            win->touchView = previous;
            return;
        }
        // The pane opens if the user left it open (TouchSidebarOpen), whatever
        // the document: it is never forced open by a document's bookmarks, and
        // never forced shut by their absence (Search and Thumbnails are useful
        // without any).
        bool showToc = gGlobalPrefs->touchSidebarOpen;
        WindowTab* docTab = win->CurrentTab();
        if (docTab && !docTab->IsAboutTab()) {
            docTab->showToc = showToc;
        }
        win->uiState.tocVisible = showToc;
        win->touchSidebarCollapsed = !showToc;
        if (showToc) {
            EnsureTouchPaneLoaded(win);
        }
    } else {
        win->touchView = view;
        if (!SelectTouchHomeTab(win)) {
            return;
        }
        win->uiState.tocVisible = false;
        win->uiState.favVisible = false;
    }
    win->touchView = view;
    if (view == TouchView::Library || view == TouchView::Web) {
        SetTouchHomeTabLabel(win, view);
        // remember where the user was, so closing the last document comes back here
        win->lastNonDocView = view;
    }
    if (view == TouchView::Web) {
        ShowTouchWebView(win, true);
    }
    UpdateRailForWindow(win);
    ScheduleUiUpdate(win, kUiForceRelayout | kUiSidebarDirty | kUiToolbarDirty);
    HwndInvalidate(win->hwndCanvas, true);
}

void SetTouchPanelMode(MainWindow* win, TouchPanelMode mode) {
    if (!win || !IsTouchChrome(win)) {
        return;
    }
    TouchView previous = win->touchView;
    win->touchView = TouchView::Doc;
    bool haveDoc = SelectTouchDocumentTab(win);
    if (!haveDoc) {
        // Favorites are stored per file and outlive the session, so the panel
        // is useful with nothing open - it is how you get back to a page you
        // marked. Every other panel describes the current document and has
        // nothing to say without one.
        if (mode != TouchPanelMode::Favorites) {
            win->touchView = previous;
            return;
        }
    }
    // Coming from the in-app browser, the webview covers the whole content
    // area and stays on top of whatever the panel is opened beside - so
    // opening a panel from the browser looked like nothing happened. Leave the
    // browser properly: the last document takes the right-hand side, or the
    // Library when there is no document to go back to.
    if (previous == TouchView::Web) {
        ShowTouchWebView(win, false);
        HomePageDestroySearch(win);
    }
    if (haveDoc) {
        win->touchView = TouchView::Doc;
    } else {
        win->touchView = TouchView::Library;
        SelectTouchHomeTab(win);
        SetTouchHomeTabLabel(win, TouchView::Library);
        win->lastNonDocView = TouchView::Library;
    }
    SetTouchPanelModeAndRestoreSearch(win, mode);
    EnsureTouchPaneLoaded(win);
    win->touchSidebarCollapsed = false;
    win->touchPanelScrollY = 0;
    win->uiState.tocVisible = true;
    // opening a panel is the other half of the same state (see above)
    gGlobalPrefs->touchSidebarOpen = true;
    WindowTab* panelTab = win->CurrentTab();
    if (panelTab && !panelTab->IsAboutTab()) {
        panelTab->showToc = true;
    }
    win->uiState.favVisible = false;
    UpdateRailForWindow(win);
    ScheduleUiUpdate(win, kUiForceRelayout | kUiSidebarDirty);
}

void SetTouchDocumentTab(MainWindow* win, int tabIndex) {
    if (!win || !IsTouchChrome(win) || tabIndex < 0 || tabIndex >= win->TabCount()) {
        return;
    }
    WindowTab* tab = win->GetTab(tabIndex);
    if (!tab || tab->IsNonDocumentTab()) {
        return;
    }
    HomePageDestroySearch(win);
    win->touchView = TouchView::Doc;
    // the pane keeps the state the user left it in (see SetTouchSidebarCollapsed)
    bool showToc = gGlobalPrefs->touchSidebarOpen;
    win->touchSidebarCollapsed = !showToc;
    TabsSelect(win, tabIndex);
    win->uiState.tocVisible = showToc;
    win->uiState.favVisible = false;
    if (showToc) {
        EnsureTouchPaneLoaded(win);
    }
    UpdateTouchPanelMode(win);
    UpdateRailForWindow(win);
    ScheduleUiUpdate(win, kUiForceRelayout | kUiSidebarDirty | kUiToolbarDirty | kUiTabsDirty);
}

Rect RailWnd::ItemRect(int idx) {
    Rect rc = HwndClientRect(hwnd);
    int btnDy = DpiScale(hwnd, kRailBtnDy);
    int gap = DpiScale(hwnd, kRailBtnGap);
    int padY = DpiScale(hwnd, kRailPadY);
    int x = (rc.dx - btnDy) / 2;

    const RailItem& item = gRailItems[idx];
    if (!IsRailItemVisible(item)) {
        return {};
    }
    if (!item.atBottom) {
        int nBefore = 0;
        for (int i = 0; i < idx; i++) {
            if (IsRailItemVisible(gRailItems[i]) && !gRailItems[i].atBottom) {
                nBefore++;
            }
        }
        int y = padY + nBefore * (btnDy + gap);
        if (!item.readingMode) {
            // 32px-wide divider with the same 4px vertical margins as the
            // design's bottom-group divider.
            y += DpiScale(hwnd, 9);
        }
        return Rect{x, y, btnDy, btnDy};
    }
    // bottom items are laid out upwards from the bottom edge
    int nAfter = 0;
    for (int i = idx + 1; i < kRailItemsCount; i++) {
        if (IsRailItemVisible(gRailItems[i]) && gRailItems[i].atBottom) {
            nAfter++;
        }
    }
    int y = rc.dy - padY - btnDy - nAfter * (btnDy + gap);
    // a short rail (small window, high dpi) would otherwise stack the bottom
    // items on top of the last top item
    int nTop = 0;
    for (int i = 0; i < kRailItemsCount; i++) {
        if (IsRailItemVisible(gRailItems[i]) && !gRailItems[i].atBottom) {
            nTop++;
        }
    }
    int nBottomBefore = 0;
    for (int i = 0; i < idx; i++) {
        if (IsRailItemVisible(gRailItems[i]) && gRailItems[i].atBottom) {
            nBottomBefore++;
        }
    }
    int minY = padY + (nTop + nBottomBefore) * (btnDy + gap) + DpiScale(hwnd, 9);
    y = std::max(y, minY);
    return Rect{x, y, btnDy, btnDy};
}

int RailWnd::ItemFromPoint(Point pt) {
    for (int i = 0; i < kRailItemsCount; i++) {
        if (ItemRect(i).Contains(pt)) {
            return i;
        }
    }
    return -1;
}

static void FillRoundedRect(HDC hdc, const Rect& r, int radius, COLORREF col) {
    AutoDeleteBrush br = CreateSolidBrush(col);
    AutoDeletePen pen = CreatePen(PS_SOLID, 1, col);
    ScopedSelectObject selBr(hdc, br);
    ScopedSelectObject selPen(hdc, pen);
    RoundRect(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy, radius * 2, radius * 2);
}

SidebarToggleWnd::SidebarToggleWnd() {
    kind = kindSidebarToggle;
}

bool SidebarToggleWnd::Create(MainWindow* w) {
    win = w;
    CreateCustomArgs args;
    args.parent = w->hwndFrame;
    args.style = WS_CHILD | WS_CLIPSIBLINGS;
    args.visible = false;
    CreateCustom(args);
    return hwnd != nullptr;
}

void SidebarToggleWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    Rect rc = HwndClientRect(hwnd);
    HdcFillRect(hdc, ToRect(ps->rcPaint), ThemeWindowControlBackgroundColor());

    int radius = rc.dy / 2;
    Rect shadow = rc;
    shadow.y += DpiScale(hwnd, 2);
    shadow.dy -= DpiScale(hwnd, 2);
    FillRoundedRect(hdc, shadow, radius, RGB(0xd5, 0xcf, 0xc6));

    Rect surface = rc;
    surface.dy -= DpiScale(hwnd, 2);
    COLORREF bg = hot ? AccentColor(ThemeWindowControlBackgroundColor(), 8) : ThemeWindowControlBackgroundColor();
    AutoDeleteBrush brush = CreateSolidBrush(bg);
    AutoDeletePen pen = CreatePen(PS_SOLID, 1, ThemeEdgeColor());
    ScopedSelectObject selectBrush(hdc, brush);
    ScopedSelectObject selectPen(hdc, pen);
    RoundRect(hdc, surface.x, surface.y, surface.x + surface.dx, surface.y + surface.dy, radius * 2, radius * 2);
    int chevronDx = DpiScale(hwnd, 3);
    int chevronDy = DpiScale(hwnd, 5);
    int cx = surface.x + surface.dx / 2;
    int cy = surface.y + surface.dy / 2;
    int outerX = cx + chevronDx;
    int innerX = cx - chevronDx;
    AutoDeletePen chevronPen = CreatePen(PS_SOLID, DpiScale(hwnd, 2), ThemeWindowDarkerTextColor());
    ScopedSelectObject selectChevronPen(hdc, chevronPen);
    MoveToEx(hdc, outerX, cy - chevronDy, nullptr);
    LineTo(hdc, innerX, cy);
    LineTo(hdc, outerX, cy + chevronDy);
}

LRESULT SidebarToggleWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_SIZE) {
        int dx = LOWORD(lparam);
        int dy = HIWORD(lparam);
        HRGN region = CreateEllipticRgn(0, 0, dx, dy);
        if (!SetWindowRgn(hwnd, region, TRUE)) {
            DeleteObject(region);
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        if (!hot) {
            hot = true;
            HwndInvalidate(hwnd, false);
        }
        if (!trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            trackingMouse = true;
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        trackingMouse = false;
        hot = false;
        HwndInvalidate(hwnd, false);
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        SetTouchSidebarCollapsed(win, true);
        return 0;
    }
    return WndProcDefault(hwnd, msg, wparam, lparam);
}

// small badge in the switcher button's top-right corner with the number of
// open documents, like a taskbar/notification count
static void DrawRailCountPill(HDC hdc, HWND hwnd, const Rect& btn, int count) {
    TempStr txt = count > 99 ? str::DupTemp("99+") : fmt("%d", count);
    HFONT font = HdcGetUiFont(hdc, 10, FW_SEMIBOLD);
    Size sz = HdcMeasureText(hdc, txt, font);
    int padX = DpiScale(hwnd, 5);
    int dy = DpiScale(hwnd, 15);
    int dx = std::max(dy, sz.dx + 2 * padX);
    Rect pill{btn.x + btn.dx - dx + DpiScale(hwnd, 2), btn.y + DpiScale(hwnd, 1), dx, dy};
    COLORREF bg, fg;
    ThemeAccentSurfaceColors(&bg, &fg);
    FillRoundedRect(hdc, pill, dy / 2, fg);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, bg);
    HdcDrawText(hdc, txt, pill, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, font);
}

// where the accent marker sits for a given button
static Rect RailMarkerRect(HWND hwnd, const Rect& btn) {
    int markerDx = DpiScale(hwnd, kRailMarkerDx);
    int markerDy = DpiScale(hwnd, kRailMarkerDy);
    return Rect{0, btn.y + (btn.dy - markerDy) / 2, markerDx, markerDy};
}

// the strip the marker can be anywhere within while it slides
Rect RailWnd::MarkerBandRect() {
    Rect band;
    if (markerFromIdx >= 0) {
        band = RailMarkerRect(hwnd, ItemRect(markerFromIdx));
    }
    if (markerToIdx >= 0) {
        Rect to = RailMarkerRect(hwnd, ItemRect(markerToIdx));
        band = band.IsEmpty() ? to : band.Union(to);
    }
    return band;
}

// The active item is derived state (it depends on the window's view / panel
// mode), so the marker notices a change where that state is read: at paint.
void RailWnd::SyncActiveMarker(int activeIdx) {
    if (markerKnown && activeIdx == markerToIdx) {
        return;
    }
    if (!markerKnown) {
        // first paint: the marker is simply where it is, it didn't move there
        markerKnown = true;
        markerToIdx = activeIdx;
        markerFromIdx = -1;
        markerAnim.Set(1.0f);
        return;
    }
    markerFromIdx = markerToIdx;
    markerToIdx = activeIdx;
    markerAnim.Set(0.0f);
    markerAnim.SetTarget(1.0f, kAnimMarkerMs);
    if (markerAnim.IsAnimating()) {
        animTimer.Start();
    }
}

void RailWnd::OnPaint(HDC hdc, PAINTSTRUCT*) {
    Rect rcClient = HwndClientRect(hwnd);
    COLORREF bgCol = RailBgColor();
    // Buffer the whole rail: an animating button repaints ~60 times a second,
    // and the fill-then-draw sequence would otherwise be visible as a shimmer.
    // The buffer is client-origin, so no world transform is involved (which
    // ImageList_Draw's BitBlt would ignore), and Flush is clipped by hdc to
    // whatever rect the animation actually invalidated.
    DoubleBuffer buffer(hwnd, rcClient);
    HDC hdcOut = hdc;
    hdc = buffer.GetDC();
    HdcFillRect(hdc, rcClient, bgCol);

    // right edge, so the rail reads as its own surface next to the panel
    HdcFillRect(hdc, Rect{rcClient.dx - 1, 0, 1, rcClient.dy}, ThemeEdgeColor());

    if (!imlNormal) {
        RebuildImageLists();
    }
    Rect readingRect = ItemRect(0);
    Rect firstPanelRect = ItemRect(1);
    int readingDividerY = (readingRect.y + readingRect.dy + firstPanelRect.y) / 2;
    int dividerDx = DpiScale(hwnd, 32);
    HdcFillRect(hdc, Rect{(rcClient.dx - dividerDx) / 2, readingDividerY, dividerDx, 1}, ThemeEdgeColor());
    int firstBottom = -1;
    for (int i = 0; i < kRailItemsCount; i++) {
        if (IsRailItemVisible(gRailItems[i]) && gRailItems[i].atBottom) {
            firstBottom = i;
            break;
        }
    }
    if (firstBottom >= 0) {
        Rect openRect = ItemRect(firstBottom);
        int dividerY = openRect.y - DpiScale(hwnd, kRailBtnGap + 5);
        HdcFillRect(hdc, Rect{(rcClient.dx - dividerDx) / 2, dividerY, dividerDx, 1}, ThemeEdgeColor());
    }
    int radius = DpiScale(hwnd, kRailBtnRadius);
    int pressInset = DpiScale(hwnd, kRailPressInset);
    COLORREF emphBg = RailEmphBgColor();
    COLORREF activeBg = RailActiveBgColor();
    COLORREF activeEmphBg = RailActiveEmphBgColor();
    int activeIdx = -1;
    for (int i = 0; i < kRailItemsCount; i++) {
        const RailItem& item = gRailItems[i];
        if (!IsRailItemVisible(item)) {
            continue;
        }
        Rect r = ItemRect(i);
        bool isActive = IsRailItemActive(win, item);
        bool isEnabled = IsRailItemEnabled(win, item);
        if (isActive) {
            activeIdx = i;
        }

        float press = pressAnim[i].Value();
        // hover and press travel the same color path; an active button is
        // already tinted, so only the press reads on it
        float emphasis = isActive ? press : hoverAnim[i].Value() * kRailHoverEmphasis + press * kRailPressEmphasis;
        emphasis = limitValue(emphasis, 0.0f, 1.0f);

        HIMAGELIST iml = imlNormal;
        if (isActive) {
            iml = imlActive;
        } else if (!isEnabled) {
            iml = imlDisabled;
            emphasis = 0.0f;
        }

        if (isActive || emphasis > 0.0f) {
            COLORREF from = isActive ? activeBg : bgCol;
            COLORREF to = isActive ? activeEmphBg : emphBg;
            // a pressed button also sinks a couple of pixels, which is what
            // makes the feedback read as a push rather than a color change
            Rect fill = r;
            int inset = AnimLerpInt(0, pressInset, press);
            fill.Inflate(-inset, -inset);
            FillRoundedRect(hdc, fill, radius, AnimLerpColor(from, to, emphasis));
        }

        // The icon lists carry per-pixel alpha (only the glyph is opaque), so
        // the animated fill shows through and the icon needs no blending; the
        // list is chosen only for the color of the glyph itself.
        int ix = r.x + (r.dx - iconDy) / 2;
        int iy = r.y + (r.dy - iconDy) / 2;
        ImageList_Draw(iml, (int)item.icon, hdc, ix, iy, ILD_NORMAL);
        // the document switcher carries a count pill with the number of open PDFs
        if (item.cmdId == kRailDocumentPreview) {
            int nDocs = 0;
            for (int t = 0; win && t < win->TabCount(); t++) {
                WindowTab* tab = win->GetTab(t);
                if (tab && !tab->IsAboutTab()) {
                    nDocs++;
                }
            }
            if (nDocs > 0) {
                DrawRailCountPill(hdc, hwnd, r, nDocs);
            }
        }
    }

    // marker on the left edge of the active button, sliding from wherever it
    // last was. Drawn after the buttons so a slide reads as one continuous bar.
    SyncActiveMarker(activeIdx);
    float mt = markerAnim.Value();
    COLORREF markerCol = RailActiveFgColor();
    Rect btnFrom = markerFromIdx >= 0 ? ItemRect(markerFromIdx) : Rect{};
    Rect btnTo = markerToIdx >= 0 ? ItemRect(markerToIdx) : Rect{};
    if (!btnFrom.IsEmpty() && !btnTo.IsEmpty()) {
        Rect m = RailMarkerRect(hwnd, btnTo);
        m.y = AnimLerpInt(RailMarkerRect(hwnd, btnFrom).y, m.y, mt);
        HdcFillRect(hdc, m, markerCol);
    } else if (!btnTo.IsEmpty()) {
        // nothing was active: fade in rather than slide from an arbitrary place
        HdcFillRect(hdc, RailMarkerRect(hwnd, btnTo), AnimLerpColor(bgCol, markerCol, mt));
    } else if (!btnFrom.IsEmpty() && mt < 1.0f) {
        HdcFillRect(hdc, RailMarkerRect(hwnd, btnFrom), AnimLerpColor(markerCol, bgCol, mt));
    }

    buffer.Flush(hdcOut);
}

// Aims each button's hover animation at its new resting value and repaints the
// two buttons that can be affected. Only the button rects are invalidated, so a
// hover fade never repaints the whole rail.
void RailWnd::SetHot(int idx) {
    if (idx == hotIdx) {
        return;
    }
    int old = hotIdx;
    hotIdx = idx;
    if (old >= 0) {
        hoverAnim[old].SetTarget(0.0f, kAnimHoverMs);
        HwndInvalidateRect(hwnd, ItemRect(old), false);
    }
    if (idx >= 0 && IsRailItemEnabled(win, gRailItems[idx])) {
        hoverAnim[idx].SetTarget(1.0f, kAnimHoverMs);
        HwndInvalidateRect(hwnd, ItemRect(idx), false);
    } else if (idx >= 0) {
        hoverAnim[idx].Set(0.0f);
    }
    animTimer.Start();
}

// The press animation is purely visual feedback: the click itself is handled on
// WM_LBUTTONUP as before, so nothing waits on this.
void RailWnd::SetPressed(int idx) {
    if (idx == pressedIdx) {
        return;
    }
    int old = pressedIdx;
    pressedIdx = idx;
    if (old >= 0) {
        pressAnim[old].SetTarget(0.0f, kAnimPressReleaseMs);
        HwndInvalidateRect(hwnd, ItemRect(old), false);
    }
    if (idx >= 0) {
        pressAnim[idx].SetTarget(1.0f, kAnimPressMs);
        HwndInvalidateRect(hwnd, ItemRect(idx), false);
    }
    animTimer.Start();
}

// One frame. Invalidates only what is in motion and stops the timer the moment
// nothing is, so an idle rail costs nothing.
void RailWnd::OnAnimTick() {
    bool anyRunning = false;
    for (int i = 0; i < kRailItemsCount; i++) {
        bool running = hoverAnim[i].IsAnimating() || pressAnim[i].IsAnimating();
        if (running || animDirty[i]) {
            HwndInvalidateRect(hwnd, ItemRect(i), false);
        }
        animDirty[i] = running;
        anyRunning |= running;
    }
    bool markerRunning = markerAnim.IsAnimating();
    if (markerRunning || markerDirty) {
        HwndInvalidateRect(hwnd, MarkerBandRect(), false);
    }
    markerDirty = markerRunning;
    anyRunning |= markerRunning;
    if (!anyRunning) {
        animTimer.Stop();
    }
}

LRESULT RailWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }

    if (msg == WM_TIMER && wparam == kRailAnimTimerId) {
        OnAnimTick();
        return 0;
    }

    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ItemFromPoint(pt);
        if (idx != hotIdx) {
            SetHot(idx);
            // hovering the switcher previews the open documents after a short
            // delay, the way the taskbar does; moving off cancels it
            KillTimer(hwnd, kRailHoverTimerId);
            if (idx >= 0 && gRailItems[idx].cmdId == kRailDocumentPreview) {
                SetTimer(hwnd, kRailHoverTimerId, kRailHoverDelayMs, nullptr);
            } else {
                HoverTouchDocumentPreview(win, hwnd, Rect{}, false);
            }
        }
        if (!trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            trackingMouse = true;
        }
        return 0;
    }

    if (msg == WM_MOUSELEAVE) {
        trackingMouse = false;
        KillTimer(hwnd, kRailHoverTimerId);
        SetHot(-1);
        // the rail takes no capture, so a press dragged off it ends here
        SetPressed(-1);
        HoverTouchDocumentPreview(win, hwnd, Rect{}, false);
        return 0;
    }

    // note: no return - WM_LBUTTONDOWN was never handled here, and the press
    // animation is feedback only, so let the default handling stand
    if (msg == WM_LBUTTONDOWN) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ItemFromPoint(pt);
        if (idx >= 0 && IsRailItemEnabled(win, gRailItems[idx])) {
            SetPressed(idx);
        }
    }

    if (msg == WM_TIMER && wparam == kRailHoverTimerId) {
        KillTimer(hwnd, kRailHoverTimerId);
        if (hotIdx >= 0 && gRailItems[hotIdx].cmdId == kRailDocumentPreview) {
            HoverTouchDocumentPreview(win, hwnd, ItemRect(hotIdx), true);
        }
        return 0;
    }

    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ItemFromPoint(pt);
        // the button eases back out while the command below already runs
        SetPressed(-1);
        // Closing the overlays first would hide the preview, so the switcher
        // could never see it as open and always re-opened it instead of
        // toggling. Leave it alone when the click IS the switcher.
        bool isPreviewItem = idx >= 0 && gRailItems[idx].cmdId == kRailDocumentPreview;
        if (!isPreviewItem) {
            CloseTouchDocumentOverlays(win);
        }
        if (idx >= 0) {
            const RailItem& item = gRailItems[idx];
            if (IsRailItemEnabled(win, item)) {
                if (item.readingMode) {
                    if (win->touchView != TouchView::Doc) {
                        SetTouchView(win, TouchView::Doc);
                        SetTouchSidebarCollapsed(win, true);
                    } else {
                        SetTouchSidebarCollapsed(win, !win->touchSidebarCollapsed);
                    }
                } else if (item.view == TouchView::Doc) {
                    if (item.cmdId == kRailDocumentPreview) {
                        ShowTouchDocumentPreview(win, hwnd, ItemRect(idx));
                    } else if (item.cmdId) {
                        HwndSendCommand(win->hwndFrame, item.cmdId);
                    } else if (IsRailItemActive(win, item)) {
                        SetTouchSidebarCollapsed(win, true);
                    } else {
                        SetTouchPanelMode(win, item.mode);
                    }
                } else {
                    SetTouchView(win, item.view);
                }
            }
        }
        return 0;
    }

    return WndProcDefault(hwnd, msg, wparam, lparam);
}

HWND RailWnd::Create(MainWindow* w) {
    win = w;
    CreateCustomArgs cargs;
    cargs.parent = w->hwndFrame;
    cargs.style = WS_CHILD | WS_CLIPSIBLINGS;
    cargs.visible = false;
    CreateCustom(cargs);
    if (!hwnd) {
        return nullptr;
    }
    animTimer.Init(hwnd, kRailAnimTimerId);
    RebuildImageLists();
    collapseWnd = new SidebarToggleWnd();
    if (!collapseWnd->Create(w)) {
        return nullptr;
    }
    w->hwndTouchSidebarCollapse = collapseWnd->hwnd;
    return hwnd;
}

void CreateRail(MainWindow* win) {
    if (win->railWnd) {
        return;
    }
    auto* rail = new RailWnd();
    if (!rail->Create(win)) {
        delete rail;
        return;
    }
    win->railWnd = rail;
    win->hwndRail = rail->hwnd;
}

bool IsRailVisible(MainWindow* win) {
    if (!win || !win->hwndRail) {
        return false;
    }
    // the rail is one of the two touch-chrome directions, so it needs the
    // touch chrome on as well as its own switch
    if (!IsTouchChrome(win)) {
        return false;
    }
    // fullscreen is a clean, distraction-free page view: hide the rail (the top
    // bar hides itself the same way) even though the touch sidebar stays.
    if (win->isFullScreen) {
        return false;
    }
    return gGlobalPrefs->showRail;
}

int GetRailDx(MainWindow* win) {
    if (!IsRailVisible(win)) {
        return 0;
    }
    return DpiScale(win->hwndFrame, kRailDx);
}

void DestroyRail(MainWindow* win) {
    if (!win || !win->railWnd) {
        return;
    }
    win->hwndTouchSidebarCollapse = nullptr;
    delete win->railWnd;
    win->railWnd = nullptr;
    win->hwndRail = nullptr;
}

void UpdateRailForWindow(MainWindow* win) {
    if (!win || !win->hwndRail) {
        return;
    }
    HwndInvalidate(win->hwndRail, false);
}

void UpdateRailAfterThemeChange(MainWindow* win) {
    if (!win || !win->railWnd) {
        return;
    }
    win->railWnd->RebuildImageLists();
    HwndInvalidate(win->hwndRail, true);
}
