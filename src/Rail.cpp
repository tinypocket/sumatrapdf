/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Dpi.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

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

static RailItem gRailItems[] = {
    {TbIcon::Document, TouchPanelMode::Bookmarks, TouchView::Doc, 0, false, true},
    {TbIcon::HomeList, TouchPanelMode::Bookmarks, TouchView::Doc, 0, false, false},
    {TbIcon::HomeThumbnails, TouchPanelMode::Thumbnails, TouchView::Doc, 0, false, false},
    {TbIcon::Search, TouchPanelMode::Search, TouchView::Doc, 0, false, false},
    {TbIcon::Annotation, TouchPanelMode::Annotations, TouchView::Doc, 0, false, false},
    {TbIcon::Attachment, TouchPanelMode::Attachments, TouchView::Doc, 0, false, false},
    {TbIcon::WindowStack, TouchPanelMode::Bookmarks, TouchView::Doc, kRailDocumentPreview, true, false, true},
    {TbIcon::Recent, TouchPanelMode::Bookmarks, TouchView::Home, 0, true, false},
    {TbIcon::Library, TouchPanelMode::Bookmarks, TouchView::Library, 0, true, false},
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

    MainWindow* win = nullptr;
    // icons in the two colors a rail button can have. Rebuilt on theme change.
    HIMAGELIST imlNormal = nullptr;
    HIMAGELIST imlActive = nullptr;
    int iconDy = 0;
    int hotIdx = -1;
    bool trackingMouse = false;
    SidebarToggleWnd* collapseWnd = nullptr;
};

RailWnd::RailWnd() {
    kind = kindRail;
}

RailWnd::~RailWnd() {
    delete collapseWnd;
    if (imlNormal) {
        ImageList_Destroy(imlNormal);
    }
    if (imlActive) {
        ImageList_Destroy(imlActive);
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

void RailWnd::RebuildImageLists() {
    if (imlNormal) {
        ImageList_Destroy(imlNormal);
        imlNormal = nullptr;
    }
    if (imlActive) {
        ImageList_Destroy(imlActive);
        imlActive = nullptr;
    }
    iconDy = DpiScale(hwnd, kTopBarIconDy);
    imlNormal = BuildTintedToolbarImageList(iconDy, RailFgColor(), RailBgColor());
    imlActive = BuildTintedToolbarImageList(iconDy, RailActiveFgColor(), RailActiveBgColor());
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

static bool IsRailItemEnabled(MainWindow* win, const RailItem& item) {
    if (!win) {
        return false;
    }
    return item.cmdId || item.view != TouchView::Doc || win->HasDocsLoaded();
}

void SetTouchSidebarCollapsed(MainWindow* win, bool collapsed) {
    if (!win || !IsTouchChrome(win) || win->touchView != TouchView::Doc || !win->HasDocsLoaded()) {
        return;
    }
    win->touchSidebarCollapsed = collapsed;
    win->uiState.tocVisible = !collapsed;
    win->uiState.favVisible = false;
    UpdateRailForWindow(win);
    ScheduleUiUpdate(win, kUiForceRelayout | kUiSidebarDirty | kUiToolbarDirty);
}

static void SetTouchHomeTabLabel(MainWindow* win, TouchView view) {
    Str label = view == TouchView::Library ? StrL("Library") : StrL("Home");
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
    TouchView oldView = win->touchView;
    if (oldView != view && (oldView == TouchView::Home || oldView == TouchView::Library)) {
        HomePageDestroySearch(win);
    }
    if (view == TouchView::Doc) {
        TouchView previous = win->touchView;
        win->touchView = TouchView::Doc;
        if (!SelectTouchDocumentTab(win)) {
            win->touchView = previous;
            return;
        }
        win->uiState.tocVisible = true;
        win->touchSidebarCollapsed = false;
    } else {
        win->touchView = view;
        if (!SelectTouchHomeTab(win)) {
            return;
        }
        win->uiState.tocVisible = false;
        win->uiState.favVisible = false;
    }
    win->touchView = view;
    if (view == TouchView::Home || view == TouchView::Library) {
        SetTouchHomeTabLabel(win, view);
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
    if (!SelectTouchDocumentTab(win)) {
        win->touchView = previous;
        return;
    }
    SetTouchPanelModeAndRestoreSearch(win, mode);
    win->touchSidebarCollapsed = false;
    win->touchPanelScrollY = 0;
    win->uiState.tocVisible = true;
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
    win->touchSidebarCollapsed = false;
    TabsSelect(win, tabIndex);
    win->uiState.tocVisible = true;
    win->uiState.favVisible = false;
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

void RailWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    Rect rcClient = HwndClientRect(hwnd);
    COLORREF bgCol = RailBgColor();
    HdcFillRect(hdc, ToRect(ps->rcPaint), bgCol);

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
    for (int i = 0; i < kRailItemsCount; i++) {
        const RailItem& item = gRailItems[i];
        if (!IsRailItemVisible(item)) {
            continue;
        }
        Rect r = ItemRect(i);
        bool isActive = IsRailItemActive(win, item);
        bool isEnabled = IsRailItemEnabled(win, item);

        COLORREF btnBg = bgCol;
        if (isActive) {
            btnBg = RailActiveBgColor();
        } else if (i == hotIdx && isEnabled) {
            // a step from the rail's own background: the theme's hot color is
            // now what the rail is painted with, so it would be invisible
            btnBg = AccentColor(bgCol, 10);
        }
        if (btnBg != bgCol) {
            FillRoundedRect(hdc, r, radius, btnBg);
        }

        // marker on the left edge of the active button
        if (isActive) {
            int markerDx = DpiScale(hwnd, kRailMarkerDx);
            int markerDy = DpiScale(hwnd, kRailMarkerDy);
            Rect rMarker{0, r.y + (r.dy - markerDy) / 2, markerDx, markerDy};
            HdcFillRect(hdc, rMarker, RailActiveFgColor());
        }

        // the icons are opaque, so an icon on the active button has to come
        // from the image list built against that button's background
        HIMAGELIST iml = isActive ? imlActive : imlNormal;
        int ix = r.x + (r.dx - iconDy) / 2;
        int iy = r.y + (r.dy - iconDy) / 2;
        ImageList_Draw(iml, (int)item.icon, hdc, ix, iy, ILD_NORMAL);
    }
}

LRESULT RailWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }

    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ItemFromPoint(pt);
        if (idx != hotIdx) {
            hotIdx = idx;
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
        if (hotIdx != -1) {
            hotIdx = -1;
            HwndInvalidate(hwnd, false);
        }
        return 0;
    }

    if (msg == WM_LBUTTONUP) {
        CloseTouchDocumentOverlays(win);
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ItemFromPoint(pt);
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
