/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Dpi.h"
#include "base/File.h"
#include "base/ScopedWin.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/Anim.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "TouchMetrics.h"
#include "DocController.h"
#include "DisplayMode.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "FileHistory.h"
#include "FileThumbnails.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Menu.h"
#include "TrimDialog.h"
#include "Commands.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "Rail.h"
#include "Tabs.h"
#include "Favorites.h"
#include "TopBar.h"
#include "Theme.h"
#include "Translations.h"

static Kind kindTopBar = "topBar";
static Kind kindTouchPreview = "touchPreview";

struct TopBarWnd;

struct PreviewCard {
    Rect rect;
    Rect closeRect;
    MainWindow* win = nullptr;
    int tabIdx = -1;
    Str filePath;
    int pageNo = 1;
    RenderedBitmap* thumbnail = nullptr;
};

struct TouchPreviewWnd : Wnd {
    TouchPreviewWnd();
    ~TouchPreviewWnd() override;
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;
    bool Create(TopBarWnd*);
    void Show(HWND anchorHwnd, Rect anchorRect);
    void Hide();
    void BuildLayout();
    void FreeCards();

    TopBarWnd* owner = nullptr;
    Vec<PreviewCard> cards;
    u64 generation = 0;
    bool trackingMouse = false;
};

struct TouchBookmarkConfirmWnd : Wnd {
    TouchBookmarkConfirmWnd();
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;
    bool Create(TopBarWnd*);
    void Show(int pageNo, Rect anchorRect);
    void Hide();

    TopBarWnd* owner = nullptr;
    int pageNo = 0;
    Rect cancelRect;
    Rect removeRect;
};

// What a slot draws. This is the "scaled" direction's toolbar (2a in the
// redesign): the controls a Sumatra user expects, in the same order, grouped
// onto rounded tracks with finger-sized buttons.
enum class TopBarItem {
    Button,    // plain icon button
    Preview,   // 40px open-documents button
    Bookmark,  // dashed save-current-page button
    Overflow,  // decorative 44px more button
    PageBox,   // "1 / 613": current page bold, the total muted
    ZoomLabel, // "88%"
};

struct TopBarSlot {
    TopBarItem item;
    TbIcon icon; // TbIcon::None for text slots
    int cmdId;   // 0 when the slot isn't clickable
    // slots sharing an index are drawn on one rounded track
    int group;
    // right-aligned groups are laid out from the right edge inwards
    bool alignRight;
    // the rail direction sheds everything the rail or the page already
    // offers, so these appear only in the scaled direction (4a vs 2a)
    bool scaledOnly;
};

constexpr int kTopBarPreview = -100;
constexpr int kTopBarPageEdit = -101;
constexpr int kTopBarBookmark = -102;
constexpr int kTopBarZoomEdit = -103;
constexpr int kTopBarSmartWidth = -104;
constexpr int kTopBarOverflow = -105;

static TopBarSlot gTopBarSlots[] = {
    // The open-documents switcher lives in the tab bar and the rail; a third
    // copy in the document toolbar was redundant.
    {TopBarItem::Button, TbIcon::SearchPrev, CmdGoToPrevPage, 1, false, false},
    {TopBarItem::PageBox, TbIcon::None, kTopBarPageEdit, 1, false, false},
    {TopBarItem::Button, TbIcon::SearchNext, CmdGoToNextPage, 1, false, false},
    {TopBarItem::Bookmark, TbIcon::Bookmark, kTopBarBookmark, 2, false, false},

    {TopBarItem::Button, TbIcon::ZoomOut, CmdZoomOut, 3, true, false},
    {TopBarItem::ZoomLabel, TbIcon::None, kTopBarZoomEdit, 3, true, false},
    {TopBarItem::Button, TbIcon::ZoomIn, CmdZoomIn, 3, true, false},
    {TopBarItem::Button, TbIcon::SmartWidth, kTopBarSmartWidth, 4, true, false},
    {TopBarItem::Button, TbIcon::RotateRight, CmdRotateRight, 4, true, false},
    // dark-mode toggles: Contrast = dark mode on the document pages,
    // Moon = light/dark theme for the app chrome (kept in both 2a and 4a)
    {TopBarItem::Button, TbIcon::Contrast, CmdInvertColors, 5, true, false},
    {TopBarItem::Button, TbIcon::Moon, CmdToggleLightDarkTheme, 5, true, false},
    // was decorative (cmdId 0 is skipped by SlotFromPoint); now opens a
    // small menu for the view options that have no room of their own
    {TopBarItem::Overflow, TbIcon::Settings, kTopBarOverflow, 6, true, false},
};

constexpr int kTopBarSlotCount = (int)dimof(gTopBarSlots);

// how far along a slot's normal -> pressed color path a plain hover sits
constexpr float kTopBarHoverEmphasis = 0.55f;
constexpr float kTopBarPressEmphasis = 1.0f - kTopBarHoverEmphasis;
constexpr int kTopBarPressInset = 2;

// slots drawn as a button, i.e. the ones that carry a hover / press wash
static bool SlotHasWash(const TopBarSlot& slot) {
    switch (slot.item) {
        case TopBarItem::Preview:
        case TopBarItem::Overflow:
        case TopBarItem::Button:
            return true;
        default:
            return false;
    }
}

struct TopBarWnd : Wnd {
    TopBarWnd();
    ~TopBarWnd() override;

    HWND Create(MainWindow*);
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) override;

    void RebuildImageList();
    // fills rects[] for every slot; returns false when the bar is too narrow
    bool Layout(HDC hdc, Rect* rects);
    int SlotFromPoint(Point pt);

    void SetHot(int idx);
    void SetPressed(int idx);
    void OnAnimTick();

    MainWindow* win = nullptr;
    HIMAGELIST iml = nullptr;
    HIMAGELIST imlPaper = nullptr;
    int iconDy = 0;
    int hotIdx = -1;
    bool trackingMouse = false;

    AnimTimer animTimer;
    AnimVal hoverAnim[kTopBarSlotCount];
    AnimVal pressAnim[kTopBarSlotCount];
    // a slot that just finished still owes one frame in its settled state
    bool animDirty[kTopBarSlotCount]{};
    int pressedIdx = -1;
    // last laid-out rects, so hit testing doesn't have to re-measure text
    Rect slotRects[kTopBarSlotCount]{};
    Rect nameRect{};
    Rect savedTrayRect{};
    Vec<Rect> savedPageRects;
    Vec<Rect> savedCloseRects;
    Vec<int> savedPages;
    StrVec savedPageNames;
    // With favorites in more than one document the tray shows a chip per
    // document instead of a flat list of pages: a mixed list gives no clue
    // which page belongs to which file. Each chip is coloured from its path so
    // the same document keeps the same colour between sessions.
    StrVec savedGroupPaths;
    Vec<Rect> savedGroupRects;
    int savedContentDx = 0;
    int savedScrollX = 0;
    bool savedTrayDragging = false;
    bool savedTrayDidDrag = false;
    int savedTrayDragX = 0;
    int savedTrayDragScrollX = 0;
    bool pageEditing = false;
    bool zoomEditing = false;
    bool editSelectAll = false;
    char editText[16]{};
    TouchPreviewWnd* previewWnd = nullptr;
    bool previewPinned = false;
    TouchBookmarkConfirmWnd* bookmarkConfirmWnd = nullptr;
    int savedHoldingIdx = -1;
    u64 savedHoldStarted = 0;
    bool savedHoldCompleted = false;
    int savedBodyHoldingIdx = -1;
    u64 savedBodyHoldStarted = 0;
    bool savedBodyHoldTriggered = false;
    int savedRenamingIdx = -1;
    bool savedRenameSelectAll = false;
    WCHAR savedRenameText[128]{};

    void BeginEdit(bool page);
    void CommitEdit();
    void CancelEdit();
    void AddCurrentPageBookmark();
    void ShowOverflowMenu(const Rect& anchor);
    // rebuilds savedPages/savedPageNames from the document's stored favorites
    void RefreshSavedPages();
    Rect PreviewAnchorRect(const Rect& slotRect);
    void ShowPreview(HWND anchorHwnd, Rect anchorRect);
    // a hover from the tab bar waits out the same delay the rail uses before
    // the preview appears; these hold what to show when it fires
    HWND pendingPreviewAnchor = nullptr;
    Rect pendingPreviewRect;
    void PinPreview(HWND anchorHwnd, Rect anchorRect);
    void SchedulePreviewClose();
    void CancelPreviewClose();
    void ShowBookmarkConfirm(int idx);
    void RemoveSavedPage(int pageNo);
    TempStr SavedPageLabelTemp(int idx);
    void BeginSavedPageRename(int idx);
    void CommitSavedPageRename();
    void CancelSavedPageRename();
    void CloseOverlays(bool commitEdits);
};

TopBarWnd::TopBarWnd() {
    kind = kindTopBar;
}

TopBarWnd::~TopBarWnd() {
    // before ~Wnd tears the window down, so no timer outlives the object
    animTimer.Stop();
    delete previewWnd;
    delete bookmarkConfirmWnd;
    if (iml) {
        ImageList_Destroy(iml);
    }
    if (imlPaper) {
        ImageList_Destroy(imlPaper);
    }
}

constexpr UINT_PTR kPreviewCloseTimerId = 0x51A2;
// hovering the switcher opens the strip after the same delay as the rail
constexpr UINT_PTR kPreviewHoverTimerId = 0x51A3;
constexpr int kPreviewHoverDelayMs = 450;
constexpr UINT_PTR kSavedPageHoldTimerId = 0x51A3;
constexpr UINT_PTR kSavedPageRenameTimerId = 0x51A4;
// drives the hover / press transitions; only runs while one is in flight
constexpr UINT_PTR kTopBarAnimTimerId = 0x51A5;

void TopBarWnd::CancelPreviewClose() {
    KillTimer(hwnd, kPreviewCloseTimerId);
}

void TopBarWnd::SchedulePreviewClose() {
    if (previewPinned) {
        return;
    }
    if (previewWnd && HwndIsVisible(previewWnd->hwnd)) {
        SetTimer(hwnd, kPreviewCloseTimerId, 300, nullptr);
    }
}

// The switcher lives in the top bar but its cards correspond to the tabs
// above it, so drop the strip left-aligned with the tab strip: card i then
// sits under tab i, instead of under the button.
Rect TopBarWnd::PreviewAnchorRect(const Rect& slotRect) {
    if (win && win->tabsCtrl && win->tabsCtrl->hwnd && HwndIsVisible(win->tabsCtrl->hwnd)) {
        Rect tabs = HwndWindowRect(win->tabsCtrl->hwnd);
        POINT tl{tabs.x, tabs.y};
        ScreenToClient(hwnd, &tl);
        Rect r = slotRect;
        r.x = tl.x;
        return r;
    }
    return slotRect;
}

void TopBarWnd::ShowPreview(HWND anchorHwnd, Rect anchorRect) {
    CancelPreviewClose();
    if (previewWnd && HwndIsVisible(previewWnd->hwnd)) {
        return;
    }
    if (!previewWnd) {
        previewWnd = new TouchPreviewWnd();
        if (!previewWnd->Create(this)) {
            delete previewWnd;
            previewWnd = nullptr;
            return;
        }
    }
    previewWnd->Show(anchorHwnd, anchorRect);
}

void TopBarWnd::PinPreview(HWND anchorHwnd, Rect anchorRect) {
    // tapping the switcher again closes it
    if (previewPinned && previewWnd && HwndIsVisible(previewWnd->hwnd)) {
        previewPinned = false;
        CancelPreviewClose();
        previewWnd->Hide();
        return;
    }
    previewPinned = true;
    ShowPreview(anchorHwnd, anchorRect);
    if (!previewWnd || !HwndIsVisible(previewWnd->hwnd)) {
        previewPinned = false;
    }
}

// The tray is a view onto the document's favorites, not its own list: these
// two vectors are a cache rebuilt from the store, so a page saved here shows up
// in the Favorites pane and survives a restart.
// A document's colour in the tray: picked from its path, so it is stable
// across sessions and the same file always reads the same.
COLORREF TouchFavoriteGroupColor(Str filePath) {
    // Dark enough that white text (always used on this palette, see the two
    // draw sites below) stays readable on every entry - the original palette
    // had three colors on the light side of IsLightColor's midpoint, which is
    // how a document's chip ended up with black text next to another's white.
    static const COLORREF kPalette[] = {
        RGB(0xB8, 0x5C, 0x40), RGB(0x3F, 0x68, 0xAE), RGB(0x3C, 0x82, 0x57),
        RGB(0x80, 0x54, 0x9E), RGB(0x8E, 0x69, 0x26), RGB(0x2C, 0x83, 0x83),
    };
    u32 h = 2166136261u;
    for (int i = 0; i < filePath.len; i++) {
        h = (h ^ (u8)filePath.s[i]) * 16777619u;
    }
    return kPalette[h % dimof(kPalette)];
}

void TopBarWnd::RefreshSavedPages() {
    // an in-flight rename holds an index into these vectors; rebuilding under
    // it would retarget the edit at a different favorite
    if (savedRenamingIdx >= 0) {
        return;
    }
    savedPages.Reset();
    savedPageNames.Reset();
    savedGroupPaths.Reset();
    if (!gGlobalPrefs->favoritesInToolbar) {
        return;
    }

    Vec<FileState*> files;
    GetFilesWithFavorites(files);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    Str path = (tab && !tab->IsAboutTab()) ? tab->filePath : Str{};

    // More than one document with favorites: chips, one per document. The
    // current document's own pages still show directly, since those are the
    // ones being used right now.
    int others = 0;
    for (FileState* fs : files) {
        if (!path || !str::Eq(fs->filePath, path)) {
            others++;
        }
    }
    if (others > 0) {
        for (FileState* fs : files) {
            if (path && str::Eq(fs->filePath, path)) {
                continue; // shown as pages below
            }
            savedGroupPaths.Append(fs->filePath);
        }
    }

    Vec<Favorite*>* favs = GetFileFavorites(path);
    if (!favs) {
        return;
    }
    for (Favorite* f : *favs) {
        if (!f || f->isTemporary) {
            continue;
        }
        savedPages.Append(f->pageNo);
        savedPageNames.Append(f->name ? f->name : StrL(""));
    }
}

void TopBarWnd::RemoveSavedPage(int pageNo) {
    int idx = savedPages.Find(pageNo);
    if (idx < 0) {
        return;
    }
    if (savedRenamingIdx == idx) {
        CancelSavedPageRename();
    } else if (savedRenamingIdx > idx) {
        savedRenamingIdx--;
    }
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (tab && tab->filePath) {
        DelFavorite(tab->filePath, pageNo);
    }
    RefreshSavedPages();
    HwndInvalidate(hwnd, false);
}

TempStr TopBarWnd::SavedPageLabelTemp(int idx) {
    if (idx >= 0 && idx < len(savedPageNames) && len(savedPageNames[idx]) > 0) {
        return str::DupTemp(savedPageNames[idx]);
    }
    if (idx < 0 || idx >= len(savedPages)) {
        return str::DupTemp("");
    }
    return fmt("p. %d", savedPages[idx]);
}

void TopBarWnd::BeginSavedPageRename(int idx) {
    if (idx < 0 || idx >= len(savedPages)) {
        return;
    }
    savedRenamingIdx = idx;
    savedRenameSelectAll = true;
    TempWStr label = ToWStrTemp(SavedPageLabelTemp(idx));
    wcsncpy_s(savedRenameText, label.s, _TRUNCATE);
    HwndSetFocus(hwnd);
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::CommitSavedPageRename() {
    if (savedRenamingIdx < 0 || savedRenamingIdx >= len(savedPages)) {
        CancelSavedPageRename();
        return;
    }
    TempStr utf8 = ToUtf8Temp(WStr(savedRenameText));
    Str name = utf8;
    str::TrimWSInPlace(name, str::TrimOpt::Both);
    TempStr defaultLabel = fmt("p. %d", savedPages[savedRenamingIdx]);
    if (len(name) == 0 || str::Eq(name, defaultLabel)) {
        name = StrL("");
    }
    savedPageNames.SetAt(savedRenamingIdx, name);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (tab && tab->filePath && savedRenamingIdx < len(savedPages)) {
        RenameFavorite(tab->filePath, savedPages[savedRenamingIdx], name);
    }
    savedRenamingIdx = -1;
    savedRenameSelectAll = false;
    savedRenameText[0] = 0;
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::CancelSavedPageRename() {
    savedRenamingIdx = -1;
    savedRenameSelectAll = false;
    savedRenameText[0] = 0;
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::CloseOverlays(bool commitEdits) {
    CancelPreviewClose();
    if (previewWnd) {
        previewWnd->Hide();
    }
    if (bookmarkConfirmWnd) {
        bookmarkConfirmWnd->Hide();
    }
    if (pageEditing || zoomEditing) {
        if (commitEdits) {
            CommitEdit();
        } else {
            CancelEdit();
        }
    }
    if (savedRenamingIdx >= 0) {
        if (commitEdits) {
            CommitSavedPageRename();
        } else {
            CancelSavedPageRename();
        }
    }
}

void TopBarWnd::ShowBookmarkConfirm(int idx) {
    if (idx < 0 || idx >= len(savedPages) || idx >= len(savedPageRects)) {
        return;
    }
    if (!bookmarkConfirmWnd) {
        bookmarkConfirmWnd = new TouchBookmarkConfirmWnd();
        if (!bookmarkConfirmWnd->Create(this)) {
            delete bookmarkConfirmWnd;
            bookmarkConfirmWnd = nullptr;
            return;
        }
    }
    bookmarkConfirmWnd->Show(savedPages[idx], savedPageRects[idx]);
}

static COLORREF TopBarBgColor() {
    return ThemeControlBackgroundColor();
}

// the track a group of controls sits on
static COLORREF TopBarGroupColor() {
    return ThemeHotBackgroundColor();
}

// The far end of a button's hover / press path. A plain hover travels only
// kTopBarHoverEmphasis of the way here, which lands on the step the
// un-animated hover used to paint, so nothing about the resting design moved.
static COLORREF TopBarEmphColor() {
    return AccentColor(TopBarGroupColor(), 18);
}

void TopBarWnd::RebuildImageList() {
    if (iml) {
        ImageList_Destroy(iml);
        iml = nullptr;
    }
    if (imlPaper) {
        ImageList_Destroy(imlPaper);
        imlPaper = nullptr;
    }
    iconDy = DpiScale(hwnd, kTopBarIconDy);
    iml = BuildTintedToolbarImageList(iconDy, ThemeWindowTextColor(), TopBarGroupColor());
    imlPaper = BuildTintedToolbarImageList(iconDy, ThemeWindowDarkerTextColor(), TopBarBgColor());
}

static Size HdcGetTextExtentPoint32Font(HDC hdc, Str s, HFONT font) {
    ScopedSelectObject sel(hdc, font);
    return HdcGetTextExtentPoint32(hdc, s);
}

static TempStr CurPageTextTemp(MainWindow* win) {
    DocController* ctrl = win ? win->ctrl : nullptr;
    if (!ctrl) {
        return str::DupTemp("");
    }
    return fmt("%d", ctrl->CurrentPageNo());
}

static TempStr TotalPagesTextTemp(MainWindow* win) {
    DocController* ctrl = win ? win->ctrl : nullptr;
    if (!ctrl) {
        return str::DupTemp("");
    }
    return fmt("/ %d", ctrl->PageCount());
}

static TempStr PageTextTemp(MainWindow* win) {
    DocController* ctrl = win ? win->ctrl : nullptr;
    if (!ctrl) {
        return str::DupTemp("");
    }
    TempStr cur = fmt("%d", ctrl->CurrentPageNo());
    return fmt("%s / %d", cur, ctrl->PageCount());
}

static TempStr ZoomTextTemp(MainWindow* win) {
    DocController* ctrl = win ? win->ctrl : nullptr;
    if (!ctrl) {
        return str::DupTemp("");
    }
    if (ctrl->GetZoomVirtual() == kZoomSmartWidth) {
        return str::DupTemp("Smart Width");
    }
    float zoom = ctrl->GetZoomVirtual(true);
    return fmt("%d%%", (int)(zoom + 0.5f));
}

// the view-mode buttons show which mode the document is in, the way the
// redesign tints the active one
static bool IsSlotActive(MainWindow* win, const TopBarSlot& slot) {
    // dark-mode toggles reflect global/theme state, not the document controller
    switch (slot.cmdId) {
        case CmdToggleLightDarkTheme:
            return !IsLightColor(ThemeWindowBackgroundColor());
        case CmdInvertColors:
            return GetInvertPageColors();
    }
    DocController* ctrl = win ? win->ctrl : nullptr;
    if (!ctrl) {
        return false;
    }
    DisplayMode mode = ctrl->GetDisplayMode();
    switch (slot.cmdId) {
        case kTopBarSmartWidth:
            return ctrl->GetZoomVirtual() == kZoomSmartWidth;
        case CmdSinglePageView:
            return IsSingle(mode);
        case CmdToggleContinuousView:
            return IsContinuous(mode);
    }
    return false;
}

static HFONT TopBarFont(HWND hwnd) {
    return GetAppFont(hwnd);
}

// the document name and the readouts carry weight, per the design
static HFONT TopBarFontWeighted(HDC hdc, int size, int weight) {
    return HdcGetUiFont(hdc, size, weight);
}

bool TopBarWnd::Layout(HDC hdc, Rect* rects) {
    Rect rc = HwndClientRect(hwnd);
    nameRect = {};
    savedTrayRect = {};
    savedPageRects.Reset();
    savedCloseRects.Reset();
    savedContentDx = 0;
    for (int i = 0; i < kTopBarSlotCount; i++) {
        rects[i] = Rect{};
    }
    if (rc.dx <= 0 || rc.dy <= 0) {
        return false;
    }

    int btn = DpiScale(hwnd, kTopBarBtnDy);
    int pad = DpiScale(hwnd, 16);
    int groupGap = DpiScale(hwnd, 12);

    // measure with the font the readouts are actually painted with, or the
    // page box comes out sized for a different typeface
    HFONT font = TopBarFontWeighted(hdc, kFontSizeBody, kFontWeightMedium);
    ScopedSelectObject selFont(hdc, font);

    // text slots decide their group's width, so measure them first
    Size pageSz = HdcGetTextExtentPoint32(hdc, PageTextTemp(win));
    int pageBoxDx = std::max(pageSz.dx + DpiScale(hwnd, 24), DpiScale(hwnd, kPageBoxDx));
    Size zoomSz = HdcGetTextExtentPoint32(hdc, ZoomTextTemp(win));
    int zoomDx = std::max(DpiScale(hwnd, kZoomLabelDx), zoomSz.dx + DpiScale(hwnd, 24));

    // without a document the readouts would be empty pills; only the settings
    // button makes sense on the Home page
    bool hasDoc = win && win->IsDocLoaded();

    bool railMode = IsRailVisible(win);

    bool visible[kTopBarSlotCount]{};
    for (int i = 0; i < kTopBarSlotCount; i++) {
        visible[i] = hasDoc;
        const TopBarSlot& slot = gTopBarSlots[i];
        if (slot.item == TopBarItem::Preview) {
            visible[i] = hasDoc && !SettingsUseTabs();
        } else if (slot.cmdId == CmdGoToPrevPage || slot.cmdId == CmdGoToNextPage) {
            visible[i] = hasDoc && pageEditing;
        } else if (slot.cmdId == CmdZoomOut || slot.cmdId == CmdZoomIn) {
            visible[i] = hasDoc && zoomEditing;
        }
    }

    auto slotDx = [&](const TopBarSlot& s) {
        if (s.item == TopBarItem::PageBox) {
            return pageBoxDx;
        }
        if (s.item == TopBarItem::ZoomLabel) {
            return zoomDx;
        }
        if (s.item == TopBarItem::Preview || s.item == TopBarItem::Bookmark) {
            return DpiScale(hwnd, 40);
        }
        return btn;
    };

    auto slotDy = [&](const TopBarSlot& s) {
        if (s.item == TopBarItem::Preview || s.item == TopBarItem::Bookmark) {
            return DpiScale(hwnd, 40);
        }
        return btn;
    };

    auto runPad = [&](int group) { return (group == 1) ? DpiScale(hwnd, 3) : ((group == 3) ? DpiScale(hwnd, 4) : 0); };

    auto runInnerGap = [&](int group) {
        return (group == 1) ? DpiScale(hwnd, 2) : ((group == 3) ? DpiScale(hwnd, 4) : 0);
    };

    // collect the groups first: right-aligned ones have to be placed back to
    // front so they still read left to right on screen
    struct GroupRun {
        int first, last, dx;
        bool right;
    };
    GroupRun runs[kTopBarSlotCount]{};
    int nRuns = 0;
    for (int i = 0; i < kTopBarSlotCount;) {
        int g = gTopBarSlots[i].group;
        int j = i;
        int groupDx = 2 * runPad(g);
        int nVisible = 0;
        while (j < kTopBarSlotCount && gTopBarSlots[j].group == g) {
            if (visible[j]) {
                if (nVisible > 0) {
                    groupDx += runInnerGap(g);
                }
                groupDx += slotDx(gTopBarSlots[j]);
                nVisible++;
            }
            j++;
        }
        // without a document only the settings button is meaningful
        bool skip = !hasDoc && gTopBarSlots[i].cmdId != CmdOptions;
        // the rail direction sheds the groups the rail already offers
        if (railMode && gTopBarSlots[i].scaledOnly) {
            skip = true;
        }
        if (!skip && nVisible > 0) {
            runs[nRuns++] = {i, j, groupDx, gTopBarSlots[i].alignRight};
        }
        i = j;
    }

    int xLeft = pad;
    int xRight = rc.dx - pad;
    auto place = [&](const GroupRun& run, int gx) {
        int group = gTopBarSlots[run.first].group;
        gx += runPad(group);
        bool first = true;
        for (int k = run.first; k < run.last; k++) {
            if (!visible[k]) {
                continue;
            }
            if (!first) {
                gx += runInnerGap(group);
            }
            int dx = slotDx(gTopBarSlots[k]);
            int dy = slotDy(gTopBarSlots[k]);
            rects[k] = Rect{gx, (rc.dy - dy) / 2, dx, dy};
            gx += dx;
            first = false;
        }
    };

    // right-aligned groups first, back to front, so they read left to right
    for (int i = nRuns - 1; i >= 0; i--) {
        if (!runs[i].right) {
            continue;
        }
        if (xRight - runs[i].dx < xLeft) {
            break; // no room: leave this group and anything further in unplaced
        }
        xRight -= runs[i].dx;
        place(runs[i], xRight);
        xRight -= groupGap;
    }

    // 4a leads with the current document name. It uses only the width the
    // title needs, capped at 280px, and yields before the right-side controls.
    // Skipped when the tab strip is showing: the active tab already carries
    // this exact title immediately above the bar, and repeating it here read
    // as a second, redundant line rather than useful information.
    if (railMode && hasDoc && !SettingsUseTabs()) {
        Str title = win->CurrentTab()->GetTabTitle();
        HFONT titleFont = TopBarFontWeighted(hdc, kFontSizeBody, kFontWeightStrong);
        Size titleSz = HdcGetTextExtentPoint32Font(hdc, title, titleFont);
        int nameDx = std::min(titleSz.dx, DpiScale(hwnd, kTopBarNameDx));
        int nameGap = groupGap;
        if (nameDx > 0 && xLeft + nameDx + nameGap < xRight) {
            nameRect = {xLeft, 0, nameDx, rc.dy};
            xLeft += nameDx + nameGap;
        }
    }

    for (int i = 0; i < nRuns; i++) {
        if (runs[i].right) {
            continue;
        }
        // never run a left group into the right-aligned cluster: on a very
        // narrow bar (e.g. a high-dpi split window) drop it rather than overlap
        if (xLeft + runs[i].dx > xRight) {
            continue;
        }
        place(runs[i], xLeft);
        xLeft += runs[i].dx + groupGap;
    }

    // Saved-page pills follow the dashed bookmark button. The tray is capped
    // at 340px, but all pills remain laid out so it can be touch-dragged or
    // mouse-wheel scrolled instead of silently clipping later pages.
    int bookmarkIdx = -1;
    for (int i = 0; i < kTopBarSlotCount; i++) {
        if (gTopBarSlots[i].item == TopBarItem::Bookmark) {
            bookmarkIdx = i;
            break;
        }
    }
    savedGroupRects.Reset();
    if (bookmarkIdx >= 0 && !rects[bookmarkIdx].IsEmpty() && (len(savedPages) > 0 || len(savedGroupPaths) > 0)) {
        int x = rects[bookmarkIdx].x + rects[bookmarkIdx].dx + DpiScale(hwnd, 8);
        int trayRight = std::min(x + DpiScale(hwnd, 340), xRight);
        int pillDy = DpiScale(hwnd, 36);
        savedTrayRect = Rect{x, (rc.dy - pillDy) / 2, std::max(0, trayRight - x), pillDy};
        HFONT pillFont = TopBarFontWeighted(hdc, kFontSizeMeta, kFontWeightStrong);
        int gap = DpiScale(hwnd, 6);
        int contentDx = 0;
        for (int i = 0; i < len(savedPages); i++) {
            TempStr label = SavedPageLabelTemp(i);
            Size sz = HdcGetTextExtentPoint32Font(hdc, label, pillFont);
            int labelDx =
                i == savedRenamingIdx ? DpiScale(hwnd, 90) : std::min(sz.dx + DpiScale(hwnd, 6), DpiScale(hwnd, 160));
            int dx = labelDx + DpiScale(hwnd, 12 + 4 + 26);
            contentDx += dx;
            if (i + 1 < len(savedPages)) {
                contentDx += gap;
            }
        }
        // chips for the other documents, sized to their names
        HFONT chipFont = TopBarFontWeighted(hdc, kFontSizeMeta, kFontWeightStrong);
        Vec<int> groupDx;
        for (int i = 0; i < len(savedGroupPaths); i++) {
            TempStr name = path::GetBaseNameTemp(savedGroupPaths[i]);
            Size sz = HdcGetTextExtentPoint32Font(hdc, name, chipFont);
            int dx = std::min(sz.dx + DpiScale(hwnd, 34), DpiScale(hwnd, 190));
            groupDx.Append(dx);
            contentDx += (contentDx > 0 ? gap : 0) + dx;
        }

        savedContentDx = contentDx;
        int maxScroll = std::max(0, savedContentDx - savedTrayRect.dx);
        savedScrollX = std::clamp(savedScrollX, 0, maxScroll);
        int pillX = x - savedScrollX;
        for (int i = 0; i < len(savedPages); i++) {
            TempStr label = SavedPageLabelTemp(i);
            Size sz = HdcGetTextExtentPoint32Font(hdc, label, pillFont);
            int labelDx =
                i == savedRenamingIdx ? DpiScale(hwnd, 90) : std::min(sz.dx + DpiScale(hwnd, 6), DpiScale(hwnd, 160));
            int dx = labelDx + DpiScale(hwnd, 12 + 4 + 26);
            Rect pill{pillX, savedTrayRect.y, dx, pillDy};
            savedPageRects.Append(pill);
            savedCloseRects.Append(Rect{pill.x + pill.dx - DpiScale(hwnd, 30), pill.y + DpiScale(hwnd, 5),
                                        DpiScale(hwnd, 26), DpiScale(hwnd, 26)});
            pillX += dx + gap;
        }
        for (int i = 0; i < len(savedGroupPaths) && i < len(groupDx); i++) {
            savedGroupRects.Append(Rect{pillX, savedTrayRect.y, groupDx[i], pillDy});
            pillX += groupDx[i] + gap;
        }
    }
    return true;
}

int TopBarWnd::SlotFromPoint(Point pt) {
    for (int i = 0; i < kTopBarSlotCount; i++) {
        if (gTopBarSlots[i].cmdId == 0) {
            continue;
        }
        if (slotRects[i].Contains(pt)) {
            return i;
        }
    }
    return -1;
}

// 4a rounds the zoom stepper and the standalone settings button into pills
// with circular buttons; the page / nav / layout groups are squared tracks
// (radius 12) with rounded-rect buttons (radius 10).
static bool GroupIsRound(int g) {
    return g == 3;
}

// Groups drawn on a rounded track. The rest sit straight on the bar, which is
// the surface their hover / press wash has to start from.
static bool GroupHasTrack(int g) {
    return g == 1 || g == 3;
}

static void FillTrack(HDC hdc, const Rect& r, int radius, COLORREF col, COLORREF borderCol = kColorUnset) {
    int d = radius * 2;
    d = std::min(d, std::min(r.dx, r.dy));
    AutoDeleteBrush br = CreateSolidBrush(col);
    AutoDeletePen pen = CreatePen(PS_SOLID, 1, borderCol == kColorUnset ? col : borderCol);
    ScopedSelectObject selBr(hdc, br);
    ScopedSelectObject selPen(hdc, pen);
    RoundRect(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy, d, d);
}

TouchPreviewWnd::TouchPreviewWnd() {
    kind = kindTouchPreview;
}

TouchPreviewWnd::~TouchPreviewWnd() {
    FreeCards();
}

void TouchPreviewWnd::FreeCards() {
    generation++;
    for (PreviewCard& card : cards) {
        str::Free(card.filePath);
        delete card.thumbnail;
    }
    cards.Reset();
}

struct TouchPreviewThumbnailRequest {
    HWND hwnd = nullptr;
    u64 generation = 0;
    int cardIdx = -1;
    Str filePath;
    int pageNo = 1;
    ~TouchPreviewThumbnailRequest() { str::Free(filePath); }
};

static void TouchPreviewThumbnailFinished(TouchPreviewThumbnailRequest* request, RenderedBitmap* thumbnail) {
    Wnd* wnd = WndListFindByHwnd(request->hwnd);
    if (wnd && wnd->kind == kindTouchPreview) {
        auto* preview = (TouchPreviewWnd*)wnd;
        if (preview->generation == request->generation && preview->cards.isValidIndex(request->cardIdx)) {
            PreviewCard& card = preview->cards[request->cardIdx];
            if (card.pageNo == request->pageNo && str::EqI(card.filePath, request->filePath)) {
                delete card.thumbnail;
                card.thumbnail = thumbnail;
                thumbnail = nullptr;
                HwndInvalidate(preview->hwnd, false);
            }
        }
    }
    delete thumbnail;
    delete request;
}

bool TouchPreviewWnd::Create(TopBarWnd* bar) {
    owner = bar;
    CreateCustomArgs args;
    args.visible = false;
    args.style = WS_POPUP;
    args.exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    args.pos = {0, 0, 10, 10};
    args.bgColor = ThemeHotBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner->win->hwndFrame);
    return true;
}

void TouchPreviewWnd::BuildLayout() {
    FreeCards();
    int pad = DpiScale(hwnd, 8);
    int gap = DpiScale(hwnd, 12);
    int cardDx = DpiScale(hwnd, 220);
    int cardDy = DpiScale(hwnd, 202);
    int x = pad;
    // Cards take their width and position from the tabs, so each card sits
    // under its own tab. Falls back to the fixed width when there is no tab
    // strip to measure (tabs turned off).
    HWND hwndTabs = nullptr;
    int firstTabX = 0;
    for (MainWindow* w : gWindows) {
        if (w->tabsCtrl && w->tabsCtrl->hwnd && HwndIsVisible(w->tabsCtrl->hwnd)) {
            hwndTabs = w->tabsCtrl->hwnd;
            break;
        }
    }
    if (hwndTabs) {
        RECT tr{};
        if (TabCtrl_GetItemRect(hwndTabs, 0, &tr)) {
            firstTabX = tr.left;
        } else {
            hwndTabs = nullptr;
        }
    }
    for (MainWindow* win : gWindows) {
        for (int i = 0; i < win->TabCount(); i++) {
            WindowTab* tab = win->GetTab(i);
            if (!tab || tab->IsNonDocumentTab()) {
                continue;
            }
            if (hwndTabs && win->tabsCtrl && win->tabsCtrl->hwnd == hwndTabs) {
                RECT tr{};
                if (TabCtrl_GetItemRect(hwndTabs, i, &tr)) {
                    x = pad + (tr.left - firstTabX);
                    cardDx = std::max(DpiScale(hwnd, 80), (int)(tr.right - tr.left));
                }
            }
            PreviewCard card;
            card.win = win;
            card.tabIdx = i;
            card.filePath = str::Dup(tab->filePath);
            card.pageNo = tab->ctrl ? tab->ctrl->CurrentPageNo() : 1;
            card.rect = {x, pad, cardDx, cardDy};
            int closeDy = DpiScale(hwnd, 24);
            card.closeRect = {x + cardDx - closeDy - DpiScale(hwnd, 4), pad + DpiScale(hwnd, 4), closeDy, closeDy};
            cards.Append(card);
            x += cardDx + gap;
        }
    }
    for (int i = 0; i < len(cards); i++) {
        PreviewCard& card = cards[i];
        auto* request = new TouchPreviewThumbnailRequest();
        request->hwnd = hwnd;
        request->generation = generation;
        request->cardIdx = i;
        request->filePath = str::Dup(card.filePath);
        request->pageNo = card.pageNo;
        auto* onRendered = NewFunc1(TouchPreviewThumbnailFinished, request);
        CreateThumbnailFromFileAsync(card.filePath, card.pageNo, onRendered);
    }
}

void TouchPreviewWnd::Show(HWND anchorHwnd, Rect anchorRect) {
    BuildLayout();
    if (len(cards) == 0) {
        Hide();
        return;
    }
    int pad = DpiScale(hwnd, 8);
    int dx = cards.Last().rect.x + cards.Last().rect.dx + pad;
    int dy = DpiScale(hwnd, 202) + 2 * pad;
    Point anchor = HwndMapWindowPoint(anchorHwnd, nullptr, {anchorRect.x, anchorRect.y + anchorRect.dy});
    int x = anchor.x;
    int y = anchor.y + DpiScale(hwnd, 4);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromPoint(POINT{anchor.x, anchor.y}, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &mi);
    Rect work = ToRect(mi.rcWork);
    // Anchored low (the switcher at the bottom of the rail): run the strip
    // along the bottom of the screen, starting just right of the rail - like
    // the taskbar's thumbnails - instead of flipping it above the button.
    bool anchoredLow = anchor.y > work.y + (work.dy * 2 / 3);
    if (anchoredLow) {
        x = anchor.x + anchorRect.dx + DpiScale(hwnd, 6);
        y = work.y + work.dy - dy - DpiScale(hwnd, 6);
    }
    if (x + dx > work.x + work.dx) {
        x = work.x + work.dx - dx;
    }
    if (x < work.x) {
        x = work.x;
    }
    if (!anchoredLow && y + dy > work.y + work.dy) {
        y = anchor.y - dy - DpiScale(hwnd, 4);
    }
    SetWindowPos(hwnd, HWND_TOP, x, y, dx, dy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HwndInvalidate(hwnd, true);
}

void TouchPreviewWnd::Hide() {
    if (owner) {
        owner->previewPinned = false;
    }
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void TouchPreviewWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    HdcFillRect(hdc, ToRect(ps->rcPaint), ThemeHotBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);
    int titleDy = DpiScale(hwnd, 32);
    int radius = DpiScale(hwnd, 12);
    for (const PreviewCard& card : cards) {
        FillTrack(hdc, card.rect, radius, RGB(0x2b, 0x2b, 0x2b), ThemeEdgeColor());

        Rect body = card.rect;
        body.y += titleDy;
        body.dy -= titleDy;
        FillTrack(hdc, body, radius, RGB(0xea, 0xe5, 0xde));
        HdcFillRect(hdc, Rect{body.x, body.y, body.dx, radius}, RGB(0xea, 0xe5, 0xde));

        WindowTab* tab = card.win->GetTab(card.tabIdx);
        if (!tab) {
            continue;
        }
        int iconDy = DpiScale(hwnd, 13);
        Rect fileIcon{card.rect.x + DpiScale(hwnd, 8), card.rect.y + (titleDy - iconDy) / 2, iconDy, iconDy};
        FillTrack(hdc, fileIcon, DpiScale(hwnd, 2), RgbToCOLORREF(0xe8927c));

        Rect title = card.rect;
        title.x = fileIcon.x + fileIcon.dx + DpiScale(hwnd, 7);
        title.dx = card.closeRect.x - title.x - DpiScale(hwnd, 4);
        title.dy = titleDy;
        SetTextColor(hdc, RGB(255, 255, 255));
        HdcDrawText(hdc, tab->GetTabTitle(), title,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 12));

        Gdiplus::Graphics gfx(hdc);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen closePen(Gdiplus::Color(235, 255, 255, 255), 1.5f);
        int arm = DpiScale(hwnd, 4);
        int cx = card.closeRect.x + card.closeRect.dx / 2;
        int cy = card.closeRect.y + card.closeRect.dy / 2;
        gfx.DrawLine(&closePen, cx - arm, cy - arm, cx + arm, cy + arm);
        gfx.DrawLine(&closePen, cx + arm, cy - arm, cx - arm, cy + arm);

        RenderedBitmap* thumbnail = card.thumbnail;
        Rect thumbArea = body;
        thumbArea.Inflate(-DpiScale(hwnd, 12), -DpiScale(hwnd, 10));
        if (thumbnail) {
            Size src = thumbnail->GetSize();
            int dstDx = thumbArea.dx;
            int dstDy = src.dy * dstDx / src.dx;
            if (dstDy > thumbArea.dy) {
                dstDy = thumbArea.dy;
                dstDx = src.dx * dstDy / src.dy;
            }
            Rect dst{thumbArea.x + (thumbArea.dx - dstDx) / 2, thumbArea.y + (thumbArea.dy - dstDy) / 2, dstDx, dstDy};
            thumbnail->Blit(hdc, dst);
        } else {
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, StrL("page"), thumbArea, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX,
                        HdcGetUiFont(hdc, 12));
        }
    }
}

LRESULT TouchPreviewWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        owner->CancelPreviewClose();
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
        owner->SchedulePreviewClose();
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        for (const PreviewCard& card : cards) {
            bool hitClose = card.closeRect.Contains(pt);
            bool hitCard = card.rect.Contains(pt);
            if (!hitClose && !hitCard) {
                continue;
            }
            // The card holds a raw MainWindow*/index captured when the popup
            // was built. A window can close (or its tabs shift) while the popup
            // lingers, so revalidate before touching either.
            if (!IsMainWindowValid(card.win) || card.tabIdx < 0 || card.tabIdx >= card.win->TabCount()) {
                Hide();
                return 0;
            }
            if (hitClose) {
                if (len(cards) > 1) {
                    WindowTab* tab = card.win->GetTab(card.tabIdx);
                    Hide();
                    if (tab) {
                        CloseTab(tab, false);
                    }
                }
                return 0;
            }
            Hide();
            TabsSelect(card.win, card.tabIdx);
            SetForegroundWindow(card.win->hwndFrame);
            return 0;
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Hide();
        return 0;
    }
    return WndProcDefault(hwnd, msg, wparam, lparam);
}

TouchBookmarkConfirmWnd::TouchBookmarkConfirmWnd() {
    kind = "touchBookmarkConfirm";
}

bool TouchBookmarkConfirmWnd::Create(TopBarWnd* bar) {
    owner = bar;
    CreateCustomArgs args;
    args.visible = false;
    args.style = WS_POPUP;
    args.exStyle = WS_EX_TOOLWINDOW;
    args.pos = {0, 0, 10, 10};
    args.bgColor = ThemeControlBackgroundColor();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner->win->hwndFrame);
    return true;
}

void TouchBookmarkConfirmWnd::Show(int page, Rect anchorRect) {
    pageNo = page;
    int dx = DpiScale(hwnd, 300);
    int dy = DpiScale(hwnd, 112);
    Point anchor = HwndMapWindowPoint(owner->hwnd, nullptr, {anchorRect.x, anchorRect.y + anchorRect.dy});
    int x = anchor.x;
    int y = anchor.y + DpiScale(hwnd, 6);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromPoint(POINT{anchor.x, anchor.y}, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &mi);
    Rect work = ToRect(mi.rcWork);
    x = std::clamp(x, work.x, std::max(work.x, work.x + work.dx - dx));
    if (y + dy > work.y + work.dy) {
        y = anchor.y - dy - DpiScale(hwnd, 6);
    }

    int buttonDy = DpiScale(hwnd, 36);
    int buttonY = dy - DpiScale(hwnd, 12) - buttonDy;
    int removeDx = DpiScale(hwnd, 78);
    int cancelDx = DpiScale(hwnd, 72);
    removeRect = {dx - DpiScale(hwnd, 12) - removeDx, buttonY, removeDx, buttonDy};
    cancelRect = {removeRect.x - DpiScale(hwnd, 8) - cancelDx, buttonY, cancelDx, buttonDy};

    SetWindowPos(hwnd, HWND_TOP, x, y, dx, dy, SWP_SHOWWINDOW);
    HwndInvalidate(hwnd, true);
}

void TouchBookmarkConfirmWnd::Hide() {
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void TouchBookmarkConfirmWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    HdcFillRect(hdc, ToRect(ps->rcPaint), ThemeControlBackgroundColor());
    Rect rc = HwndClientRect(hwnd);
    AutoDeletePen border = CreatePen(PS_SOLID, 1, ThemeEdgeColor());
    ScopedSelectObject selPen(hdc, border);
    SelectObject(hdc, GetStockBrush(NULL_BRUSH));
    int radius = DpiScale(hwnd, 10);
    RoundRect(hdc, 0, 0, rc.dx, rc.dy, radius * 2, radius * 2);

    Rect prompt{DpiScale(hwnd, 14), DpiScale(hwnd, 12), rc.dx - DpiScale(hwnd, 28), DpiScale(hwnd, 34)};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, fmt("Remove page %d from your bookmarks?", pageNo), prompt,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, HdcGetUiFont(hdc, 14, FW_MEDIUM));

    FillTrack(hdc, cancelRect, cancelRect.dy / 2, RGB(0xea, 0xe5, 0xde));
    COLORREF accentBg, accentFg;
    ThemeAccentSurfaceColors(&accentBg, &accentFg);
    accentBg = RGB(0xb4, 0x53, 0x0a);
    accentFg = RGB(255, 255, 255);
    FillTrack(hdc, removeRect, removeRect.dy / 2, accentBg);
    SetTextColor(hdc, ThemeWindowTextColor());
    HdcDrawText(hdc, StrL("Cancel"), cancelRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_MEDIUM));
    SetTextColor(hdc, accentFg);
    HdcDrawText(hdc, StrL("Remove"), removeRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, 13, FW_SEMIBOLD));
}

LRESULT TouchBookmarkConfirmWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (removeRect.Contains(pt)) {
            int page = pageNo;
            Hide();
            owner->RemoveSavedPage(page);
        } else if (cancelRect.Contains(pt)) {
            Hide();
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Hide();
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) {
        Hide();
        return 0;
    }
    return WndProcDefault(hwnd, msg, wparam, lparam);
}

static void DrawDashedTrack(HDC hdc, const Rect& r, int radius, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), 1.5f);
    pen.SetDashStyle(Gdiplus::DashStyleDash);
    float x = (float)r.x + 0.75f;
    float y = (float)r.y + 0.75f;
    float dx = (float)r.dx - 1.5f;
    float dy = (float)r.dy - 1.5f;
    float d = (float)std::min(radius * 2, std::min(r.dx, r.dy));
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + dx - d, y, d, d, 270, 90);
    path.AddArc(x + dx - d, y + dy - d, d, d, 0, 90);
    path.AddArc(x, y + dy - d, d, d, 90, 90);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

void TopBarWnd::OnPaint(HDC hdc, PAINTSTRUCT*) {
    Rect rcClient = HwndClientRect(hwnd);
    COLORREF bgCol = TopBarBgColor();
    // Buffer the whole bar: an animating button repaints ~60 times a second and
    // the fill-then-draw sequence would show as a shimmer. Client-origin, so no
    // world transform (which ImageList_Draw's BitBlt would ignore) is involved,
    // and Flush is clipped by hdc to whatever the animation invalidated.
    DoubleBuffer buffer(hwnd, rcClient);
    HDC hdcOut = hdc;
    hdc = buffer.GetDC();
    HdcFillRect(hdc, rcClient, bgCol);
    // bottom edge, separating the bar from the canvas
    HdcFillRect(hdc, Rect{0, rcClient.dy - 1, rcClient.dx, 1}, ThemeEdgeColor());

    if (!iml) {
        RebuildImageList();
    }
    if (!Layout(hdc, slotRects)) {
        buffer.Flush(hdcOut);
        return;
    }

    bool isEnabled = win && win->IsDocLoaded();
    COLORREF groupCol = TopBarGroupColor();

    // one rounded track per group, drawn under the slots. Derive the range
    // from the table: a hardcoded bound silently stops drawing tracks for any
    // group added later.
    int maxGroup = -1;
    for (int i = 0; i < kTopBarSlotCount; i++) {
        maxGroup = std::max(maxGroup, gTopBarSlots[i].group);
    }
    for (int g = 0; g <= maxGroup; g++) {
        if (!GroupHasTrack(g)) {
            continue;
        }
        Rect track;
        bool has = false;
        for (int i = 0; i < kTopBarSlotCount; i++) {
            if (gTopBarSlots[i].group != g || slotRects[i].IsEmpty()) {
                continue;
            }
            track = has ? track.Union(slotRects[i]) : slotRects[i];
            has = true;
        }
        if (!has) {
            continue;
        }
        int trackPad = DpiScale(hwnd, g == 1 ? 3 : 4);
        track.Inflate(trackPad, trackPad);
        int trackRadius = GroupIsRound(g) ? (track.dy / 2) : DpiScale(hwnd, 12);
        FillTrack(hdc, track, trackRadius, groupCol);
    }

    SetBkMode(hdc, TRANSPARENT);
    HFONT font = TopBarFont(hwnd);
    ScopedSelectObject selFont(hdc, font);

    COLORREF textCol = ThemeWindowTextColor();
    COLORREF mutedCol = ThemeWindowDarkerTextColor();

    if (!nameRect.IsEmpty()) {
        SetTextColor(hdc, textCol);
        Str title = win->CurrentTab()->GetTabTitle();
        HdcDrawText(hdc, title, nameRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX,
                    TopBarFontWeighted(hdc, kFontSizeBody, kFontWeightStrong));
    }

    int currentPage = win && win->ctrl ? win->ctrl->CurrentPageNo() : 0;
    HFONT savedFont = TopBarFontWeighted(hdc, kFontSizeMeta, kFontWeightStrong);
    int trayDc = SaveDC(hdc);
    if (!savedTrayRect.IsEmpty()) {
        IntersectClipRect(hdc, savedTrayRect.x, savedTrayRect.y, savedTrayRect.x + savedTrayRect.dx,
                          savedTrayRect.y + savedTrayRect.dy);
    }
    for (int i = 0; i < len(savedGroupRects) && i < len(savedGroupPaths); i++) {
        Rect chip = savedGroupRects[i];
        if (chip.Intersect(savedTrayRect).IsEmpty()) {
            continue;
        }
        Str fp = savedGroupPaths[i];
        COLORREF chipCol = TouchFavoriteGroupColor(fp);
        FillTrack(hdc, chip, chip.dy / 2, chipCol);
        // every palette entry is dark enough for this to always read
        SetTextColor(hdc, RGB(255, 255, 255));
        TempStr name = path::GetBaseNameTemp(fp);
        Rect tr{chip.x + DpiScale(hwnd, 12), chip.y, chip.dx - DpiScale(hwnd, 24), chip.dy};
        HdcDrawText(hdc, name, tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, savedFont);
    }

    // Same palette a document reads as elsewhere in the tray (see the chip
    // loop above), so a page pill still says which document it belongs to
    // after you have opened that document and its chip is gone.
    WindowTab* curTabForCol = win ? win->CurrentTab() : nullptr;
    bool haveOwnDocCol = curTabForCol && !curTabForCol->IsAboutTab();
    COLORREF ownDocCol = haveOwnDocCol ? TouchFavoriteGroupColor(curTabForCol->filePath) : groupCol;

    for (int i = 0; i < len(savedPageRects); i++) {
        int page = savedPages[i];
        Rect pill = savedPageRects[i];
        if (pill.Intersect(savedTrayRect).IsEmpty()) {
            continue;
        }
        bool active = page == currentPage;
        COLORREF pillBg = ownDocCol;
        COLORREF pillFg = haveOwnDocCol ? RGB(255, 255, 255) : textCol;
        if (active) {
            ThemeAccentSurfaceColors(&pillBg, &pillFg);
        }
        FillTrack(hdc, pill, pill.dy / 2, pillBg);
        Rect close = savedCloseRects[i];
        Rect label = pill;
        label.x += DpiScale(hwnd, 12);
        label.dx = close.x - label.x - DpiScale(hwnd, 2);
        SetTextColor(hdc, pillFg);
        if (i == savedRenamingIdx) {
            if (savedRenameSelectAll) {
                Rect selection = label;
                selection.Inflate(DpiScale(hwnd, 2), -DpiScale(hwnd, 8));
                FillTrack(hdc, selection, DpiScale(hwnd, 3), GetSysColor(COLOR_HIGHLIGHT));
                SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
            }
            HdcDrawText(hdc, WStr(savedRenameText), label,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, savedFont);
        } else {
            HdcDrawText(hdc, SavedPageLabelTemp(i), label,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, savedFont);
        }
        COLORREF old = SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HFONT closeFont = HdcGetUiFont(hdc, 12, FW_NORMAL);
        HdcDrawText(hdc, StrL("×"), close, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX, closeFont);
        SetTextColor(hdc, old);
        if (i == savedHoldingIdx) {
            double elapsed = (double)(GetTickCount64() - savedHoldStarted);
            float sweep = (float)(std::min(elapsed, 3000.0) * 360.0 / 3000.0);
            Gdiplus::Graphics gfx(hdc);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen progressPen(Gdiplus::Color(255, 0xb4, 0x53, 0x0a), 2.0f);
            Rect ring = close;
            ring.Inflate(-DpiScale(hwnd, 2), -DpiScale(hwnd, 2));
            gfx.DrawArc(&progressPen, ring.x, ring.y, ring.dx, ring.dy, -90.0f, sweep);
        }
    }
    RestoreDC(hdc, trayDc);

    COLORREF emphCol = TopBarEmphColor();
    int pressInset = DpiScale(hwnd, kTopBarPressInset);
    for (int i = 0; i < kTopBarSlotCount; i++) {
        const TopBarSlot& slot = gTopBarSlots[i];
        Rect r = slotRects[i];
        if (r.IsEmpty()) {
            continue;
        }
        // hover and press are two stops on one color path; a press also sinks
        // the fill a couple of pixels, which is what makes it read as a push
        float press = pressAnim[i].Value();
        float emphasis = hoverAnim[i].Value() * kTopBarHoverEmphasis + press * kTopBarPressEmphasis;
        emphasis = isEnabled ? limitValue(emphasis, 0.0f, 1.0f) : 0.0f;
        Rect washRect = r;
        int inset = AnimLerpInt(0, pressInset, press);
        washRect.Inflate(-inset, -inset);
        switch (slot.item) {
            case TopBarItem::PageBox: {
                // 4a: the page readout sits in its own inset box - a lighter
                // fill with a 1px border - within the darker group track
                {
                    Rect box = r;
                    box.Inflate(0, -DpiScale(hwnd, 2));
                    FillTrack(hdc, box, DpiScale(hwnd, 10), ThemeWindowControlBackgroundColor(), ThemeEdgeColor());
                }
                // "1" in the text color, "/ 613" a size down and muted
                TempStr cur = pageEditing ? str::DupTemp(editText) : CurPageTextTemp(win);
                TempStr total = TotalPagesTextTemp(win);
                HFONT fCur = TopBarFontWeighted(hdc, kFontSizeLabel + 2, kFontWeightStrong);
                HFONT fTot = TopBarFontWeighted(hdc, kFontSizeLabel, FW_DONTCARE);
                Size szCur = HdcGetTextExtentPoint32Font(hdc, cur, fCur);
                Size szTot = HdcGetTextExtentPoint32Font(hdc, total, fTot);
                int gap = DpiScale(hwnd, 6);
                int totalDx = szCur.dx + gap + szTot.dx;
                Rect rCur = r;
                rCur.x += (r.dx - totalDx) / 2;
                rCur.dx = szCur.dx;
                if (pageEditing && editSelectAll) {
                    Rect selection = rCur;
                    selection.Inflate(DpiScale(hwnd, 3), -DpiScale(hwnd, 8));
                    FillTrack(hdc, selection, DpiScale(hwnd, 3), GetSysColor(COLOR_HIGHLIGHT));
                    SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                } else {
                    SetTextColor(hdc, isEnabled ? textCol : mutedCol);
                }
                HdcDrawTextTabular(hdc, cur, rCur, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX, fCur);
                Rect rTot = r;
                rTot.x = rCur.x + szCur.dx + gap;
                rTot.dx = szTot.dx;
                SetTextColor(hdc, mutedCol);
                HdcDrawTextTabular(hdc, total, rTot, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX, fTot);
                break;
            }
            case TopBarItem::ZoomLabel: {
                SetTextColor(hdc, isEnabled ? textCol : mutedCol);
                uint fmtFlags = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX;
                TempStr text = zoomEditing ? fmt("%s%%", Str(editText)) : ZoomTextTemp(win);
                HdcDrawTextTabular(hdc, text, r, fmtFlags, TopBarFontWeighted(hdc, kFontSizeBody, kFontWeightMedium));
                break;
            }
            case TopBarItem::Preview:
            case TopBarItem::Overflow: {
                FillTrack(hdc, r, r.dy / 2, groupCol);
                if (emphasis > 0.0f) {
                    FillTrack(hdc, washRect, washRect.dy / 2, AnimLerpColor(groupCol, emphCol, emphasis));
                }
                int ix = r.x + ((r.dx - iconDy) / 2);
                int iy = r.y + ((r.dy - iconDy) / 2);
                ImageList_Draw(iml, (int)slot.icon, hdc, ix, iy, ILD_NORMAL);
                break;
            }
            case TopBarItem::Bookmark: {
                DrawDashedTrack(hdc, r, r.dy / 2, RGB(0xc9, 0xc2, 0xb6));
                int ix = r.x + ((r.dx - iconDy) / 2);
                int iy = r.y + ((r.dy - iconDy) / 2);
                ImageList_Draw(imlPaper, (int)slot.icon, hdc, ix, iy, ILD_NORMAL);
                break;
            }
            default: {
                // a button: the active view mode is tinted, as in the design;
                // otherwise a hover fill. Circular in round groups (zoom /
                // settings), rounded-rect in the squared page / nav groups.
                int btnRadius = GroupIsRound(slot.group) ? (r.dy / 2) : DpiScale(hwnd, 10);
                bool isActive = isEnabled && IsSlotActive(win, slot);
                if (isActive) {
                    COLORREF abg, afg;
                    ThemeAccentSurfaceColors(&abg, &afg);
                    FillTrack(hdc, r, btnRadius, abg);
                } else if (emphasis > 0.0f) {
                    // One step darker than the surface the button sits on - the
                    // same formula the rail uses, so hover feels like one
                    // system. ThemeHotBackgroundColor equals the track color on
                    // the Touch Paper theme, which made the hover invisible.
                    // Only groups 1 and 3 get a track drawn under them; the
                    // rest sit on the bar itself, and starting the fade from
                    // the wrong surface would make it jump on its first frame.
                    COLORREF base = GroupHasTrack(slot.group) ? groupCol : bgCol;
                    int washRadius = GroupIsRound(slot.group) ? (washRect.dy / 2) : btnRadius;
                    FillTrack(hdc, washRect, washRadius, AnimLerpColor(base, emphCol, emphasis));
                }
                int ix = r.x + ((r.dx - iconDy) / 2);
                int iy = r.y + ((r.dy - iconDy) / 2);
                ImageList_Draw(iml, (int)slot.icon, hdc, ix, iy, ILD_NORMAL);
                break;
            }
        }
    }
    buffer.Flush(hdcOut);
}

// Aims a slot's hover animation at its resting value. Only the two slots that
// can change are invalidated, so a hover fade never repaints the whole bar.
void TopBarWnd::SetHot(int idx) {
    if (idx == hotIdx) {
        return;
    }
    int old = hotIdx;
    hotIdx = idx;
    if (old >= 0) {
        hoverAnim[old].SetTarget(0.0f, kAnimHoverMs);
        HwndInvalidateRect(hwnd, slotRects[old], false);
    }
    if (idx >= 0 && SlotHasWash(gTopBarSlots[idx])) {
        hoverAnim[idx].SetTarget(1.0f, kAnimHoverMs);
        HwndInvalidateRect(hwnd, slotRects[idx], false);
    } else if (idx >= 0) {
        hoverAnim[idx].Set(0.0f);
    }
    animTimer.Start();
}

// Feedback only: the click is still acted on at WM_LBUTTONUP, so nothing waits
// for this to finish.
void TopBarWnd::SetPressed(int idx) {
    if (idx == pressedIdx) {
        return;
    }
    int old = pressedIdx;
    pressedIdx = idx;
    if (old >= 0) {
        pressAnim[old].SetTarget(0.0f, kAnimPressReleaseMs);
        HwndInvalidateRect(hwnd, slotRects[old], false);
    }
    if (idx >= 0) {
        pressAnim[idx].SetTarget(1.0f, kAnimPressMs);
        HwndInvalidateRect(hwnd, slotRects[idx], false);
    }
    animTimer.Start();
}

// One frame; stops the timer as soon as nothing is in motion, so an idle bar
// costs nothing.
void TopBarWnd::OnAnimTick() {
    bool anyRunning = false;
    for (int i = 0; i < kTopBarSlotCount; i++) {
        bool running = hoverAnim[i].IsAnimating() || pressAnim[i].IsAnimating();
        if (running || animDirty[i]) {
            HwndInvalidateRect(hwnd, slotRects[i], false);
        }
        animDirty[i] = running;
        anyRunning |= running;
    }
    if (!anyRunning) {
        animTimer.Stop();
    }
}

void TopBarWnd::BeginEdit(bool page) {
    if (!win || !win->ctrl) {
        return;
    }
    if (!page && win->ctrl->GetZoomVirtual() == kZoomSmartWidth) {
        float manualZoom = win->ctrl->GetZoomVirtual(true);
        win->ctrl->SetZoomVirtual(manualZoom, nullptr);
    }
    pageEditing = page;
    zoomEditing = !page;
    editSelectAll = true;
    int value = page ? win->ctrl->CurrentPageNo() : (int)(win->ctrl->GetZoomVirtual(true) + 0.5f);
    snprintf(editText, dimof(editText), "%d", value);
    HwndSetFocus(hwnd);
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::CommitEdit() {
    if ((!pageEditing && !zoomEditing) || !win || !win->ctrl) {
        return;
    }
    int value = atoi(editText);
    if (pageEditing) {
        value = limitValue(value, 1, win->ctrl->PageCount());
        win->ctrl->GoToPage(value, true);
    } else {
        value = limitValue(value, 10, 400);
        win->ctrl->SetZoomVirtual((float)value, nullptr);
        HwndInvalidate(win->hwndCanvas, false);
    }
    pageEditing = false;
    zoomEditing = false;
    editSelectAll = false;
    editText[0] = 0;
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::CancelEdit() {
    pageEditing = false;
    zoomEditing = false;
    editSelectAll = false;
    editText[0] = 0;
    HwndInvalidate(hwnd, false);
}

// The top bar's "..." menu: view options that have no room of their own.
void TopBarWnd::ShowOverflowMenu(const Rect& anchor) {
    if (!win) {
        return;
    }
    constexpr int kOverflowSmartMargins = 1;
    constexpr int kOverflowSmartHeaderFooter = 2;
    constexpr int kOverflowTrimDialog = 3;

    HMENU popup = CreatePopupMenu();
    bool on = gGlobalPrefs->smartMargins;
    bool hasDoc = win->AsFixed() != nullptr;
    uint enabled = hasDoc ? MF_ENABLED : (MF_DISABLED | MF_GRAYED);
    uint flags = MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED) | enabled;
    AppendMenuW(popup, flags, kOverflowSmartMargins, L"Smart margins");
    // only does anything on top of the margin trim, so it follows it and greys
    // out until it is on
    bool hf = gGlobalPrefs->smartHeaderFooter;
    uint hfFlags = MF_STRING | (hf ? MF_CHECKED : MF_UNCHECKED) | (hasDoc && on ? MF_ENABLED : (MF_DISABLED | MF_GRAYED));
    AppendMenuW(popup, hfFlags, kOverflowSmartHeaderFooter, L"Smart header && footer");
    // the manual fallback, for documents nothing can be read from
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING | enabled, kOverflowTrimDialog, L"Trim headers && footers…");
    MarkMenuOwnerDraw(popup);

    Point pt = HwndClientToScreen(hwnd, Point{anchor.x, anchor.y + anchor.dy});
    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);

    if (cmd == kOverflowTrimDialog) {
        ShowTrimHeaderFooterDialog(win);
        HwndInvalidate(hwnd, false);
        return;
    }
    if (cmd == kOverflowSmartMargins) {
        gGlobalPrefs->smartMargins = !gGlobalPrefs->smartMargins;
    } else if (cmd == kOverflowSmartHeaderFooter) {
        gGlobalPrefs->smartHeaderFooter = !gGlobalPrefs->smartHeaderFooter;
    } else {
        return;
    }
    SaveSettings();
    // Every page's laid-out height changes, so relayout and put the view back
    // where it was (same shape as ToggleMangaMode).
    for (MainWindow* w : gWindows) {
        DisplayModel* dm = w->AsFixed();
        if (!dm) {
            continue;
        }
        ScrollState state = dm->GetScrollState();
        dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
        dm->SetScrollState(state);
        w->RedrawAll(true);
    }
    HwndInvalidate(hwnd, false);
}

void TopBarWnd::AddCurrentPageBookmark() {
    if (!win || !win->ctrl) {
        return;
    }
    int page = win->ctrl->CurrentPageNo();
    if (savedPages.Contains(page)) {
        return;
    }
    // titled by the ToC heading covering the page when there is one, so a saved
    // page reads as its section name rather than "p. 12"
    TempStr name = FavoriteDefaultNameTemp(win, page);
    AddFavoriteQuiet(win, page, name);
    RefreshSavedPages();
    // Keep the newly-created pill visible when the tray already overflows.
    savedScrollX = INT_MAX;
    HwndInvalidate(hwnd, false);
}

LRESULT TopBarWnd::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }

    if (msg == WM_MOUSEMOVE) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (savedTrayDragging) {
            int delta = pt.x - savedTrayDragX;
            if (abs(delta) >= DpiScale(hwnd, 6)) {
                savedTrayDidDrag = true;
                savedBodyHoldingIdx = -1;
                savedBodyHoldStarted = 0;
                KillTimer(hwnd, kSavedPageRenameTimerId);
            }
            int maxScroll = std::max(0, savedContentDx - savedTrayRect.dx);
            int next = std::clamp(savedTrayDragScrollX - delta, 0, maxScroll);
            if (next != savedScrollX) {
                savedScrollX = next;
                HwndInvalidate(hwnd, false);
            }
            return 0;
        }
        int idx = SlotFromPoint(pt);
        int oldHotIdx = hotIdx;
        SetHot(idx);
        if (idx == 0 && oldHotIdx != 0 && gTopBarSlots[0].item == TopBarItem::Preview) {
            KillTimer(hwnd, kPreviewHoverTimerId);
            SetTimer(hwnd, kPreviewHoverTimerId, kPreviewHoverDelayMs, nullptr);
        } else if (oldHotIdx == 0 && idx != 0 && gTopBarSlots[0].item == TopBarItem::Preview) {
            KillTimer(hwnd, kPreviewHoverTimerId);
            SchedulePreviewClose();
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
        if (hotIdx == 0 && gTopBarSlots[0].item == TopBarItem::Preview) {
            SchedulePreviewClose();
        }
        SetHot(-1);
        // the bar only captures for the saved-page tray, so a press dragged off
        // a button ends here
        SetPressed(-1);
        return 0;
    }

    if (msg == WM_TIMER && wparam == kTopBarAnimTimerId) {
        OnAnimTick();
        return 0;
    }

    if (msg == WM_LBUTTONDOWN) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (savedRenamingIdx >= 0 &&
            (savedRenamingIdx >= len(savedPageRects) || !savedPageRects[savedRenamingIdx].Contains(pt))) {
            CommitSavedPageRename();
        }
        int clickedSlot = SlotFromPoint(pt);
        if (clickedSlot != 0 || gTopBarSlots[0].item != TopBarItem::Preview) {
            CloseTouchDocumentPreview(win);
        }
        for (int i = 0; i < len(savedCloseRects); i++) {
            if (!savedTrayRect.Contains(pt) || !savedCloseRects[i].Contains(pt)) {
                continue;
            }
            if (bookmarkConfirmWnd) {
                bookmarkConfirmWnd->Hide();
            }
            savedHoldingIdx = i;
            savedHoldStarted = GetTickCount64();
            savedHoldCompleted = false;
            SetCapture(hwnd);
            SetTimer(hwnd, kSavedPageHoldTimerId, 30, nullptr);
            HwndInvalidate(hwnd, false);
            return 0;
        }
        for (int i = 0; i < len(savedPageRects); i++) {
            if (!savedTrayRect.Contains(pt) || !savedPageRects[i].Contains(pt)) {
                continue;
            }
            if (i == savedRenamingIdx) {
                HwndSetFocus(hwnd);
                return 0;
            }
            savedBodyHoldingIdx = i;
            savedBodyHoldStarted = GetTickCount64();
            savedBodyHoldTriggered = false;
            savedTrayDragging = savedContentDx > savedTrayRect.dx;
            savedTrayDidDrag = false;
            savedTrayDragX = pt.x;
            savedTrayDragScrollX = savedScrollX;
            SetCapture(hwnd);
            SetTimer(hwnd, kSavedPageRenameTimerId, 500, nullptr);
            return 0;
        }
        if (savedTrayRect.Contains(pt) && savedContentDx > savedTrayRect.dx) {
            savedTrayDragging = true;
            savedTrayDidDrag = false;
            savedTrayDragX = pt.x;
            savedTrayDragScrollX = savedScrollX;
            SetCapture(hwnd);
            return 0;
        }
        if (bookmarkConfirmWnd) {
            bookmarkConfirmWnd->Hide();
        }
        // feedback only, and deliberately after the tray cases above (which
        // return): a press on a button, not on a saved-page pill
        if (clickedSlot >= 0 && win && win->IsDocLoaded() && SlotHasWash(gTopBarSlots[clickedSlot])) {
            SetPressed(clickedSlot);
        }
    }

    if (msg == WM_CAPTURECHANGED && GetCapture() != hwnd) {
        bool changed = false;
        if (savedHoldingIdx >= 0) {
            savedHoldingIdx = -1;
            savedHoldStarted = 0;
            KillTimer(hwnd, kSavedPageHoldTimerId);
            changed = true;
        }
        if (savedTrayDragging) {
            savedTrayDragging = false;
            savedTrayDidDrag = false;
            changed = true;
        }
        if (savedBodyHoldingIdx >= 0) {
            savedBodyHoldingIdx = -1;
            savedBodyHoldStarted = 0;
            KillTimer(hwnd, kSavedPageRenameTimerId);
            changed = true;
        }
        SetPressed(-1);
        if (changed) {
            HwndInvalidate(hwnd, false);
            return 0;
        }
    }

    if (msg == WM_TIMER && wparam == kPreviewHoverTimerId) {
        KillTimer(hwnd, kPreviewHoverTimerId);
        if (pendingPreviewAnchor) {
            ShowPreview(pendingPreviewAnchor, pendingPreviewRect);
            pendingPreviewAnchor = nullptr;
        }
        return 0;
    }

    if (msg == WM_TIMER && wparam == kPreviewCloseTimerId) {
        CancelPreviewClose();
        if (previewWnd) {
            previewWnd->Hide();
        }
        return 0;
    }

    if (msg == WM_TIMER && wparam == kSavedPageHoldTimerId) {
        if (savedHoldingIdx < 0 || savedHoldingIdx >= len(savedPages)) {
            KillTimer(hwnd, kSavedPageHoldTimerId);
            return 0;
        }
        if (GetTickCount64() - savedHoldStarted >= 3000) {
            int page = savedPages[savedHoldingIdx];
            savedHoldingIdx = -1;
            savedHoldCompleted = true;
            KillTimer(hwnd, kSavedPageHoldTimerId);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            RemoveSavedPage(page);
        } else {
            HwndInvalidate(hwnd, false);
        }
        return 0;
    }

    if (msg == WM_TIMER && wparam == kSavedPageRenameTimerId) {
        KillTimer(hwnd, kSavedPageRenameTimerId);
        if (savedBodyHoldingIdx >= 0 && savedBodyHoldingIdx < len(savedPages) && !savedTrayDidDrag) {
            int idx = savedBodyHoldingIdx;
            savedBodyHoldingIdx = -1;
            savedBodyHoldStarted = 0;
            savedBodyHoldTriggered = true;
            savedTrayDragging = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            BeginSavedPageRename(idx);
        }
        return 0;
    }

    if (msg == WM_LBUTTONUP) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // the button eases back out while the command below already runs
        SetPressed(-1);
        if (savedTrayDragging) {
            bool dragged = savedTrayDidDrag;
            int quickBodyIdx = savedBodyHoldingIdx;
            savedTrayDragging = false;
            savedTrayDidDrag = false;
            savedBodyHoldingIdx = -1;
            savedBodyHoldStarted = 0;
            KillTimer(hwnd, kSavedPageRenameTimerId);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (dragged) {
                return 0;
            }
            if (quickBodyIdx >= 0 && quickBodyIdx < len(savedPages) && win && win->ctrl) {
                win->ctrl->GoToPage(savedPages[quickBodyIdx], true);
                HwndInvalidate(hwnd, false);
                return 0;
            }
        }
        if (savedBodyHoldTriggered) {
            savedBodyHoldTriggered = false;
            return 0;
        }
        if (savedBodyHoldingIdx >= 0) {
            int idx = savedBodyHoldingIdx;
            savedBodyHoldingIdx = -1;
            savedBodyHoldStarted = 0;
            KillTimer(hwnd, kSavedPageRenameTimerId);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (idx < len(savedPages) && win && win->ctrl) {
                win->ctrl->GoToPage(savedPages[idx], true);
                HwndInvalidate(hwnd, false);
            }
            return 0;
        }
        if (savedHoldCompleted) {
            savedHoldCompleted = false;
            return 0;
        }
        if (savedHoldingIdx >= 0) {
            int idx = savedHoldingIdx;
            savedHoldingIdx = -1;
            KillTimer(hwnd, kSavedPageHoldTimerId);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            HwndInvalidate(hwnd, false);
            ShowBookmarkConfirm(idx);
            return 0;
        }
        // a document chip opens that document's saved pages
        for (int i = 0; i < len(savedGroupRects) && i < len(savedGroupPaths); i++) {
            if (!savedTrayRect.Contains(pt) || !savedGroupRects[i].Contains(pt)) {
                continue;
            }
            Str fp = savedGroupPaths[i];
            Vec<Favorite*>* favs = GetFileFavorites(fp);
            if (!favs || len(*favs) == 0) {
                return 0;
            }
            HMENU popup = CreatePopupMenu();
            for (int k = 0; k < len(*favs); k++) {
                Favorite* fav = (*favs)[k];
                if (!fav || fav->isTemporary) {
                    continue;
                }
                TempStr label = FavReadableNameTemp(fav);
                AppendMenuW(popup, MF_STRING, (UINT_PTR)(k + 1), ToWStrTemp(label).s);
            }
            MarkMenuOwnerDraw(popup);
            Rect chip = savedGroupRects[i];
            Point screen = HwndClientToScreen(hwnd, Point{chip.x, chip.y + chip.dy});
            int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_LEFTBUTTON, screen.x, screen.y, 0, win->hwndFrame,
                                     nullptr);
            FreeMenuOwnerDrawInfoData(popup);
            DestroyMenu(popup);
            if (cmd > 0 && cmd <= len(*favs)) {
                FileState* fs = gFileHistory.FindByPath(fp);
                if (fs) {
                    GoToFavorite(win, fs, (*favs)[cmd - 1]);
                }
            }
            return 0;
        }
        for (int i = 0; i < len(savedCloseRects); i++) {
            if (savedTrayRect.Contains(pt) && savedCloseRects[i].Contains(pt)) {
                ShowBookmarkConfirm(i);
                return 0;
            }
        }
        for (int i = 0; i < len(savedPageRects); i++) {
            if (savedTrayRect.Contains(pt) && savedPageRects[i].Contains(pt) && win && win->ctrl) {
                win->ctrl->GoToPage(savedPages[i], true);
                HwndInvalidate(hwnd, false);
                return 0;
            }
        }
        int idx = SlotFromPoint(pt);
        if (idx >= 0 && win && win->IsDocLoaded()) {
            const TopBarSlot& slot = gTopBarSlots[idx];
            if (slot.cmdId == kTopBarPageEdit) {
                BeginEdit(true);
            } else if (slot.cmdId == kTopBarZoomEdit) {
                if (win->ctrl->GetZoomVirtual() == kZoomSmartWidth) {
                    // Re-run Smart Width against the current page. SetZoomVirtual
                    // deliberately relayouts fit modes even when the virtual
                    // zoom value is unchanged.
                    win->ctrl->SetZoomVirtual(kZoomSmartWidth, nullptr);
                    HwndInvalidate(win->hwndCanvas, false);
                } else {
                    BeginEdit(false);
                }
            } else if (slot.cmdId == kTopBarPreview) {
                PinPreview(hwnd, PreviewAnchorRect(slotRects[idx]));
            } else if (slot.cmdId == kTopBarBookmark) {
                AddCurrentPageBookmark();
            } else if (slot.cmdId == kTopBarOverflow) {
                ShowOverflowMenu(slotRects[idx]);
            } else if (slot.cmdId == kTopBarSmartWidth) {
                if (win->ctrl->GetZoomVirtual() == kZoomSmartWidth) {
                    float manualZoom = win->ctrl->GetZoomVirtual(true);
                    win->ctrl->SetZoomVirtual(manualZoom, nullptr);
                } else {
                    win->ctrl->SetZoomVirtual(kZoomSmartWidth, nullptr);
                }
                HwndSetFocus(hwnd);
                HwndInvalidate(hwnd, false);
            } else if (slot.cmdId > 0) {
                HwndSendCommand(win->hwndFrame, slot.cmdId);
                if (pageEditing) {
                    snprintf(editText, dimof(editText), "%d", win->ctrl->CurrentPageNo());
                } else if (zoomEditing) {
                    snprintf(editText, dimof(editText), "%d", (int)(win->ctrl->GetZoomVirtual(true) + 0.5f));
                }
                HwndSetFocus(hwnd);
                HwndInvalidate(hwnd, false);
            }
        }
        return 0;
    }

    if (msg == WM_RBUTTONUP) {
        Point pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        for (int i = 0; i < len(savedPageRects); i++) {
            if (savedTrayRect.Contains(pt) && savedPageRects[i].Contains(pt) && !savedCloseRects[i].Contains(pt)) {
                BeginSavedPageRename(i);
                return 0;
            }
        }
    }

    if (msg == WM_MOUSEHWHEEL || (msg == WM_MOUSEWHEEL && IsShiftPressed())) {
        if (savedContentDx > savedTrayRect.dx) {
            int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            int maxScroll = std::max(0, savedContentDx - savedTrayRect.dx);
            int next = std::clamp(savedScrollX - notches * DpiScale(hwnd, 48), 0, maxScroll);
            if (next != savedScrollX) {
                savedScrollX = next;
                HwndInvalidate(hwnd, false);
            }
            return 0;
        }
    }

    if (msg == WM_CHAR && savedRenamingIdx >= 0) {
        WCHAR ch = (WCHAR)wparam;
        if (ch == L'\r') {
            CommitSavedPageRename();
        } else if (ch == 27) {
            CancelSavedPageRename();
        } else if (ch == L'\b') {
            if (savedRenameSelectAll) {
                savedRenameText[0] = 0;
                savedRenameSelectAll = false;
            } else {
                int n = (int)wcslen(savedRenameText);
                if (n > 0) {
                    savedRenameText[n - 1] = 0;
                }
            }
            HwndInvalidate(hwnd, false);
        } else if (ch >= L' ') {
            if (savedRenameSelectAll) {
                savedRenameText[0] = 0;
                savedRenameSelectAll = false;
            }
            int n = (int)wcslen(savedRenameText);
            if (n < dimofi(savedRenameText) - 1) {
                savedRenameText[n] = ch;
                savedRenameText[n + 1] = 0;
                HwndInvalidate(hwnd, false);
            }
        }
        return 0;
    }

    if (msg == WM_CHAR && (pageEditing || zoomEditing)) {
        char ch = (char)wparam;
        if (ch == '\r') {
            CommitEdit();
        } else if (ch == 27) {
            CancelEdit();
        } else if (ch == '\b') {
            if (editSelectAll) {
                editText[0] = 0;
                editSelectAll = false;
                HwndInvalidate(hwnd, false);
                return 0;
            }
            int n = (int)strlen(editText);
            if (n > 0) {
                editText[n - 1] = 0;
                HwndInvalidate(hwnd, false);
            }
        } else if (ch >= '0' && ch <= '9') {
            if (editSelectAll) {
                editText[0] = 0;
                editSelectAll = false;
            }
            int n = (int)strlen(editText);
            if (n < dimofi(editText) - 1) {
                editText[n] = ch;
                editText[n + 1] = 0;
                HwndInvalidate(hwnd, false);
            }
        }
        return 0;
    }

    if (msg == WM_GETDLGCODE && (pageEditing || zoomEditing || savedRenamingIdx >= 0)) {
        return DLGC_WANTCHARS | DLGC_WANTALLKEYS;
    }

    if (msg == WM_KILLFOCUS) {
        if (pageEditing || zoomEditing) {
            CommitEdit();
            return 0;
        }
        if (savedRenamingIdx >= 0) {
            CommitSavedPageRename();
            return 0;
        }
    }

    return WndProcDefault(hwnd, msg, wparam, lparam);
}

HWND TopBarWnd::Create(MainWindow* w) {
    win = w;
    CreateCustomArgs cargs;
    cargs.parent = w->hwndFrame;
    cargs.style = WS_CHILD | WS_CLIPSIBLINGS;
    cargs.visible = false;
    CreateCustom(cargs);
    if (!hwnd) {
        return nullptr;
    }
    animTimer.Init(hwnd, kTopBarAnimTimerId);
    RebuildImageList();
    return hwnd;
}

void CreateTopBar(MainWindow* win) {
    if (win->topBarWnd) {
        return;
    }
    auto* bar = new TopBarWnd();
    if (!bar->Create(win)) {
        delete bar;
        return;
    }
    win->topBarWnd = bar;
    win->hwndTopBar = bar->hwnd;
}

void DestroyTopBar(MainWindow* win) {
    if (!win || !win->topBarWnd) {
        return;
    }
    delete win->topBarWnd;
    win->topBarWnd = nullptr;
    win->hwndTopBar = nullptr;
}

bool IsTouchChrome(MainWindow* win) {
    if (!win) {
        return false;
    }
    // Presentation is document-only. Regular fullscreen keeps the touch chrome
    // active so the sidebar stays styled, but the rail and top bar hide
    // themselves (see IsRailVisible / IsTopBarVisible) for a clean page view.
    if (win->presentation != PM_DISABLED) {
        return false;
    }
    return gGlobalPrefs->touchChrome;
}

bool IsTopBarVisible(MainWindow* win) {
    if (!win || !win->hwndTopBar) {
        return false;
    }
    // This is the document toolbar: with nothing open it has nothing to show
    // and was drawing as an empty band. Testing touchView alone was not enough,
    // because launching with no document leaves touchView at Doc while the
    // Library is on screen - going to Web and back was what "fixed" it, since
    // that finally set touchView.
    if (!win->IsDocLoaded()) {
        return false;
    }
    if (win->isFullScreen) {
        return false;
    }
    if (!win->isToolbarVisible) {
        return false;
    }
    if (win->touchView != TouchView::Doc) {
        return false;
    }
    return IsTouchChrome(win);
}

int GetTopBarDy(MainWindow* win) {
    if (!IsTopBarVisible(win)) {
        return 0;
    }
    // Layout() scales every control from the top-bar child HWND. Use that same
    // DPI source for the child height: on some Windows-on-ARM startup paths the
    // frame and a newly-created child briefly report different DPI values.
    return DpiScale(win->hwndTopBar, kTopBarDy);
}

void UpdateTopBarForWindow(MainWindow* win) {
    if (!win || !win->hwndTopBar) {
        return;
    }
    // the tray is built from the favorites store, which changes underneath us
    // (a tab switch, an edit in the favorites pane), so re-read it here rather
    // than only when the bookmark button adds a page
    if (win->topBarWnd) {
        win->topBarWnd->RefreshSavedPages();
    }
    HwndInvalidate(win->hwndTopBar, false);
}

void UpdateTopBarAfterThemeChange(MainWindow* win) {
    if (!win || !win->topBarWnd) {
        return;
    }
    win->topBarWnd->RebuildImageList();
    HwndInvalidate(win->hwndTopBar, true);
}

void ShowTouchDocumentPreview(MainWindow* win, HWND anchorHwnd, Rect anchorRect) {
    if (win && win->topBarWnd) {
        win->topBarWnd->PinPreview(anchorHwnd, anchorRect);
    }
}

void HoverTouchDocumentPreview(MainWindow* win, HWND anchorHwnd, Rect anchorRect, bool isOver) {
    if (!win || !win->topBarWnd) {
        return;
    }
    TopBarWnd* bar = win->topBarWnd;
    HWND barHwnd = bar->hwnd;
    if (!barHwnd) {
        return;
    }
    KillTimer(barHwnd, kPreviewHoverTimerId);
    if (isOver) {
        // Wait, like the rail does. Showing on the first pixel of hover made
        // the previews flash open while the pointer was only passing through.
        bar->pendingPreviewAnchor = anchorHwnd;
        bar->pendingPreviewRect = anchorRect;
        SetTimer(barHwnd, kPreviewHoverTimerId, kPreviewHoverDelayMs, nullptr);
    } else {
        bar->pendingPreviewAnchor = nullptr;
        bar->SchedulePreviewClose();
    }
}

void CloseTouchDocumentPreview(MainWindow* win) {
    if (!win || !win->topBarWnd || !win->topBarWnd->previewWnd) {
        return;
    }
    win->topBarWnd->CancelPreviewClose();
    win->topBarWnd->previewWnd->Hide();
}

void CloseTouchDocumentOverlays(MainWindow* win, bool commitEdits) {
    if (win && win->topBarWnd) {
        win->topBarWnd->CloseOverlays(commitEdits);
    }
}
