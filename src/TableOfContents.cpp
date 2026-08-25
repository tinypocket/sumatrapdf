/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/BitManip.h"
#include "base/Dpi.h"
#include "base/ScopedWin.h"
#include "base/File.h"
#include "base/UITask.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/LabelWithCloseWnd.h"
#include "wingui/Anim.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "Annotation.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "TouchMetrics.h"
#include "Rail.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "TopBar.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "base/Pixmap.h"
#include "Favorites.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"
#include "Accelerators.h"
#include "Theme.h"
#include "FilterHighlightDraw.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "SearchAndDDE.h"

static void LayoutTocContainer(MainWindow* win);
static void UpdateTocStickyHeader(MainWindow* win);
void UpdateTouchPanelMode(MainWindow* win);
void EngineMupdfGetAnnotations(EngineBase* engine, Vec<Annotation*>& annotsOut);

int TouchSidebarRowDy() {
    Str density = gGlobalPrefs->touchSidebarDensity;
    if (str::EqI(density, StrL("condensed"))) {
        return 34;
    }
    if (str::EqI(density, StrL("expanded"))) {
        return 52;
    }
    return kPanelRowDy;
}

static int TouchSidebarListRowDy() {
    return TouchSidebarRowDy() + 6;
}

static int TouchSearchResultRowDy() {
    // Search results always need room for both their page label and snippet.
    // Bookmark density preferences must not compress this two-line card.
    return std::max(TouchSidebarRowDy() + 18, 68);
}

static int TouchSearchResultsY(MainWindow* win) {
    return DpiScale(win->hwndTocBox, kPanelHeaderDy + kPanelFilterDy + 64) - win->touchPanelScrollY;
}

static Rect TouchSearchResultRect(MainWindow* win, int idx) {
    int stride = DpiScale(win->hwndTocBox, TouchSearchResultRowDy());
    int gap = DpiScale(win->hwndTocBox, 8);
    Rect client = HwndClientRect(win->hwndTocBox);
    return Rect{DpiScale(win->hwndTocBox, 12), TouchSearchResultsY(win) + idx * stride + gap / 2,
                client.dx - DpiScale(win->hwndTocBox, 24), stride - gap};
}

// When true, multi-highlight every TOC item that matches the current page
// (issue #4642). Easy to flip for comparison with single-selection behavior.
bool gShowAllMatchingTOC = true;

// set tooltip for this item but only if the text isn't fully shown
// TODO: I might have lost something in translation
static void TocCustomizeTooltip(TreeView::GetTooltipEvent* ev) {
    auto* treeView = ev->treeView;
    auto ti = ev->treeItem;
    auto* nm = ev->info;
    TocItem* tocItem = (TocItem*)ti;
    IPageDestination* link = tocItem->GetPageDestination();
    if (!link) {
        return;
    }
    Str path = PageDestGetValue(link);
    if (!path) {
        path = tocItem->title;
    }
    if (!path) {
        return;
    }
    const auto* k = link->GetKind();
    // TODO: TocItem from Chm contain other types
    // we probably shouldn't set TocItem::dest there
    if (k == kindDestinationScrollTo) {
        return;
    }
    if (k == kindDestinationNone) {
        return;
    }

    bool isOk = (k == kindDestinationLaunchURL) || (k == kindDestinationLaunchFile) ||
                (k == kindDestinationLaunchEmbedded) || (k == kindDestinationMupdf) || (k == kindDestinationDjVu) ||
                (k == kindDestinationAttachment);
    ReportIf(!isOk);

    str::Builder infotip;

    // Display the item's full label, if it's overlong
    Rect rcLine, rcLabel;
    treeView->GetItemRect(ev->treeItem, false, rcLine);
    treeView->GetItemRect(ev->treeItem, true, rcLabel);

    // TODO: this causes a duplicate. Not sure what changed
    if (false && rcLine.x + rcLine.dx + 2 < rcLabel.x + rcLabel.dx) {
        Str currInfoTip = treeView->treeModel->Text(ti);
        infotip.Append(currInfoTip);
        infotip.Append("\r\n");
    }

    if (kindDestinationLaunchEmbedded == k || kindDestinationAttachment == k) {
        TempStr tmp = fmt(_TRA("Attachment: %s").s, path);
        infotip.Append(tmp);
    } else {
        infotip.Append(path);
    }

    str::BufSet(nm->pszText, nm->cchTextMax, ToStr(infotip));
}

// Deferred TOC navigation must not hold raw TocItem* / IPageDestination*
// pointers: the TOC tree can be rebuilt or freed before the uitask runs
// (while tab->ctrl still matches), which caused UAF in HandleLink
// (crash 8bfe7adb1000001: EngineMupdf::HandleLink / dest->GetKind).

// Own a stable copy of the destination at post time. Engine-private kinds
// that hold fz_outline/fz_link (mupdf) are converted to scrollTo with
// page/rect/zoom already resolved on the TocItem/dest.
static IPageDestination* SnapshotDestForDeferredNav(IPageDestination* dest, int tocPageNo) {
    if (!dest) {
        return nullptr;
    }
    Kind k = dest->GetKind();
    if (k == kindDestinationLaunchURL) {
        Str url = ((PageDestinationURL*)dest)->url;
        if (!url) {
            url = PageDestGetValue(dest);
        }
        return url ? new PageDestinationURL(url) : nullptr;
    }
    if (k == kindDestinationLaunchFile) {
        auto* f = (PageDestinationFile*)dest;
        auto* copy = new PageDestinationFile(f->path, f->dest);
        copy->openInNewWindow = f->openInNewWindow;
        copy->rect = f->rect;
        return copy;
    }
    if (k == kindDestinationLaunchEmbedded || k == kindDestinationAttachment) {
        auto* p = (PageDestination*)dest;
        auto* copy = new PageDestination();
        copy->kind = k;
        copy->pageNo = p->pageNo;
        copy->rect = p->rect;
        copy->zoom = p->zoom;
        copy->value = str::Dup(p->value);
        copy->name = str::Dup(p->name);
        copy->embedObjNum = p->embedObjNum;
        return copy;
    }
    if (k == kindDestinationScrollTo) {
        int pageNo = PageDestGetPageNo(dest);
        if (pageNo <= 0) {
            pageNo = tocPageNo;
        }
        if (pageNo < 1) {
            logf("SnapshotDestForDeferredNav: skip scrollTo pageNo=%d (tocPageNo=%d)\n", PageDestGetPageNo(dest),
                 tocPageNo);
            return nullptr;
        }
        auto* copy = new PageDestination();
        copy->kind = k;
        copy->pageNo = pageNo;
        copy->rect = PageDestGetRect(dest);
        copy->zoom = PageDestGetZoom(dest);
        copy->value = str::Dup(PageDestGetValue(dest));
        copy->name = str::Dup(PageDestGetName(dest));
        return copy;
    }
    // mupdf, djvu, none → page navigation snapshot
    int pageNo = PageDestGetPageNo(dest);
    if (pageNo <= 0) {
        pageNo = tocPageNo;
    }
    if (pageNo < 1) {
        Str val = PageDestGetValue(dest);
        if (val && IsExternalUrl(val)) {
            return new PageDestinationURL(val);
        }
        logf("SnapshotDestForDeferredNav: skip dest kind pageNo=%d (tocPageNo=%d)\n", PageDestGetPageNo(dest),
             tocPageNo);
        return nullptr;
    }
    RectF r = PageDestGetRect(dest);
    float zoom = PageDestGetZoom(dest);
    if (k == kindDestinationMupdf) {
        // Prefer resolved anchor; outline x/y can be 0 and scroll to the wrong place
        RectF pt = PageDestGetDestPoint(dest);
        if ((r.dx == 0 && r.dy == 0) || (r.dx == kDestUseDefault && r.dy == kDestUseDefault)) {
            if (pt.x != 0 || pt.y != 0 || r.IsEmpty()) {
                r = RectF{pt.x, pt.y, kDestUseDefault, kDestUseDefault};
            }
        }
        zoom = dest->GetZoom2();
    }
    return NewSimpleDest(pageNo, r, zoom);
}

#if defined(DEBUG)
bool TableOfContents_UnitTestSnapshotNamedDest() {
    PageDestination source;
    source.kind = kindDestinationScrollTo;
    source.pageNo = 1;
    source.rect = RectF(2, 3, 4, 5);
    source.zoom = 125;
    source.value = str::Dup(StrL("value"));
    source.name = str::Dup(StrL("https://sumatrapdf.md/issue-5842.html#target-heading"));

    IPageDestination* snapshot = SnapshotDestForDeferredNav(&source, 7);
    bool ok = snapshot && snapshot->GetKind() == kindDestinationScrollTo && PageDestGetPageNo(snapshot) == 1 &&
              PageDestGetRect(snapshot) == source.rect && PageDestGetZoom(snapshot) == source.zoom &&
              str::Eq(PageDestGetValue(snapshot), source.value) && str::Eq(PageDestGetName(snapshot), source.name);
    delete snapshot;
    return ok;
}
#endif

static TocItem* FindTocItemByTitlePage(TocItem* item, Str title, int pageNo) {
    for (; item; item = item->next) {
        if (pageNo > 0 && item->pageNo != pageNo) {
            // keep searching children; same page can nest under different titles
        } else if (title && item->title && str::Eq(title, item->title)) {
            if (pageNo <= 0 || item->pageNo == pageNo) {
                return item;
            }
        } else if (!title && pageNo > 0 && item->pageNo == pageNo) {
            return item;
        }
        TocItem* found = FindTocItemByTitlePage(item->child, title, pageNo);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static TocItem* FindTocItemById(TocItem* item, int id) {
    for (; item; item = item->next) {
        if (item->id == id) {
            return item;
        }
        TocItem* found = FindTocItemById(item->child, id);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static int CountTocLeaves(TocItem* item) {
    int count = 0;
    for (; item; item = item->next) {
        if (item->child) {
            count += CountTocLeaves(item->child);
        } else {
            count++;
        }
    }
    return count;
}

static TempStr TocParentPathTemp(TocItem* item) {
    TocItem* chain[64]{};
    int count = 0;
    for (TocItem* parent = item ? item->parent : nullptr; parent && parent->parent && count < dimofi(chain);
         parent = parent->parent) {
        chain[count++] = parent;
    }
    str::Builder path;
    for (int i = count - 1; i >= 0; i--) {
        if (i != count - 1) {
            path.Append(StrL(" · "));
        }
        path.Append(chain[i]->title);
    }
    return str::DupTemp(ToStr(path));
}

struct GoToTocLinkData {
    WindowTab* tab = nullptr;
    DocController* ctrl = nullptr;
    // owned snapshot; may be null (then pageNo alone is used)
    IPageDestination* dest = nullptr;
    int pageNo = 0;
    // owned; used to re-select in tree after palette-driven nav
    Str title;
    // true when the navigation was driven from outside the tree (e.g. the
    // command palette), so afterwards we must move the tree's selection to the
    // item ourselves. For tree-driven navigation the tree is already selected.
    bool selectInTree = false;

    ~GoToTocLinkData() {
        delete dest;
        str::Free(title);
    }
};

// URL / file / embedded targets keep pageNo = -1 by design; only page-nav dests need pageNo >= 1.
static bool DestNeedsValidPageNo(IPageDestination* dest) {
    if (!dest) {
        return false;
    }
    Kind k = dest->GetKind();
    return k != kindDestinationLaunchURL && k != kindDestinationLaunchFile && k != kindDestinationLaunchEmbedded &&
           k != kindDestinationAttachment;
}

static GoToTocLinkData* NewGoToTocLinkData(MainWindow* win, TocItem* tocItem, bool selectInTree) {
    int pageNo = tocItem->pageNo;
    IPageDestination* dest = SnapshotDestForDeferredNav(tocItem->GetPageDestination(), pageNo);

    // drop page-navigation destinations that still have no valid page
    if (dest && DestNeedsValidPageNo(dest) && PageDestGetPageNo(dest) < 1) {
        logf("NewGoToTocLinkData: skip dest with pageNo=%d\n", PageDestGetPageNo(dest));
        delete dest;
        dest = nullptr;
    }

    // nothing to navigate to: no dest and no valid page number
    if (!dest && pageNo < 1) {
        logf("NewGoToTocLinkData: skip toc item pageNo=%d title='%s'\n", pageNo, tocItem->title);
        return nullptr;
    }

    auto* data = new GoToTocLinkData;
    data->ctrl = win->ctrl;
    data->tab = win->CurrentTab();
    data->pageNo = pageNo;
    data->dest = dest;
    data->selectInTree = selectInTree;
    if (selectInTree && tocItem->title) {
        data->title = str::Dup(tocItem->title);
    }
    return data;
}

static void GoToTocLink(GoToTocLinkData* d) {
    AutoDelete delData(d);

    auto* tab = d->tab;
    auto* ctrl = d->ctrl;

    // validate tab before dereferencing — it may have been freed
    // while this task was queued (e.g. user closed the tab/window)
    if (!IsWindowTabValid(tab)) {
        return;
    }
    MainWindow* win = tab->win;
    // destination snapshot is invalid if the DocController has been replaced
    if (!IsMainWindowValid(win) || win->CurrentTab() != tab || tab->ctrl != ctrl) {
        return;
    }

    // make sure that the tree item that the user selected
    // isn't unselected in UpdateTocSelection right again
    win->tocKeepSelection = true;
    if (d->dest) {
        ctrl->HandleLink(d->dest, win->linkHandler);
    } else if (d->pageNo > 0) {
        ctrl->GoToPage(d->pageNo, true);
    }
    win->tocKeepSelection = false;

    // when driven from the command palette the tree wasn't the source of the
    // navigation, so the page-based UpdateTocSelection was suppressed above and
    // the tree still shows the old item. Move the selection to this item now
    // (programmatic SelectItem doesn't re-navigate -- see TocTreeSelectionChanged).
    if (d->selectInTree && win->tocLoaded && win->tocTreeView) {
        TocTree* tree = tab->currToc;
        TocItem* tocItem = nullptr;
        if (tree && tree->root) {
            tocItem = FindTocItemByTitlePage(tree->root, d->title, d->pageNo);
        }
        if (tocItem) {
            TreeView* treeView = win->tocTreeView;
            HTREEITEM hi = treeView->GetHandleByTreeItem((TreeItem)tocItem);
            if (hi) {
                TreeView_EnsureVisible(treeView->hwnd, hi);
            }
            treeView->SelectItem((TreeItem)tocItem);
        }
    }
}

// navigate to a TocItem regardless of whether it points to a page in this
// document or to an external destination (used by the command palette, where
// the user explicitly picked the item so we always honor it)
void GoToTocItem(MainWindow* win, TocItem* tocItem) {
    if (!win || !tocItem) {
        return;
    }
    auto* data = NewGoToTocLinkData(win, tocItem, true);
    if (!data) {
        return;
    }
    auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
    uitask::Post(fn, "TaskGoToTocFromPalette");
}

static bool IsScrollToLink(IPageDestination* link) {
    if (!link) {
        return false;
    }
    const auto* kind = link->GetKind();
    return kind == kindDestinationScrollTo;
}

static void GoToTocTreeItem(MainWindow* win, TreeItem ti, bool allowExternal) {
    if (!ti) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    bool validPage = (tocItem->pageNo > 0);
    bool isScroll = IsScrollToLink(tocItem->GetPageDestination());
    if (validPage || (allowExternal || isScroll)) {
        // delay changing the page until the tree messages have been handled
        auto* data = NewGoToTocLinkData(win, tocItem, false);
        if (!data) {
            return;
        }
        auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
        uitask::Post(fn, "TaskGoToTocTreeItem");
    }
}

void ClearTocBox(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    // set tocLoaded to false before SetText("") because SetText triggers
    // EN_CHANGE synchronously which calls ApplyTocFilter() re-entrantly
    // and we need it to bail out early
    win->tocLoaded = false;

    win->tocTreeView->Clear();
    win->tocMatchingItems.Reset();

    // clear filter state
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    win->currPageNo = 0;
}

void ToggleTocBox(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    if (win->uiState.tocVisible) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        return;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    if (win->uiState.tocVisible) {
        HwndSetFocus(win->tocTreeView->hwnd);
    }
}

struct VistorForPageNoData {
    int pageNo = -1;

    TocItem* bestMatch = nullptr;
    int bestMatchPageNo = 0;
    int nItems = 0;
};

static void visitTree(VistorForPageNoData* d, TreeItemVisitorData* vd) {
    auto* tocItem = (TocItem*)vd->item;
    if (!tocItem) {
        return;
    }
    if (!d->bestMatch) {
        // if nothing else matches, match the root node
        d->bestMatch = tocItem;
    }
    ++d->nItems;
    int page = tocItem->pageNo;
    if ((page <= d->pageNo) && (page >= d->bestMatchPageNo) && (page >= 1)) {
        d->bestMatch = tocItem;
        d->bestMatchPageNo = page;
        if (d->pageNo == d->bestMatchPageNo) {
            // we can stop earlier if we found the exact match
            vd->stopTraversal = true;
            return;
        }
    }
}

// find the closest item in tree view to a given page number
static TocItem* TreeItemForPageNo(TreeView* treeView, int pageNo) {
    TreeModel* tm = treeView->treeModel;
    if (!tm) {
        return 0;
    }
    VistorForPageNoData d;
    d.pageNo = pageNo;
    auto fn = MkFunc1<VistorForPageNoData, TreeItemVisitorData*>(visitTree, &d);
    VisitTreeModelItems(tm, fn);
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (d.nItems < 2) {
        return 0;
    }
    return d.bestMatch;
}

struct CollectSamePageData {
    int pageNo = 0;
    Vec<TocItem*>* out = nullptr;
};

static void visitCollectSamePage(CollectSamePageData* d, TreeItemVisitorData* vd) {
    auto* tocItem = (TocItem*)vd->item;
    if (!tocItem || tocItem->pageNo < 1) {
        return;
    }
    if (tocItem->pageNo == d->pageNo) {
        d->out->Append(tocItem);
    }
}

static bool TocMatchingItemsContains(const Vec<TocItem*>& items, TocItem* item) {
    for (TocItem* t : items) {
        if (t == item) {
            return true;
        }
    }
    return false;
}

// Fill win->tocMatchingItems with every entry that should look "current" for
// bestMatch: all TOC items on the same page, plus the ancestor chain (so a
// nested 6 / 6.1 / 6.1.1 path all highlight together). TreeView still has only
// one selection; extras are painted in OnTocCustomDraw when gShowAllMatchingTOC.
static void SetTocMultiHighlight(MainWindow* win, TreeView* treeView, TocItem* bestMatch) {
    win->tocMatchingItems.Reset();
    if (!gShowAllMatchingTOC || !bestMatch || !treeView) {
        return;
    }

    // All bookmarks that point at the same page as the best match (the issue's
    // "subsequent" same-page entries that TreeView single-select cannot show).
    if (bestMatch->pageNo >= 1 && treeView->treeModel) {
        CollectSamePageData d;
        d.pageNo = bestMatch->pageNo;
        d.out = &win->tocMatchingItems;
        auto fn = MkFunc1<CollectSamePageData, TreeItemVisitorData*>(visitCollectSamePage, &d);
        VisitTreeModelItems(treeView->treeModel, fn);
    }

    // Ancestor chain (chapter → section → subsection), including bestMatch.
    for (TocItem* p = bestMatch; p; p = p->parent) {
        if (!TocMatchingItemsContains(win->tocMatchingItems, p)) {
            win->tocMatchingItems.Append(p);
        }
    }

    // TreeView selection paint won't cover the extra matches; repaint so
    // OnTocCustomDraw can draw them.
    if (treeView->hwnd) {
        HwndInvalidate(treeView->hwnd, true);
    }
}

static bool TocItemIsMultiHighlight(MainWindow* win, TocItem* item) {
    if (!gShowAllMatchingTOC || !win || !item) {
        return false;
    }
    return TocMatchingItemsContains(win->tocMatchingItems, item);
}

// TODO: I can't use TreeItem->IsExpanded() because it's not in sync with
// the changes user makes to TreeCtrl
static TocItem* FindVisibleParentTreeItem(TreeView* treeView, TocItem* ti) {
    if (!ti) {
        return nullptr;
    }
    while (true) {
        auto* parent = ti->parent;
        if (parent == nullptr) {
            // ti is a root node
            return ti;
        }
        if (treeView->IsExpanded((TreeItem)parent)) {
            return ti;
        }
        ti = parent;
    }
    return nullptr;
}

void UpdateTocSelection(MainWindow* win, int currPageNo) {
    auto* treeView = win->tocTreeView;
    if (!win->tocLoaded || !win->uiState.tocVisible || !treeView) {
        return;
    }

    auto* item = TreeItemForPageNo(treeView, currPageNo);
    if (win->tocKeepSelection) {
        // the tree selection is deliberately left alone: the user clicked a
        // bookmark and GoToTocLink set tocKeepSelection so the page change
        // doesn't move the selection off it. The multi-match "current page"
        // highlight is a different thing though and must still follow the page,
        // otherwise the previous page's highlight stays until the sidebar is
        // rebuilt.
        SetTocMultiHighlight(win, treeView, item);
        return;
    }

    // only select the items that are visible i.e. are top nodes or
    // children of expanded node
    TreeItem toSelect = (TreeItem)FindVisibleParentTreeItem(treeView, item);
    treeView->SelectItem(toSelect);
    SetTocMultiHighlight(win, treeView, item);
}

// expand the table of contents tree down to the entry matching the current
// page, then select and scroll to it (issue #1998, like Explorer's
// "Expand to current folder")
void ExpandTocToCurrentPage(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    // make sure the bookmarks (table of contents) sidebar is visible
    if (!win->uiState.tocVisible) {
        SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    }
    if (!win->tocLoaded || !win->uiState.tocVisible) {
        return;
    }
    TreeView* treeView = win->tocTreeView;
    int currPageNo = win->ctrl->CurrentPageNo();
    TocItem* item = TreeItemForPageNo(treeView, currPageNo);
    if (!item) {
        return;
    }
    HTREEITEM hi = treeView->GetHandleByTreeItem((TreeItem)item);
    if (!hi) {
        return;
    }
    // TreeView_EnsureVisible expands any collapsed ancestors and scrolls the
    // item into view, which is exactly the "expand to current page" behavior
    TreeView_EnsureVisible(treeView->hwnd, hi);
    treeView->SelectItem((TreeItem)item);
    SetTocMultiHighlight(win, treeView, item);
    HwndSetFocus(treeView->hwnd);
}

static void UpdateDocTocExpansionStateRecur(TreeView* treeView, Vec<int>& tocState, TocItem* tocItem) {
    while (tocItem) {
        // items without children cannot be toggled
        if (tocItem->child) {
            // we have to query the state of the tree view item because
            // isOpenToggled is not kept in sync
            // TODO: keep toggle state on TocItem in sync
            // by subscribing to the right notifications
            bool isExpanded = treeView->IsExpanded((TreeItem)tocItem);
            bool wasToggled = isExpanded != tocItem->isOpenDefault;
            if (wasToggled) {
                tocState.Append(tocItem->id);
            }
            UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem->child);
        }
        tocItem = tocItem->next;
    }
}

void UpdateTocExpansionState(Vec<int>& tocState, TreeView* treeView, TocTree* docTree) {
    if (treeView->treeModel != docTree) {
        // CrashMe();
        return;
    }
    tocState.Reset();
    TocItem* tocItem = docTree->root->child;
    UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}
// clang-format off

static void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        TempWStr ws = ToWStrTemp(node->title);
        for (int i = 0; i < ws.len; i++) {
            WCHAR c = ws.s[i];
            if (isLeftToRightChar(c)) {
                l2r++;
            } else if (isRightToLeftChar(c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

static void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        item->isOpenToggled = tocState.Contains(item->id);
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

static void AddFavoriteFromToc(MainWindow* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }
    Str name = dti->title;
    TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

static void SaveAttachment(WindowTab* tab, Str fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    Str data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (len(data) == 0) {
        return;
    }
    TempStr dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data);
}

static void OpenAttachment(WindowTab* tab, Str fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    Str data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (len(data) == 0) {
        return;
    }
    MainWindow* win = tab->win;
    EngineBase* newEngine = CreateEngineMupdfFromData(data, fileName, nullptr);
    DocController* ctrl = CreateControllerForEngineOrFile(newEngine, nullptr, nullptr, win);
    LoadArgs* args = new LoadArgs(tab->filePath, win);
    args->SetDisplayName(fileName);
    args->ctrl = ctrl;
    LoadDocumentFinish(args);
    str::Free(data);
}

static void OpenEmbeddedFile(WindowTab* tab, IPageDestination* dest) {
    ReportIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    MainWindow* win = tab->win;
    PageDestinationFile *destFile = (PageDestinationFile*)dest;
    Str path = destFile->path;
    Str tabPath = tab->filePath;
    if (!str::StartsWith(path, tabPath)) {
        return;
    }
    LoadArgs args(path, win);
    args.activateExisting = true;
    args.activateExistingInWindow = true;
    LoadDocument(&args);
}

static void SaveEmbeddedFile(WindowTab* tab, Str srcPath, Str fileName) {
    Str data = LoadEmbeddedPDFFile(srcPath);
    if (len(data) == 0) {
        // TODO: show an error message
        return;
    }
    TempStr dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data);
}

// Expand outline nodes whose depth is < maxDepth (depth 1 = top-level rows).
// Call after a full collapse so the tree ends at exactly that level.
static void TocExpandItemsToDepth(HWND hwnd, HTREEITEM item, int depth, int maxDepth) {
    while (item) {
        if (depth < maxDepth) {
            TreeView_Expand(hwnd, item, TVE_EXPAND);
            HTREEITEM child = TreeView_GetChild(hwnd, item);
            if (child) {
                TocExpandItemsToDepth(hwnd, child, depth + 1, maxDepth);
            }
        }
        item = TreeView_GetNextSibling(hwnd, item);
    }
}

// Expand outline only through `level` (1 = top-level rows collapsed, 2 = expand
// top-level once, 3 = two levels deep). Issue #5239.
static void TocExpandToLevel(TreeView* tv, int level) {
    if (!tv || !tv->hwnd || level < 1) {
        return;
    }
    HWND hwnd = tv->hwnd;
    tv->SuspendRedraw();
    HTREEITEM root = TreeView_GetRoot(hwnd);
    TreeViewExpandRecursively(hwnd, root, TVE_COLLAPSE, false);
    if (level > 1) {
        TocExpandItemsToDepth(hwnd, root, 1, level);
    }
    tv->ResumeRedraw();
}

// Collapse all; if there is a single top-level entry with children (typical
// Word-export TOC), expand it one level so Collapse All is useful (#5239).
static void TocCollapseAll(TreeView* tv) {
    if (!tv || !tv->hwnd) {
        return;
    }
    TocExpandToLevel(tv, 1);
    HWND hwnd = tv->hwnd;
    HTREEITEM root = TreeView_GetRoot(hwnd);
    if (root && !TreeView_GetNextSibling(hwnd, root) && TreeView_GetChild(hwnd, root)) {
        TreeView_Expand(hwnd, root, TVE_EXPAND);
    }
}

// Collapse every outline row that shares the parent of `ti` (same nesting level
// / siblings). If `ti` is null, use the current selection; if still none, all
// top-level rows. Issue #1895.
static void TocCollapseSameLevel(TreeView* tv, TreeItem ti) {
    if (!tv || !tv->hwnd) {
        return;
    }
    HWND hwnd = tv->hwnd;
    HTREEITEM hItem = TreeModel::kNullItem != ti ? tv->GetHandleByTreeItem(ti) : nullptr;
    if (!hItem) {
        hItem = TreeView_GetSelection(hwnd);
    }
    HTREEITEM first = nullptr;
    if (hItem) {
        HTREEITEM parent = TreeView_GetParent(hwnd, hItem);
        first = parent ? TreeView_GetChild(hwnd, parent) : TreeView_GetRoot(hwnd);
    } else {
        first = TreeView_GetRoot(hwnd);
    }
    if (!first) {
        return;
    }
    tv->SuspendRedraw();
    for (HTREEITEM sibling = first; sibling; sibling = TreeView_GetNextSibling(hwnd, sibling)) {
        if (TreeView_GetChild(hwnd, sibling)) {
            TreeView_Expand(hwnd, sibling, TVE_COLLAPSE);
        }
    }
    tv->ResumeRedraw();
}

// clang-format off
static MenuDef menuDefContextToc[] = {
    {
        _TRN("Expand All"),
        CmdExpandAll,
    },
    {
        _TRN("Collapse All"),
        CmdCollapseAll,
    },
    {
        _TRN("Expand to Level 1"),
        CmdTocExpandToLevel1,
    },
    {
        _TRN("Expand to Level 2"),
        CmdTocExpandToLevel2,
    },
    {
        _TRN("Expand to Level 3"),
        CmdTocExpandToLevel3,
    },
    {
        _TRN("Collapse Same Level"),
        CmdTocCollapseSameLevel,
    },
    {
        _TRN("Expand to Current Page"),
        CmdExpandToCurrentPage,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Open Embedded PDF"),
        CmdOpenEmbeddedPDF,
    },
    {
        _TRN("Save Embedded File..."),
        CmdSaveEmbeddedFile,
    },
    {
        _TRN("Open Attachment"),
        CmdOpenAttachment,
    },
    {
        _TRN("Save Attachment..."),
        CmdSaveAttachment,
    },
    // note: strings cannot be "" or else items are not there
    {
        "Add to favorites",
        CmdFavoriteAdd,
    },
    {
        "Remove from favorites",
        CmdFavoriteDel,
    },
    {
        nullptr,
        0,
    },
};
// clang-format on

static void TocContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    Str filePath = win->ctrl->GetFilePath();

    Point pt{};

    TreeItem ti = GetOrSelectTreeItemAtPos(ev, pt);
    if (ti == TreeModel::kNullItem) {
        pt = {ev->mouseScreen.x, ev->mouseScreen.y};
    }
    int pageNo = 0;
    TocItem* dti = (TocItem*)ti;
    IPageDestination* dest = dti ? dti->dest : nullptr;
    if (dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }

    WindowTab* tab = win->CurrentTab();
    HMENU popup = BuildMenuFromDef(menuDefContextToc, CreatePopupMenu(), nullptr);

    Str path;
    Str fileName;
    Kind destKind = dest ? dest->GetKind() : nullptr;

    // TODO: this is pontentially not used at all
    if (destKind == kindDestinationLaunchEmbedded) {
        auto* embeddedFile = (PageDestinationFile*)dest;
        // this is a path to a file on disk, e.g. a path to opened PDF
        // with the embedded stream number
        path = embeddedFile->path;
        // this is name of the file as set inside PDF file
        fileName = PageDestGetName(dest);
        bool canOpenEmbedded = str::EndsWithI(fileName, StrL(".pdf"));
        if (!canOpenEmbedded) {
            MenuRemove(popup, CmdOpenEmbeddedPDF);
        }
    } else {
        // TODO: maybe move this to BuildMenuFromMenuDef
        MenuRemove(popup, CmdSaveEmbeddedFile);
        MenuRemove(popup, CmdOpenEmbeddedPDF);
    }

    int attachmentNo = -1;
    if (destKind == kindDestinationAttachment) {
        auto* attachment = (PageDestinationFile*)dest;
        // this is a path to a file on disk, e.g. a path to opened PDF
        // with the embedded stream number
        path = attachment->path;
        // this is name of the file as set inside PDF file
        fileName = PageDestGetName(dest);
        // hack: attachmentNo is saved in pageNo see
        // PdfLoadAttachments and DestFromAttachment
        attachmentNo = pageNo;
        bool canOpenEmbedded = str::EndsWithI(fileName, StrL(".pdf"));
        if (!canOpenEmbedded) {
            MenuRemove(popup, CmdOpenAttachment);
        }
    } else {
        // TODO: maybe move this to BuildMenuFromMenuDef
        MenuRemove(popup, CmdSaveAttachment);
        MenuRemove(popup, CmdOpenAttachment);
    }

    if (pageNo > 0) {
        TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
        bool isBookmarked = IsPageInFavorites(filePath, pageNo);
        if (isBookmarked) {
            MenuRemove(popup, CmdFavoriteAdd);

            // %s and not %d because re-using translation from RebuildFavMenu()
            Str tr = _TRA("Remove page %s from favorites");
            TempStr s = fmt(tr.s, pageLabel);
            MenuSetText(popup, CmdFavoriteDel, s);
        } else {
            MenuRemove(popup, CmdFavoriteDel);
            // %s and not %d because re-using translation from RebuildFavMenu()
            TempStr s = fmt(_TRA("Add page %s to favorites").s, pageLabel);
            s = AppendAccelKeyToMenuStringTemp(s, CmdFavoriteAdd);
            MenuSetText(popup, CmdFavoriteAdd, s);
        }
    } else {
        MenuRemove(popup, CmdFavoriteAdd);
        MenuRemove(popup, CmdFavoriteDel);
    }
    RemoveBadMenuSeparators(popup);
    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmd) {
        case CmdExpandAll:
            win->tocTreeView->ExpandAll();
            break;
        case CmdCollapseAll:
            TocCollapseAll(win->tocTreeView);
            break;
        case CmdTocExpandToLevel1:
            TocExpandToLevel(win->tocTreeView, 1);
            break;
        case CmdTocExpandToLevel2:
            TocExpandToLevel(win->tocTreeView, 2);
            break;
        case CmdTocExpandToLevel3:
            TocExpandToLevel(win->tocTreeView, 3);
            break;
        case CmdTocCollapseSameLevel:
            TocCollapseSameLevel(win->tocTreeView, ti);
            break;
        case CmdExpandToCurrentPage:
            ExpandTocToCurrentPage(win);
            break;
        case CmdFavoriteAdd:
            AddFavoriteFromToc(win, dti);
            break;
        case CmdFavoriteDel:
            DelFavorite(filePath, pageNo);
            break;
        case CmdSaveEmbeddedFile: {
            SaveEmbeddedFile(tab, path, fileName);
        } break;
        case CmdOpenEmbeddedPDF:
            // TODO: maybe also allow for a fileName hint
            OpenEmbeddedFile(tab, dest);
            break;
        case CmdSaveAttachment: {
            SaveAttachment(tab, fileName, attachmentNo);
            break;
        }
        case CmdOpenAttachment: {
            OpenAttachment(tab, fileName, attachmentNo);
        }
    }
}

static void OnTocCustomDraw(TreeView::CustomDrawEvent* /*ev*/);

// auto-expand root level ToC nodes if there are at most two
static void AutoExpandTopLevelItems(TocItem* root) {
    if (!root) {
        return;
    }
    if (root->next && root->next->next) {
        return;
    }

    if (!root->IsExpanded()) {
        root->isOpenToggled = !root->isOpenToggled;
    }
    if (!root->next) {
        return;
    }
    if (!root->next->IsExpanded()) {
        root->next->isOpenToggled = !root->next->isOpenToggled;
    }
}

void LoadTocTree(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        ReportIf(true);
        return;
    }

    if (win->tocLoaded) {
        return;
    }

    win->tocLoaded = true;

    // clear filter when loading new toc
    // null out currToc first so that SetText("") callback doesn't use stale pointer
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    tab->currToc = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    auto* tocTree = tab->ctrl->GetToc();
    if (!tocTree || !tocTree->root) {
        return;
    }

    tab->currToc = tocTree;

    // consider a ToC tree right-to-left if a more than half of the
    // alphabetic characters are in a right-to-left script
    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeView* treeView = win->tocTreeView;
    HWND hwnd = treeView->hwnd;
    HwndSetRtl(hwnd, isRTL);

    UpdateControlsColors(win);
    SetInitialExpandState(tocTree->root, tab->tocState);
    AutoExpandTopLevelItems(tocTree->root->child);

    treeView->SetTreeModel(tocTree);

    treeView->onCustomDraw = MkFunc1Void(OnTocCustomDraw);
    LayoutTocContainer(win);
    // uint fl = RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN;
    // RedrawWindow(hwnd, nullptr, nullptr, fl);
}

// TODO: use https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getobject?redirectedfrom=MSDN
// to get LOGFONT from existing font and then create a derived font
static void UpdateFont(HDC hdc, HWND hwnd, int fontFlags) {
    bool italic = bit::IsSet(fontFlags, fontBitItalic);
    bool bold = bit::IsSet(fontFlags, fontBitBold);
    HFONT hfont = GetAppTreeFontEx(hwnd, bold, italic);
    SelectObject(hdc, hfont);
}

static void GetTocFilterWords(MainWindow* win, StrVec& wordsOut) {
    wordsOut.Reset();
    if (!win || !win->tocFilterEdit) {
        return;
    }
    TempStr filter = win->tocFilterEdit->GetTextTemp();
    if (filter) {
        SplitFilterToWords(filter, wordsOut);
    }
}

static bool HasTocFilter(MainWindow* win) {
    StrVec words;
    GetTocFilterWords(win, words);
    return len(words) > 0;
}

// "3 sections" under a row title, as in the redesign. Only rows that have
// children get one.
static int CountTocChildren(TocItem* ti) {
    int n = 0;
    for (TocItem* c = ti ? ti->child : nullptr; c; c = c->next) {
        n++;
    }
    return n;
}

static int TocItemDepth(TocItem* item) {
    int depth = 0;
    while (item && item->parent && item->parent->parent) {
        depth++;
        item = item->parent;
    }
    return depth;
}

// the redesigned panel draws a selected row as a rounded pill in the accent
// tint, matching the rail's active button
static bool TocUsesRedesignedRows(MainWindow* win) {
    return IsTouchChrome(win);
}

static void TocSelectionColors(COLORREF* bgOut, COLORREF* txtOut) {
    ThemeAccentSurfaceColors(bgOut, txtOut);
}

// 4a's disclosure chevron: a right-pointing '>' when collapsed, a down '⌄'
// when expanded, drawn as two antialiased strokes centered in `box`.
static void DrawTocChevron(HDC hdc, const Rect& box, bool expanded, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (float)box.x + box.dx / 2.0f;
    float cy = (float)box.y + box.dy / 2.0f;
    // box is already DPI-scaled, so derive the glyph size from it
    float d = box.dx / 5.0f;
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(col), std::max(1.5f, box.dx / 14.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF pts[3];
    if (expanded) {
        // v : down chevron
        pts[0] = {cx - d, cy - d / 2};
        pts[1] = {cx, cy + d / 2};
        pts[2] = {cx + d, cy - d / 2};
    } else {
        // > : right chevron
        pts[0] = {cx - d / 2, cy - d};
        pts[1] = {cx + d / 2, cy};
        pts[2] = {cx - d / 2, cy + d};
    }
    g.DrawLines(&pen, pts, 3);
}

static void DrawTocHierarchyGuides(HDC hdc, HWND hwnd, const Rect& row, int depth) {
    if (depth <= 0) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    COLORREF guideCol = ThemeEdgeColor();
    Gdiplus::Pen pen(GdiRgbFromCOLORREF(guideCol), std::max(1.0f, (float)DpiScale(hwnd, 1)));
    float inset = (float)DpiScale(hwnd, kPanelRowPadX);
    float step = (float)DpiScale(hwnd, kPanelIndentDx);
    float center = (float)DpiScale(hwnd, kPanelChevronDx) / 2.0f;
    float top = (float)row.y;
    float bottom = (float)(row.y + row.dy);
    float mid = top + row.dy / 2.0f;

    // Keep every ancestor rail visible through this row. The final rail bends
    // into the current level, like a compact discussion-thread hierarchy.
    for (int level = 0; level < depth - 1; level++) {
        float x = inset + level * step + center;
        g.DrawLine(&pen, x, top, x, bottom);
    }
    float parentX = inset + (depth - 1) * step + center;
    float currentX = inset + depth * step + center;
    float radius = std::min(step / 2.0f, (float)DpiScale(hwnd, 8));
    Gdiplus::GraphicsPath path;
    path.StartFigure();
    path.AddLine(parentX, top, parentX, mid - radius);
    path.AddBezier(parentX, mid - radius, parentX, mid, parentX + radius, mid, currentX, mid);
    g.DrawPath(&pen, &path);
}

// POSTPAINT: redraw title (optional filter highlight), optional right-aligned
// page label, and multi-match "current page" highlight (issue #4642).
static void FillTocPill(HDC hdc, const Rect& r, int radius, COLORREF col, COLORREF borderCol = kColorUnset,
                        int borderWidth = 1) {
    int d = std::min(radius * 2, std::min(r.dx, r.dy));
    AutoDeleteBrush br = CreateSolidBrush(col);
    // the filter field is the same color as the panel, so only a border makes
    // it readable as a field
    AutoDeletePen pen = CreatePen(PS_SOLID, borderWidth, borderCol == kColorUnset ? col : borderCol);
    ScopedSelectObject selBr(hdc, br);
    ScopedSelectObject selPen(hdc, pen);
    RoundRect(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy, d, d);
}

static void DrawTocItemPostPaint(TreeView::CustomDrawEvent* ev, MainWindow* win) {
    TocItem* tocItem = (TocItem*)ev->treeItem;
    if (!tocItem || !tocItem->title) {
        return;
    }

    TreeView* tv = ev->treeView;
    Rect labelRect{};
    if (!tv->GetItemRect(ev->treeItem, true, labelRect)) {
        return;
    }
    Rect itemRect{};
    tv->GetItemRect(ev->treeItem, false, itemRect);

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    HDC hdc = tvcd->nmcd.hdc;
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    if (cd->rc.right <= cd->rc.left || cd->rc.bottom <= cd->rc.top) {
        return;
    }

    // POSTPAINT often omits CDIS_SELECTED; also check the control selection.
    bool isTreeSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    if (!isTreeSelected) {
        HTREEITEM hSel = TreeView_GetSelection(tv->hwnd);
        HTREEITEM hItem = tv->GetHandleByTreeItem(ev->treeItem);
        isTreeSelected = hSel && hItem && hSel == hItem;
    }
    bool isMultiMatch = TocItemIsMultiHighlight(win, tocItem);
    // Treat multi-match rows like selected for fill/text colors so every
    // current bookmark is visible, not only the TreeView selection.
    bool isSelected = isTreeSelected || isMultiMatch;
    // Focus ring / highlight-text only for the real tree selection.
    bool hasFocus = isTreeSelected && (GetFocus() == tv->hwnd);
    COLORREF bgCol, txtCol;
    ResolveTreeFilterItemColors(hdc, itemRect, tv->bgColor, tv->textColor, isSelected, hasFocus, &bgCol, &txtCol);
    // Per-bookmark color from the document (when not the focused selection).
    if (!(isTreeSelected && hasFocus) && tocItem->color != kColorUnset) {
        txtCol = tocItem->color;
    }

    bool showPage = gGlobalPrefs->showTocPageNumbers && win && win->IsDocLoaded() && win->ctrl && tocItem->pageNo > 0;
    TempStr pageLabel{};
    if (showPage) {
        pageLabel = win->ctrl->GetPageLabeTemp(tocItem->pageNo);
        if (!pageLabel) {
            showPage = false;
        }
    }

    StrVec words;
    GetTocFilterWords(win, words);
    bool filterActive = len(words) > 0;

    // Always repaint selected / multi-match rows so themed selection colors
    // replace Explorer's light inactive-selection face (issue #5848). Also
    // when page numbers or filter bars need drawing.
    if (!showPage && !filterActive && !isSelected) {
        return;
    }

    // Label area extends to the visible right edge so the page number stays
    // right-aligned against the sidebar, not under a long title.
    RECT drawRc = ToRECT(labelRect);
    drawRc.right = std::min(itemRect.x + itemRect.dx, (int)cd->rc.right);
    if (drawRc.right <= drawRc.left) {
        return;
    }

    if (tocItem->fontFlags != 0) {
        UpdateFont(hdc, tv->hwnd, tocItem->fontFlags);
    }
    HFONT font = (HFONT)SendMessageW(tv->hwnd, WM_GETFONT, 0, 0);
    if (tocItem->fontFlags == 0 && font) {
        SelectObject(hdc, font);
    }

    TempWStr pageW{};
    Size pageSize{};
    int pageReserve = 0;
    if (showPage) {
        pageW = ToWStrTemp(pageLabel);
        if (pageW.len > 0) {
            pageSize = HdcGetTextExtentPoint32(hdc, pageLabel);
            // reserve the number's width plus its right padding so a long
            // title ellipsizes before it reaches the number
            int pageGap = TocUsesRedesignedRows(win) ? (kPanelRowPadX + 8) : 8;
            pageReserve = pageSize.dx + DpiScale(tv->hwnd, pageGap);
        } else {
            showPage = false;
        }
    }

    Rect drawRect = ToRect(drawRc);
    bool roundedRow = isSelected && TocUsesRedesignedRows(win);
    if (TocUsesRedesignedRows(win)) {
        int left = DpiScale(tv->hwnd, kPanelRowPadX + TocItemDepth(tocItem) * kPanelIndentDx);
        int right = drawRect.x + drawRect.dx;
        drawRect.x = left;
        drawRect.dx = std::max(0, right - left);
        // Clear the whole row to the panel background first: the system paints
        // the item text at the un-shifted position and it would otherwise bleed
        // out to the left of our redrawn, gutter-shifted title (esp. on the
        // selected row). Then draw the accent pill for the selection.
        Rect full = ToRect(cd->rc);
        HdcFillRect(hdc, full, tv->bgColor);
        if (roundedRow) {
            TocSelectionColors(&bgCol, &txtCol);
            Rect pill = full;
            pill.Inflate(-DpiScale(tv->hwnd, 12), -DpiScale(tv->hwnd, 1));
            FillTocPill(hdc, pill, DpiScale(tv->hwnd, kPanelRowRadius), bgCol);
        }
        DrawTocHierarchyGuides(hdc, tv->hwnd, full, TocItemDepth(tocItem));
    } else {
        HBRUSH brushBg = CreateSolidBrush(bgCol);
        HdcFillRect(hdc, drawRect, brushBg);
        DeleteObject(brushBg);
    }

    Rect titleRect = drawRect;
    titleRect.dx = std::max(0, titleRect.dx - pageReserve);
    titleRect.Inflate(-2, -1);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txtCol);
    SetBkColor(hdc, bgCol);

    if (filterActive && TocUsesRedesignedRows(win)) {
        Rect full = ToRect(cd->rc);
        Rect resultTitle{DpiScale(tv->hwnd, 16), full.y + DpiScale(tv->hwnd, 5),
                         full.dx - DpiScale(tv->hwnd, 32) - pageReserve, DpiScale(tv->hwnd, 21)};
        HFONT resultFont = HdcGetUiFont(hdc, 15, FW_MEDIUM);
        SelectObject(hdc, resultFont);
        DrawTreeItemFilterHighlight(hdc, resultTitle, tocItem->title, words, bgCol, txtCol, resultFont);

        WindowTab* tab = win ? win->CurrentTab() : nullptr;
        TocItem* original = tab && tab->currToc ? FindTocItemById(tab->currToc->root, tocItem->id) : nullptr;
        TempStr parentPath = TocParentPathTemp(original);
        if (parentPath) {
            Rect parentRect{resultTitle.x, full.y + DpiScale(tv->hwnd, 26), resultTitle.dx, DpiScale(tv->hwnd, 20)};
            SetTextColor(hdc, roundedRow ? txtCol : ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, parentPath, parentRect,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS | DT_LEFT, HdcGetUiFont(hdc, 13));
        }
        if (showPage && pageW.len > 0) {
            Rect client = HwndClientRect(tv->hwnd);
            int right = client.dx - DpiScale(tv->hwnd, 16);
            Rect pageRect{right - pageSize.dx, full.y, pageSize.dx, full.dy};
            // TreeView can clip a flat filtered leaf to its text bounds during
            // ITEMPOSTPAINT. The page-number column intentionally lives at the
            // far edge of the full row, so widen the clip to that row while it
            // is drawn.
            int savedDc = SaveDC(hdc);
            SelectClipRgn(hdc, nullptr);
            IntersectClipRect(hdc, 0, full.y, client.dx, full.y + full.dy);
            SetTextColor(hdc, roundedRow ? txtCol : ThemeWindowDarkerTextColor());
            HdcDrawTextTabular(hdc, pageLabel, pageRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
            RestoreDC(hdc, savedDc);
        }
        return;
    }

    int nChildren = CountTocChildren(tocItem);
    // reserve a chevron gutter at the left of every touch row so parent and
    // leaf titles line up; parents draw a chevron into it
    if (TocUsesRedesignedRows(win)) {
        int chevronW = DpiScale(tv->hwnd, kPanelChevronDx);
        int gap = DpiScale(tv->hwnd, 8);
        if (nChildren > 0) {
            bool expanded = tv->IsExpanded(ev->treeItem);
            Rect chevBox{titleRect.x, drawRect.y, chevronW, drawRect.dy};
            COLORREF chevCol = roundedRow ? txtCol : ThemeWindowDarkerTextColor();
            DrawTocChevron(hdc, chevBox, expanded, chevCol);
        }
        titleRect.x += chevronW + gap;
        titleRect.dx = std::max(0, titleRect.dx - (chevronW + gap));
    }
    if (TocUsesRedesignedRows(win)) {
        int depth = TocItemDepth(tocItem);
        int size = depth == 0 ? 14 : (depth == 1 ? 13 : (depth == 2 ? 12 : 13));
        int weight = depth == 0 ? FW_SEMIBOLD : FW_MEDIUM;
        font = HdcGetUiFont(hdc, size, weight);
        SelectObject(hdc, font);
    }

    if (filterActive) {
        DrawTreeItemFilterHighlight(hdc, titleRect, tocItem->title, words, bgCol, txtCol, font);
    } else {
        HdcDrawText(hdc, tocItem->title, titleRect,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS | DT_LEFT);
    }

    if (showPage && pageW.len > 0) {
        Rect pageRect = drawRect;
        pageRect.Inflate(-2, -1);
        int right = pageRect.x + pageRect.dx;
        if (roundedRow || TocUsesRedesignedRows(win)) {
            // 4a: the number is a solid muted warm gray, the accent on the
            // active row, with the row's own right padding - not the washed
            // text/background blend the classic rows use.
            right -= DpiScale(tv->hwnd, kPanelRowPadX);
            COLORREF numCol = ThemeWindowDarkerTextColor();
            if (roundedRow) {
                COLORREF selBg, selFg;
                TocSelectionColors(&selBg, &selFg);
                numCol = selFg;
            }
            SetTextColor(hdc, numCol);
        } else if (!(isTreeSelected && hasFocus)) {
            // Slightly muted vs title when not selected (keeps numbers secondary).
            COLORREF muted =
                RGB((GetRValue(txtCol) * 2 + GetRValue(bgCol)) / 3, (GetGValue(txtCol) * 2 + GetGValue(bgCol)) / 3,
                    (GetBValue(txtCol) * 2 + GetBValue(bgCol)) / 3);
            SetTextColor(hdc, muted);
        }
        pageRect.x = std::max(pageRect.x, right - pageSize.dx);
        pageRect.dx = right - pageRect.x;
        if (roundedRow || TocUsesRedesignedRows(win)) {
            // tabular figures so the column of page numbers aligns
            HdcDrawTextTabular(hdc, pageLabel, pageRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
        } else {
            HdcDrawText(hdc, pageW, pageRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_RIGHT);
        }
    }

    if ((cd->uItemState & CDIS_FOCUS) && isTreeSelected && hasFocus) {
        DrawFocusRect(hdc, &drawRc);
    }
}

// https://docs.microsoft.com/en-us/windows/win32/controls/about-custom-draw
// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-nmtvcustomdraw
void OnTocCustomDraw(TreeView::CustomDrawEvent* ev) {
    ev->result = CDRF_DODEFAULT;
    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &(tvcd->nmcd);

    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    bool filterActive = HasTocFilter(win);
    bool showPageNumbers = gGlobalPrefs->showTocPageNumbers;
    bool multiHighlight = gShowAllMatchingTOC && win && len(win->tocMatchingItems) > 0;

    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        TocItem* tocItem = (TocItem*)ev->treeItem;
        if (!tocItem) {
            return;
        }
        TreeView* tv = ev->treeView;
        bool isTreeSelected = (cd->uItemState & CDIS_SELECTED) != 0;
        if (!isTreeSelected) {
            HTREEITEM hSel = TreeView_GetSelection(tv->hwnd);
            HTREEITEM hItem = tv->GetHandleByTreeItem(ev->treeItem);
            isTreeSelected = hSel && hItem && hSel == hItem;
        }
        bool isMultiMatch = TocItemIsMultiHighlight(win, tocItem);
        bool isSelected = isTreeSelected || isMultiMatch;
        bool hasFocus = isTreeSelected && (GetFocus() == tv->hwnd);

        LRESULT res = 0;
        if (isSelected) {
            // Theme-aware selection fill/text; strip CDIS_SELECTED so Explorer
            // theme does not paint a light inactive selection over dark text.
            COLORREF bgCol, txtCol;
            ResolveTreeFilterItemColors(cd->hdc, ToRect(cd->rc), tv->bgColor, tv->textColor, true, hasFocus, &bgCol,
                                        &txtCol);
            if (!(isTreeSelected && hasFocus) && tocItem->color != kColorUnset) {
                txtCol = tocItem->color;
            }
            if (TocUsesRedesignedRows(win)) {
                TocSelectionColors(&bgCol, &txtCol);
                // full-row select would fill a rectangle over the pill, so let
                // the default fill be the panel background; the pill and the
                // text are drawn in post-paint
                bgCol = tv->bgColor;
            }
            tvcd->clrText = txtCol;
            tvcd->clrTextBk = bgCol;
            cd->uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
            res |= CDRF_NEWFONT;
        } else if (tocItem->color != kColorUnset) {
            tvcd->clrText = tocItem->color;
        }
        if (tocItem->fontFlags != 0) {
            UpdateFont(cd->hdc, ev->treeView->hwnd, tocItem->fontFlags);
            res |= CDRF_NEWFONT;
        }
        // POSTPAINT: selection colors (issue #5848), page numbers, filter, multi-match.
        bool needPost = isSelected || filterActive || (showPageNumbers && tocItem->pageNo > 0) ||
                        (multiHighlight && isMultiMatch) || TocUsesRedesignedRows(win);
        if (needPost) {
            res |= CDRF_NOTIFYPOSTPAINT;
        }
        ev->result = res;
        return;
    }

    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        if (win) {
            DrawTocItemPostPaint(ev, win);
        }
        ev->result = CDRF_DODEFAULT;
        return;
    }
}

// disabled because of https://github.com/sumatrapdfreader/sumatrapdf/issues/2202
// it was added for https://github.com/sumatrapdfreader/sumatrapdf/issues/1716
// but unclear if its still needed
// this calls GoToTocLinkTask) which will eventually call GoToPage()
// which adds nav point. Maybe I should not add nav point
// if going to the same page?
// set when a mouse click changed the tree selection (handled by
// TocTreeSelectionChanged), so the NM_CLICK that follows doesn't navigate again
static bool gTocSelChangedByMouseClick = false;

static void TocTreeClick(TreeView::ClickEvent* ev) {
    // In 4a, a parent row is an expand/collapse control; only leaves navigate.
    // This preserves every other branch's expansion state (not an accordion).
    if (!ev->isDblClick && ev->treeItem) {
        MainWindow* w = FindMainWindowByHwnd(ev->treeView->hwnd);
        if (w && TocUsesRedesignedRows(w)) {
            TreeView* tv = ev->treeView;
            TocItem* it = (TocItem*)ev->treeItem;
            if (it->child) {
                HTREEITEM hi = tv->GetHandleByTreeItem(ev->treeItem);
                if (hi) {
                    TreeView_Expand(tv->hwnd, hi, TVE_TOGGLE);
                    ev->result = 1;
                    return;
                }
            }
        }
    }
    bool handledBySelChange = gTocSelChangedByMouseClick;
    gTocSelChangedByMouseClick = false;
    // A normal click changes the selection and is handled by
    // TocTreeSelectionChanged. Clicking the item that is already selected fires
    // no selection-change notification, so handle that here to let the user
    // re-click the current bookmark to jump back to its page (#2465).
    if (ev->isDblClick || !ev->treeItem || handledBySelChange) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (!win) {
        ReportIf(true);
        return;
    }
    GoToTocTreeItem(win, ev->treeItem, true);
}

static void TocTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (!win) {
        ReportIf(true);
        return;
    }

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (!shouldHandle) {
        return;
    }
    if (ev->byMouse) {
        // remember that this click already navigated, so the following
        // NM_CLICK (TocTreeClick) doesn't navigate a second time (#2465)
        gTocSelChangedByMouseClick = true;
    }
    bool allowExternal = ev->byMouse;
    GoToTocTreeItem(win, ev->selectedItem, allowExternal);
}

// Tab / Ctrl+Tab focus movement (also reused by Favorites tree)
void TocTreeKeyDown2(TreeView::KeyDownEvent* ev);

static void FocusTocFilterEdit(MainWindow* win) {
    if (!win || !win->tocFilterEdit || !win->tocFilterEdit->hwnd) {
        return;
    }
    HwndSetFocus(win->tocFilterEdit->hwnd);
    win->tocFilterEdit->SetCursorPositionAtEnd();
}

// Select the first top-level bookmark (Down from the search box).
static void SelectFirstTocTreeItem(MainWindow* win) {
    TreeView* tv = win ? win->tocTreeView : nullptr;
    if (!tv || !tv->treeModel || !tv->hwnd) {
        return;
    }
    TreeModel* tm = tv->treeModel;
    TreeItem root = tm->Root();
    if (tm->ChildCount(root) == 0) {
        return;
    }
    TreeItem first = tm->ChildAt(root, 0);
    tv->SelectItem(first);
    HTREEITEM h = tv->GetHandleByTreeItem(first);
    if (h) {
        TreeView_EnsureVisible(tv->hwnd, h);
    }
}

// TOC tree keyboard: Esc clears filter / focuses search; Up on first row returns
// to the search box. Tab (and Ctrl+Tab) stay in TocTreeKeyDown2 so Favorites can
// reuse that path.
static void TocTreeKeyDown(TreeView::KeyDownEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (ev->keyCode == VK_ESCAPE) {
        if (win && win->tocFilterEdit) {
            win->tocFilterEdit->SetText("");
            FocusTocFilterEdit(win);
            ev->result = 1;
            return;
        }
    }
    if (ev->keyCode == VK_UP && win && win->tocFilterEdit) {
        TreeItem sel = ev->treeView->GetSelection();
        HTREEITEM hSel = sel ? ev->treeView->GetHandleByTreeItem(sel) : nullptr;
        HTREEITEM hFirst = TreeView_GetRoot(ev->treeView->hwnd);
        if (hSel && hFirst && hSel == hFirst) {
            FocusTocFilterEdit(win);
            ev->result = 1;
            return;
        }
    }
    TocTreeKeyDown2(ev);
}

void TocTreeKeyDown2(TreeView::KeyDownEvent* ev) {
    // TODO: trying to fix https://github.com/sumatrapdfreader/sumatrapdf/issues/1841
    // doesn't work i.e. page up / page down seems to be processed anyway by TreeCtrl
#if 0
    if ((ev->keyCode == VK_PRIOR) || (ev->keyCode == VK_NEXT)) {
        // up/down in tree is not very useful, so instead
        // send it to frame so that it scrolls document instead
        MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
        // this is sent as WM_NOTIFY to TreeCtrl but for frame it's WM_KEYDOWN
        // alternatively, we could call FrameOnKeydown(ev->wp, ev->lp, false);
        SendMessageW(win->hwndFrame, WM_KEYDOWN, ev->wp, ev->lp);
        ev->didHandle = true;
        ev->result = 1;
        return;
    }
#endif
    if (ev->keyCode != VK_TAB) {
        ev->result = 0;
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (win->tabsVisible && IsCtrlPressed()) {
        TabsOnCtrlTab(win, IsShiftPressed());
        ev->result = 1;
        return;
    }
    AdvanceFocus(win);
    ev->result = 1;
}

// Position label, filter edit, and tree window within toc container using the
// wingui layout engine (VBox built in CreateToc).
static void LayoutTocContainer(MainWindow* win) {
    if (!win->tocLayout) {
        return;
    }
    Rect rc = HwndWindowRect(win->hwndTocBox);
    if (IsTouchChrome(win)) {
        int headerDy = DpiScale(win->hwndTocBox, kPanelHeaderDy);
        int filterDy = DpiScale(win->hwndTocBox, kPanelFilterDy);
        int filterInsetY = DpiScale(win->hwndTocBox, 8);
        int filterSide = DpiScale(win->hwndTocBox, 46);
        int bodyY = headerDy + filterDy + DpiScale(win->hwndTocBox, 14);
        bool showBreadcrumb = win->touchPanelMode == TouchPanelMode::Bookmarks && !HasTocFilter(win);
        int stickyDy = showBreadcrumb ? DpiScale(win->hwndTocBox, 36) : 0;
        int treeY = bodyY + stickyDy;
        int labelDx = HasTocFilter(win) ? rc.dx - DpiScale(win->hwndTocBox, 124) : rc.dx;
        win->tocLabelWithClose->SetBounds(Rect{0, 0, std::max(0, labelDx), headerDy});
        SetWindowPos(win->tocFilterEdit->hwnd, nullptr, filterSide, headerDy + filterInsetY, rc.dx - 2 * filterSide,
                     filterDy - 2 * filterInsetY, SWP_NOZORDER);
        SetWindowPos(win->tocTreeView->hwnd, nullptr, 0, treeY, rc.dx, std::max(0, rc.dy - treeY), SWP_NOZORDER);
        SetWindowPos(win->hwndTocSticky, HWND_TOP, 0, bodyY, rc.dx, stickyDy, SWP_NOACTIVATE);
        bool bookmarks = win->touchPanelMode == TouchPanelMode::Bookmarks;
        bool search = win->touchPanelMode == TouchPanelMode::Search;
        // Sets the child WS_VISIBLE bits even when the whole panel is currently
        // hidden: HwndSetVisible() compares that bit rather than
        // IsWindowVisible() (which also examines ancestors and would no-op
        // here, leaving the stale tree covering Search after opening it from
        // Home or Library).
        HwndSetVisible(win->tocFilterEdit->hwnd, bookmarks || search);
        HwndSetVisible(win->tocTreeView->hwnd, bookmarks);
        UpdateTocStickyHeader(win);
        return;
    }
    win->tocLayout->Layout(Tight(Size{rc.dx, rc.dy}));
    win->tocLayout->SetBounds(Rect{0, 0, rc.dx, rc.dy});
}

static Str TouchPanelTitle(TouchPanelMode mode) {
    switch (mode) {
        case TouchPanelMode::Thumbnails:
            return StrL("Thumbnails");
        case TouchPanelMode::Search:
            return StrL("Search");
        case TouchPanelMode::Annotations:
            return StrL("Annotations");
        case TouchPanelMode::Attachments:
            return StrL("Attachments");
        case TouchPanelMode::Favorites:
            return StrL("Favorites");
        default:
            return StrL("Bookmarks");
    }
}

static Str* TouchPanelSearchQuery(MainWindow* win, TouchPanelMode mode) {
    if (mode == TouchPanelMode::Bookmarks) {
        return &win->touchBookmarkSearchQuery;
    }
    if (mode == TouchPanelMode::Search) {
        return &win->touchDocumentSearchQuery;
    }
    return nullptr;
}

void SetTouchPanelModeAndRestoreSearch(MainWindow* win, TouchPanelMode mode) {
    if (!win) {
        return;
    }
    Str* oldQuery = TouchPanelSearchQuery(win, win->touchPanelMode);
    if (oldQuery && win->tocFilterEdit) {
        str::ReplaceWithCopy(oldQuery, win->tocFilterEdit->GetTextTemp());
    }
    win->touchPanelMode = mode;
    Str* newQuery = TouchPanelSearchQuery(win, mode);
    if (newQuery && win->tocFilterEdit) {
        win->tocFilterEdit->SetText(*newQuery);
    }
    UpdateTouchPanelMode(win);
}

// True when the sidebar is showing the touch chrome's Search panel. That panel
// renders win->findMatches inline, so it is a second live consumer of the find
// results besides the classic find bar / floating find window.
bool IsTouchSearchPanelVisible(MainWindow* win) {
    if (!win || !IsTouchChrome(win)) {
        return false;
    }
    return win->uiState.tocVisible && win->touchPanelMode == TouchPanelMode::Search;
}

// The Search panel's live query, or {} when the panel is not the one driving
// the find. The panel never opens the classic find bar, so win->hwndFindEdit
// (which the find code used to treat as the only source of the query) is empty
// while the panel owns the search - which is why Find Next / Prev did nothing.
// The edit is the live value; touchDocumentSearchQuery is its saved copy and
// covers the moment right after a mode switch, before the edit is refilled.
TempStr TouchSearchPanelQueryTemp(MainWindow* win) {
    if (!IsTouchSearchPanelVisible(win)) {
        return {};
    }
    if (win->tocFilterEdit && win->tocFilterEdit->hwnd) {
        TempStr s = win->tocFilterEdit->GetTextTemp();
        if (!str::IsEmptyOrWhiteSpace(s)) {
            return s;
        }
    }
    if (!str::IsEmptyOrWhiteSpace(win->touchDocumentSearchQuery)) {
        return str::DupTemp(win->touchDocumentSearchQuery);
    }
    return {};
}

void UpdateTouchPanelMode(MainWindow* win) {
    if (!win || !IsTouchChrome(win) || !win->tocLabelWithClose) {
        return;
    }
    bool bookmarks = win->touchPanelMode == TouchPanelMode::Bookmarks;
    bool hasFilter = bookmarks || win->touchPanelMode == TouchPanelMode::Search;
    win->tocLabelWithClose->SetLabel(TouchPanelTitle(win->touchPanelMode));
    LayoutTocContainer(win);
    if (win->tocFilterEdit) {
        Str cue = bookmarks ? StrL("Search bookmarks") : StrL("Search this document");
        WStr cueW = ToWStrTemp(cue);
        SendMessageW(win->tocFilterEdit->hwnd, EM_SETCUEBANNER, TRUE, (LPARAM)cueW.s);
        ShowWindow(win->tocFilterEdit->hwnd, hasFilter ? SW_SHOW : SW_HIDE);
    }
    if (win->tocTreeView) {
        ShowWindow(win->tocTreeView->hwnd, bookmarks ? SW_SHOW : SW_HIDE);
    }
    HwndInvalidate(win->hwndTocBox, true);
}

static Rect TouchThumbnailRect(MainWindow* win, int pageIndex) {
    HWND hwnd = win->hwndTocBox;
    Rect rc = HwndClientRect(hwnd);
    int pad = DpiScale(hwnd, 16);
    int gap = DpiScale(hwnd, 14);
    int yStart = DpiScale(hwnd, kPanelHeaderDy + 16);
    int cardDx = (rc.dx - (2 * pad) - gap) / 2;
    int pageDy = (cardDx * 25) / 18;
    int labelDy = DpiScale(hwnd, 24);
    int row = pageIndex / 2;
    int col = pageIndex % 2;
    return Rect{pad + col * (cardDx + gap), yStart + row * (pageDy + labelDy + gap) - win->touchPanelScrollY, cardDx,
                pageDy};
}

struct TouchThumbnailRenderData {
    HWND hwnd = nullptr;
    MainWindow* win = nullptr;
    int pageIdx = -1;
};

// Back on the UI thread: the render is no longer in flight, so a later cache
// miss for this page is free to ask for it again.
static void TouchThumbnailRenderDone(TouchThumbnailRenderData* data) {
    MainWindow* win = data->win;
    if (IsMainWindowValid(win)) {
        int at = win->touchThumbnailRequested.Find(data->pageIdx);
        if (at >= 0) {
            win->touchThumbnailRequested.RemoveAt(at);
        }
        if (win->hwndTocBox) {
            HwndInvalidate(win->hwndTocBox, false);
        }
    }
    delete data;
}

// Runs on the render thread (RenderCache guarantees exactly one call per
// request, including failures and queue evictions).
static void TouchThumbnailRenderFinished(TouchThumbnailRenderData* data, PageRenderRequest*) {
    // InvalidateRect is safe across threads. The HWND value can be stale if
    // the window closed, in which case Windows simply rejects the request.
    if (data->hwnd) {
        InvalidateRect(data->hwnd, nullptr, FALSE);
    }
    // touchThumbnailRequested is UI-thread state, so clearing the in-flight
    // mark has to hop threads. Without it the mark was permanent, and once the
    // shared render cache evicted a thumbnail (which it does as soon as the
    // document view renders anything else) the page was never re-requested -
    // that is why reopening the panel showed blank white cards.
    uitask::Post(MkFunc0<TouchThumbnailRenderData>(TouchThumbnailRenderDone, data), "TouchThumbnailRenderDone");
}

static void RequestTouchThumbnail(MainWindow* win, DisplayModel* dm, int pageNo, Rect card) {
    auto* engine = dm->GetEngine();
    RectF pageRect = engine->PageMediabox(pageNo);
    if (pageRect.IsEmpty()) {
        return;
    }
    pageRect = engine->Transform(pageRect, pageNo, 1.0f, 0);
    float zoom = std::min((float)card.dx / pageRect.dx, (float)card.dy / pageRect.dy);
    pageRect = engine->Transform(pageRect, pageNo, 1.0f, 0, true);
    auto* data = new TouchThumbnailRenderData();
    data->hwnd = win->hwndTocBox;
    data->win = win;
    data->pageIdx = pageNo - 1;
    auto cb = MkFunc1(TouchThumbnailRenderFinished, data);
    gRenderCache->Render(dm, pageNo, 0, zoom, pageRect, cb);
}

static void CollectAttachmentItems(TocItem* item, Vec<TocItem*>& items) {
    for (TocItem* it = item; it; it = it->next) {
        if (it->dest && it->dest->GetKind() == kindDestinationAttachment) {
            items.Append(it);
        }
        CollectAttachmentItems(it->child, items);
    }
}

// A flattened row list for the favorites panel: a header row per document
// followed by that document's saved pages. Paint, hit-testing and the scroll
// extent all build it the same way, so they cannot disagree about what sits
// at a given y (the class of bug that made search results untappable).
struct TouchFavRow {
    bool isHeader = false;
    FileState* fs = nullptr;
    Favorite* fav = nullptr; // null on a header row
};

static void CollectTouchFavRows(Vec<TouchFavRow>& rows) {
    rows.Reset();
    Vec<FileState*> files;
    GetFilesWithFavorites(files);
    for (FileState* fs : files) {
        TouchFavRow hdr;
        hdr.isHeader = true;
        hdr.fs = fs;
        rows.Append(hdr);
        for (Favorite* f : *fs->favorites) {
            if (!f || f->isTemporary) {
                continue;
            }
            TouchFavRow r;
            r.fs = fs;
            r.fav = f;
            rows.Append(r);
        }
    }
}

// height of the strip holding the "add current page" button, when there is a
// document to add a page from
static int TouchFavAddStripDy(MainWindow* win) {
    // the strip always carries the toolbar toggle; the add button only appears
    // when there is a page to add
    return DpiScale(win->hwndTocBox, 40);
}

static int TouchFavRowsTop(MainWindow* win) {
    return DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) + TouchFavAddStripDy(win) - win->touchPanelScrollY;
}

// the delete target on a favorite row, and the "add current page" button in the
// panel header. Both are computed here so the paint and the hit test cannot
// drift apart.
// --- favorites drag-to-reorder -------------------------------------------
// Only within one document: the panel groups by file, and moving a favorite
// between documents would mean re-homing it, which is a different action.
static MainWindow* gFavDragWin = nullptr;
static int gFavDragFromRow = -1;   // index into the flattened row list
static int gFavDragOverRow = -1;   // where it would land
static bool gFavDragActive = false;

static void ResetFavDrag() {
    gFavDragWin = nullptr;
    gFavDragFromRow = -1;
    gFavDragOverRow = -1;
    gFavDragActive = false;
}

// which flattened row a y lands on, or -1
static int TouchFavRowAt(MainWindow* win, int y, int nRows) {
    int rowDy = DpiScale(win->hwndTocBox, TouchSidebarListRowDy());
    int y0 = TouchFavRowsTop(win);
    if (rowDy <= 0 || y < y0) {
        return -1;
    }
    int idx = (y - y0) / rowDy;
    return (idx >= 0 && idx < nRows) ? idx : -1;
}

static Rect TouchFavDeleteRect(MainWindow* win, const Rect& row) {
    HWND hw = win->hwndTocBox;
    int d = DpiScale(hw, 26);
    return Rect{row.x + row.dx - d, row.y + (row.dy - d) / 2, d, d};
}

// Below the header band, not inside it: the panel title is a child window that
// paints itself over that band, so anything drawn there disappears under it.
// "Show in toolbar" toggle: a display preference, so it is offered whether or
// not a document is open.
static Rect TouchFavToolbarToggleRect(MainWindow* win) {
    HWND hw = win->hwndTocBox;
    int dy = DpiScale(hw, 26);
    int y = DpiScale(hw, kPanelHeaderDy + 7);
    return Rect{DpiScale(hw, 14), y, DpiScale(hw, 150), dy};
}

static Rect TouchFavAddRect(MainWindow* win) {
    HWND hw = win->hwndTocBox;
    Rect client = HwndClientRect(hw);
    int d = DpiScale(hw, 28);
    int pad = DpiScale(hw, 14);
    int y = DpiScale(hw, kPanelHeaderDy + 6);
    return Rect{client.dx - d - pad, y, d, d};
}

static int TouchPanelMaxScroll(MainWindow* win) {
    Rect client = HwndClientRect(win->hwndTocBox);
    int contentBottom = client.dy;
    if (win->touchPanelMode == TouchPanelMode::Thumbnails && win->ctrl) {
        int count = win->ctrl->PageCount();
        if (count > 0) {
            Rect last = TouchThumbnailRect(win, count - 1);
            contentBottom = last.y + win->touchPanelScrollY + last.dy + DpiScale(win->hwndTocBox, 40);
        }
    } else if (win->touchPanelMode == TouchPanelMode::Search) {
        contentBottom = DpiScale(win->hwndTocBox, kPanelHeaderDy + kPanelFilterDy + 64) +
                        len(win->findMatches) * DpiScale(win->hwndTocBox, TouchSearchResultRowDy()) +
                        DpiScale(win->hwndTocBox, 12);
    } else if (win->touchPanelMode == TouchPanelMode::Annotations && win->AsFixed()) {
        Vec<Annotation*> annotations;
        EngineMupdfGetAnnotations(win->AsFixed()->GetEngine(), annotations);
        contentBottom = DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) +
                        len(annotations) * DpiScale(win->hwndTocBox, TouchSidebarListRowDy()) +
                        DpiScale(win->hwndTocBox, 12);
    } else if (win->touchPanelMode == TouchPanelMode::Favorites) {
        Vec<TouchFavRow> rows;
        CollectTouchFavRows(rows);
        contentBottom = DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) +
                        len(rows) * DpiScale(win->hwndTocBox, TouchSidebarListRowDy()) +
                        DpiScale(win->hwndTocBox, 12);
    } else if (win->touchPanelMode == TouchPanelMode::Attachments && win->ctrl) {
        Vec<TocItem*> attachments;
        TocTree* toc = win->ctrl->GetToc();
        if (toc && toc->root) {
            CollectAttachmentItems(toc->root->child, attachments);
        }
        contentBottom = DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) +
                        len(attachments) * DpiScale(win->hwndTocBox, TouchSidebarListRowDy()) +
                        DpiScale(win->hwndTocBox, 12);
    }
    return std::max(0, contentBottom - client.dy);
}

static void PaintTouchPanelPress(MainWindow* win, HDC hdc);

static void PaintTouchPanelMode(MainWindow* win, HDC hdc) {
    TouchPanelMode mode = win->touchPanelMode;
    if (mode == TouchPanelMode::Bookmarks) {
        return;
    }
    // drawn at the end of every return path below
    struct PressOverlay {
        MainWindow* w;
        HDC dc;
        ~PressOverlay() {
            PaintTouchPanelPress(w, dc);
        }
    } pressOverlay{win, hdc};
    Rect rc = HwndClientRect(win->hwndTocBox);
    HdcFillRect(hdc, Rect{0, DpiScale(win->hwndTocBox, kPanelHeaderDy), rc.dx, rc.dy}, ThemeHotBackgroundColor());
    SetBkMode(hdc, TRANSPARENT);
    if (mode == TouchPanelMode::Thumbnails) {
        int pageCount = win->ctrl ? win->ctrl->PageCount() : 0;
        int current = win->ctrl ? win->ctrl->CurrentPageNo() : 0;
        auto* dm = win->AsFixed();
        if (win->touchThumbnailDm != dm) {
            win->touchThumbnailDm = dm;
            win->touchThumbnailRequested.Reset();
        }
        int count = pageCount;
        int first = 0;
        int last = count;
        if (count > 0) {
            Rect firstCard = TouchThumbnailRect(win, 0);
            Rect thirdCard = count > 2 ? TouchThumbnailRect(win, 2) : firstCard;
            int rowStride = std::max(1, thirdCard.y - firstCard.y);
            int contentStartY = firstCard.y + win->touchPanelScrollY;
            first = std::max(0, ((win->touchPanelScrollY - contentStartY) / rowStride) * 2 - 2);
            int visibleRows = (rc.dy / rowStride) + 3;
            last = std::min(count, first + visibleRows * 2);
        }
        for (int i = first; i < last; i++) {
            Rect card = TouchThumbnailRect(win, i);
            bool isCurrent = i + 1 == current;
            COLORREF border = isCurrent ? ThemeWindowLinkColor() : RGB(255, 255, 255);
            FillTocPill(hdc, card, DpiScale(win->hwndTocBox, 6), RGB(255, 255, 255), border,
                        isCurrent ? DpiScale(win->hwndTocBox, 2) : 1);
            if (dm) {
                auto* engine = dm->GetEngine();
                RectF pageRect = engine->PageMediabox(i + 1);
                pageRect = engine->Transform(pageRect, i + 1, 1.0f, 0);
                float zoom =
                    pageRect.IsEmpty() ? 0.0f : std::min((float)card.dx / pageRect.dx, (float)card.dy / pageRect.dy);
                BitmapCacheEntry* entry = zoom > 0 ? gRenderCache->Find(dm, i + 1, 0, zoom) : nullptr;
                if (entry) {
                    int inset = DpiScale(win->hwndTocBox, 2);
                    Rect imageRc = card;
                    imageRc.Inflate(-inset, -inset);
                    BlitPixmap(entry->bitmap, hdc, imageRc);
                    gRenderCache->DropCacheEntry(entry);
                } else if (win->touchThumbnailRequested.Find(i) < 0) {
                    win->touchThumbnailRequested.Append(i);
                    RequestTouchThumbnail(win, dm, i + 1, card);
                }
            }
            Rect label{card.x, card.y + card.dy, card.dx, DpiScale(win->hwndTocBox, 24)};
            SetTextColor(hdc, (i + 1 == current) ? ThemeWindowLinkColor() : ThemeWindowTextColor());
            HdcDrawTextTabular(hdc, fmt("%d", i + 1), label, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                               HdcGetUiFont(hdc, kFontSizeMeta, i + 1 == current ? kFontWeightStrong : FW_DONTCARE));
        }
        return;
    }

    if (mode == TouchPanelMode::Search) {
        win->touchPanelScrollY = std::clamp(win->touchPanelScrollY, 0, TouchPanelMaxScroll(win));
        int controlsY = DpiScale(win->hwndTocBox, kPanelHeaderDy + kPanelFilterDy + 4);
        Rect countRc{DpiScale(win->hwndTocBox, 16), controlsY, rc.dx - DpiScale(win->hwndTocBox, 120),
                     DpiScale(win->hwndTocBox, 44)};
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, fmt("%d matches", len(win->findMatches)), countRc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kFontSizeMeta));
        int navDy = DpiScale(win->hwndTocBox, 36);
        Rect prevRc{rc.dx - DpiScale(win->hwndTocBox, 92), controlsY + DpiScale(win->hwndTocBox, 4), navDy, navDy};
        Rect nextRc{rc.dx - DpiScale(win->hwndTocBox, 48), prevRc.y, navDy, navDy};
        FillTocPill(hdc, prevRc, navDy / 2, ThemeControlBackgroundColor(), ThemeControlBackgroundColor());
        FillTocPill(hdc, nextRc, navDy / 2, ThemeControlBackgroundColor(), ThemeControlBackgroundColor());
        SetTextColor(hdc, ThemeWindowTextColor());
        HdcDrawText(hdc, StrL("<"), prevRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kFontSizeLabel));
        HdcDrawText(hdc, StrL(">"), nextRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, kFontSizeLabel));

        int rowDy = DpiScale(win->hwndTocBox, TouchSearchResultRowDy());
        int first = std::max(0, win->touchPanelScrollY / rowDy - 1);
        int visible = rc.dy / rowDy + 3;
        int last = std::min(len(win->findMatches), first + visible);
        StrVec findWords;
        SplitFilterToWords(win->findCountText ? win->findCountText : win->browserFindTerm, findWords);
        Vec<u8> highlighted;
        for (int i = first; i < last; i++) {
            const FindMatch& match = win->findMatches[i];
            Rect row = TouchSearchResultRect(win, i);
            FillTocPill(hdc, row, DpiScale(win->hwndTocBox, 8), ThemeWindowControlBackgroundColor(),
                        ThemeWindowControlBackgroundColor());
            Rect pageRc{row.x + DpiScale(win->hwndTocBox, 12), row.y + DpiScale(win->hwndTocBox, 6),
                        row.dx - DpiScale(win->hwndTocBox, 24), DpiScale(win->hwndTocBox, 18)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("Page %d", match.startPage), pageRc, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX,
                        HdcGetUiFont(hdc, kFontSizeMeta));
            Rect snippetRc{pageRc.x, pageRc.y + pageRc.dy + DpiScale(win->hwndTocBox, 3), pageRc.dx,
                           row.y + row.dy - pageRc.y - pageRc.dy - DpiScale(win->hwndTocBox, 7)};
            SetTextColor(hdc, ThemeWindowTextColor());
            HFONT snippetFont = HdcGetUiFont(hdc, kFontSizeLabel);
            ScopedSelectObject selectFont(hdc, snippetFont);
            DrawMaybeHighlightedText(hdc, snippetRc, match.snippet, findWords, highlighted, ThemeHotBackgroundColor(),
                                     false, win->findMatchWholeWord,
                                     DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_TOP);
        }
        if (last > first) {
            return;
        }
    }
    if (mode == TouchPanelMode::Favorites) {
        Vec<TouchFavRow> rows;
        CollectTouchFavRows(rows);
        HWND hw = win->hwndTocBox;
        int rowDy = DpiScale(hw, TouchSidebarListRowDy());
        int y0 = TouchFavRowsTop(win);
        if (len(rows) == 0) {
            TempStr empty = str::DupTemp(StrL("No saved pages yet. Tap the bookmark button to save one."));
            Rect r{DpiScale(hw, 16), y0 + DpiScale(hw, 8), rc.dx - DpiScale(hw, 32), rowDy * 2};
            HFONT f = HdcGetUiFont(hdc, kPanelRowFontSize);
            ScopedSelectObject sel(hdc, f);
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, empty, r, DT_LEFT | DT_WORDBREAK);
            return;
        }
        WindowTab* curTab = win->CurrentTab();
        Str curPath = (curTab && !curTab->IsAboutTab()) ? curTab->filePath : Str{};
        // add the page being read; nothing to add without a document
        if (win->IsDocLoaded()) {
            Rect add = TouchFavAddRect(win);
            Gdiplus::Graphics gfx(hdc);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush bg(GdiRgbFromCOLORREF(ThemeTouchSurfaceColor()));
            gfx.FillEllipse(&bg, add.x, add.y, add.dx, add.dy);
            Gdiplus::Pen pen(GdiRgbFromCOLORREF(ThemeWindowLinkColor()),
                             (Gdiplus::REAL)std::max(1, DpiScale(hw, 2)));
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            int acx = add.x + add.dx / 2;
            int acy = add.y + add.dy / 2;
            int aarm = DpiScale(hw, 7);
            gfx.DrawLine(&pen, acx - aarm, acy, acx + aarm, acy);
            gfx.DrawLine(&pen, acx, acy - aarm, acx, acy + aarm);
        }
        {
            // toolbar toggle: a check box and a label
            Rect tg = TouchFavToolbarToggleRect(win);
            bool on = gGlobalPrefs->favoritesInToolbar;
            Gdiplus::Graphics tgx(hdc);
            tgx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            int box = DpiScale(hw, 15);
            Rect br{tg.x, tg.y + (tg.dy - box) / 2, box, box};
            Gdiplus::Pen bp(GdiRgbFromCOLORREF(ThemeWindowDarkerTextColor()),
                            (Gdiplus::REAL)std::max(1, DpiScale(hw, 1)));
            if (on) {
                Gdiplus::SolidBrush fill(GdiRgbFromCOLORREF(ThemeWindowLinkColor()));
                tgx.FillRectangle(&fill, br.x, br.y, br.dx, br.dy);
                Gdiplus::Pen tick(GdiRgbFromCOLORREF(RGB(255, 255, 255)),
                                  (Gdiplus::REAL)std::max(1, DpiScale(hw, 2)));
                tick.SetStartCap(Gdiplus::LineCapRound);
                tick.SetEndCap(Gdiplus::LineCapRound);
                tgx.DrawLine(&tick, br.x + box / 4, br.y + box / 2, br.x + box / 2, br.y + (box * 3) / 4);
                tgx.DrawLine(&tick, br.x + box / 2, br.y + (box * 3) / 4, br.x + (box * 3) / 4, br.y + box / 4);
            } else {
                tgx.DrawRectangle(&bp, br.x, br.y, br.dx, br.dy);
            }
            HFONT ft = HdcGetUiFont(hdc, kPanelSubFontSize);
            ScopedSelectObject selt(hdc, ft);
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            Rect tl{br.x + box + DpiScale(hw, 8), tg.y, tg.dx - box - DpiScale(hw, 8), tg.dy};
            HdcDrawText(hdc, StrL("Show in toolbar"), tl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        for (int i = 0; i < len(rows); i++) {
            const TouchFavRow& row = rows[i];
            Rect r{DpiScale(hw, 12), y0 + i * rowDy, rc.dx - DpiScale(hw, 24), rowDy};
            if (r.y + r.dy < 0 || r.y > rc.dy) {
                continue; // scrolled out of view
            }
            if (row.isHeader) {
                // the document this group belongs to; the open one is marked
                TempStr name = path::GetBaseNameTemp(row.fs->filePath);
                bool isCurrent = curPath && str::Eq(row.fs->filePath, curPath);
                HFONT f = HdcGetUiFont(hdc, kPanelSubFontSize, kFontWeightStrong);
                ScopedSelectObject sel(hdc, f);
                SetTextColor(hdc, isCurrent ? ThemeWindowLinkColor() : ThemeWindowDarkerTextColor());
                Rect tr = r;
                tr.x += DpiScale(hw, 4);
                HdcDrawText(hdc, name, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                continue;
            }
            // one saved page: its name, and the page number on the right
            TempStr label = FavReadableNameTemp(row.fav);
            HFONT f = HdcGetUiFont(hdc, kPanelRowFontSize);
            ScopedSelectObject sel(hdc, f);
            SetTextColor(hdc, ThemeWindowTextColor());
            Rect tr = r;
            tr.x += DpiScale(hw, 20);
            tr.dx -= DpiScale(hw, 20 + 56);
            HdcDrawText(hdc, label, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            TempStr pageStr = fmt("%d", row.fav->pageNo);
            HFONT fp = HdcGetUiFont(hdc, kPanelPageFontSize);
            ScopedSelectObject selp(hdc, fp);
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            Rect pr{r.x + r.dx - DpiScale(hw, 82), r.y, DpiScale(hw, 44), r.dy};
            HdcDrawText(hdc, pageStr, pr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // where a dragged favorite would land
            if (gFavDragActive && gFavDragWin == win && gFavDragOverRow == i && gFavDragFromRow != i) {
                Gdiplus::Graphics dg(hdc);
                Gdiplus::SolidBrush accent(GdiRgbFromCOLORREF(ThemeWindowLinkColor()));
                bool below = gFavDragFromRow < i;
                int lineY = below ? (r.y + r.dy - DpiScale(hw, 1)) : r.y;
                dg.FillRectangle(&accent, r.x, lineY, r.dx, std::max(2, DpiScale(hw, 2)));
            }
            // the row being dragged reads as lifted
            if (gFavDragActive && gFavDragWin == win && gFavDragFromRow == i) {
                Gdiplus::Graphics dg(hdc);
                COLORREF c = ThemeWindowTextColor();
                Gdiplus::SolidBrush lift(Gdiplus::Color(28, GetRValue(c), GetGValue(c), GetBValue(c)));
                dg.FillRectangle(&lift, r.x, r.y, r.dx, r.dy);
            }

            // delete: a small x at the end of the row
            Rect del = TouchFavDeleteRect(win, r);
            Gdiplus::Graphics gfx(hdc);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen delPen(GdiRgbFromCOLORREF(ThemeWindowDarkerTextColor()),
                                (Gdiplus::REAL)std::max(1, DpiScale(hw, 1)));
            delPen.SetStartCap(Gdiplus::LineCapRound);
            delPen.SetEndCap(Gdiplus::LineCapRound);
            int cx = del.x + del.dx / 2;
            int cy = del.y + del.dy / 2;
            int arm = DpiScale(hw, 5);
            gfx.DrawLine(&delPen, cx - arm, cy - arm, cx + arm, cy + arm);
            gfx.DrawLine(&delPen, cx + arm, cy - arm, cx - arm, cy + arm);
        }
        return;
    }
    if (mode == TouchPanelMode::Annotations && win->AsFixed()) {
        Vec<Annotation*> annotations;
        EngineMupdfGetAnnotations(win->AsFixed()->GetEngine(), annotations);
        int y = DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) - win->touchPanelScrollY;
        int rowDy = DpiScale(win->hwndTocBox, TouchSidebarListRowDy());
        int count = std::min(len(annotations), 10);
        for (int i = 0; i < count; i++) {
            Annotation* annot = annotations[i];
            Rect row{DpiScale(win->hwndTocBox, 12), y + i * rowDy, rc.dx - DpiScale(win->hwndTocBox, 24), rowDy};
            Rect swatch{row.x + DpiScale(win->hwndTocBox, 2), row.y + DpiScale(win->hwndTocBox, 12),
                        DpiScale(win->hwndTocBox, 12), DpiScale(win->hwndTocBox, 12)};
            COLORREF swatchCol = RGB(245, 198, 107);
            PdfColor pdfCol = GetColor(annot);
            if (pdfCol != kColorUnset) {
                u8 r, g, b, a;
                UnpackPdfColor(pdfCol, r, g, b, a);
                swatchCol = RGB(r, g, b);
            }
            FillTocPill(hdc, swatch, DpiScale(win->hwndTocBox, 3), swatchCol, swatchCol);
            Str contents = Contents(annot);
            Str label = contents ? contents : AnnotationReadableNameTemp(Type(annot));
            Rect textRc{row.x + DpiScale(win->hwndTocBox, 28), row.y + DpiScale(win->hwndTocBox, 3),
                        row.dx - DpiScale(win->hwndTocBox, 28), DpiScale(win->hwndTocBox, 21)};
            SetTextColor(hdc, ThemeWindowTextColor());
            HdcDrawText(hdc, label, textRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_TOP,
                        HdcGetUiFont(hdc, kFontSizeLabel));
            Rect metaRc{textRc.x, textRc.y + textRc.dy, textRc.dx, DpiScale(win->hwndTocBox, 18)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("Page %d · %s", PageNo(annot), AnnotationReadableNameTemp(Type(annot))), metaRc,
                        DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_TOP, HdcGetUiFont(hdc, kFontSizeMeta));
        }
        if (count > 0) {
            return;
        }
    }
    if (mode == TouchPanelMode::Attachments && win->ctrl) {
        Vec<TocItem*> attachments;
        TocTree* toc = win->ctrl->GetToc();
        if (toc && toc->root) {
            CollectAttachmentItems(toc->root->child, attachments);
        }
        int y = DpiScale(win->hwndTocBox, kPanelHeaderDy + 12) - win->touchPanelScrollY;
        int rowDy = DpiScale(win->hwndTocBox, TouchSidebarListRowDy());
        for (int i = 0; i < len(attachments); i++) {
            TocItem* item = attachments[i];
            Rect row{DpiScale(win->hwndTocBox, 12), y + i * rowDy, rc.dx - DpiScale(win->hwndTocBox, 24), rowDy};
            Rect icon{row.x, row.y + DpiScale(win->hwndTocBox, 10), DpiScale(win->hwndTocBox, 36),
                      DpiScale(win->hwndTocBox, 36)};
            FillTocPill(hdc, icon, DpiScale(win->hwndTocBox, 8), RGB(234, 229, 222), RGB(234, 229, 222));
            int paperclipDy = DpiScale(win->hwndTocBox, 18);
            HIMAGELIST attachmentIcons =
                GetTintedToolbarImageList(paperclipDy, ThemeWindowDarkerTextColor(), RGB(234, 229, 222));
            if (attachmentIcons) {
                ImageList_Draw(attachmentIcons, (int)TbIcon::Attachment, hdc, icon.x + (icon.dx - paperclipDy) / 2,
                               icon.y + (icon.dy - paperclipDy) / 2, ILD_NORMAL);
            }
            Rect textRc{row.x + DpiScale(win->hwndTocBox, 48), row.y + DpiScale(win->hwndTocBox, 5),
                        row.dx - DpiScale(win->hwndTocBox, 48), DpiScale(win->hwndTocBox, 21)};
            SetTextColor(hdc, ThemeWindowTextColor());
            HdcDrawText(hdc, item->title, textRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_TOP,
                        HdcGetUiFont(hdc, kFontSizeLabel, FW_MEDIUM));
            Rect metaRc{textRc.x, textRc.y + textRc.dy, textRc.dx, DpiScale(win->hwndTocBox, 18)};
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, StrL("Attached file"), metaRc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_TOP,
                        HdcGetUiFont(hdc, kFontSizeMeta));
        }
        if (len(attachments) > 0) {
            return;
        }
    }

    Str message;
    switch (mode) {
        case TouchPanelMode::Search:
            message = StrL("Search this document");
            break;
        case TouchPanelMode::Annotations:
            message = StrL("No annotations on this page");
            break;
        case TouchPanelMode::Attachments:
            message = StrL("No attachments in this document");
            break;
        default:
            break;
    }
    Rect textRc{DpiScale(win->hwndTocBox, 16), DpiScale(win->hwndTocBox, kPanelHeaderDy + 24),
                rc.dx - DpiScale(win->hwndTocBox, 32), DpiScale(win->hwndTocBox, 44)};
    SetTextColor(hdc, ThemeWindowDarkerTextColor());
    HdcDrawText(hdc, message, textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                HdcGetUiFont(hdc, kFontSizeLabel));
}

static Rect TouchFilterClearRect(MainWindow* win, bool hitTarget) {
    Edit* edit = win->tocFilterEdit;
    if (!edit || !edit->hwnd || !HwndIsVisible(edit->hwnd)) {
        return {};
    }
    HWND hwnd = win->hwndTocBox;
    Rect er = HwndWindowRect(edit->hwnd);
    POINT tl{er.x, er.y};
    ScreenToClient(hwnd, &tl);
    int sideMargin = DpiScale(hwnd, 16);
    Rect pill{sideMargin, tl.y - DpiScale(hwnd, 8), HwndClientRect(hwnd).dx - (2 * sideMargin),
              er.dy + DpiScale(hwnd, 16)};
    int clearDy = DpiScale(hwnd, 22);
    Rect clear{pill.x + pill.dx - DpiScale(hwnd, 10) - clearDy, pill.y + (pill.dy - clearDy) / 2, clearDy, clearDy};
    if (hitTarget) {
        clear.Inflate(DpiScale(hwnd, 9), DpiScale(hwnd, 9));
    }
    return clear;
}

// Draws the rounded filter/search field around win->tocFilterEdit on the panel
// itself (the edit is a plain child; the pill, magnifier and clear button are
// ours). It runs after EndPaint(), on a GetDC() of the panel, so whatever it
// touches wins over what WM_PAINT just drew.
//
// `ownsBodyBelow` says who is responsible for the area under the field. In
// Bookmarks mode nobody paints it in WM_PAINT - the tree and sticky-header
// child windows cover it - and a plain erase down to the bottom of the client
// is the cheapest way to avoid a WC_STATIC-background sliver between the pill
// and the tree, so pass true. In the self-painted modes (Search) the body under
// the field is the panel's own content, drawn by PaintTouchPanelMode() during
// WM_PAINT; erasing down to the bottom there wipes the match count, the
// prev/next buttons and the whole results list right after they were drawn.
// Pass false and the erase stops at the bottom of the pill.
static void PaintTouchFilterChrome(MainWindow* win, bool ownsBodyBelow) {
    Edit* edit = win->tocFilterEdit;
    if (!edit || !edit->hwnd || !HwndIsVisible(edit->hwnd)) {
        return;
    }
    HWND hwnd = win->hwndTocBox;
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return;
    }
    Rect rcClient = HwndClientRect(hwnd);
    Rect er = HwndWindowRect(edit->hwnd);
    POINT tl{er.x, er.y};
    ScreenToClient(hwnd, &tl);
    int sideMargin = DpiScale(hwnd, 16);
    Rect pill{sideMargin, tl.y, rcClient.dx - (2 * sideMargin), er.dy};
    pill.Inflate(0, DpiScale(hwnd, 8));
    // The panel is a WC_STATIC and paints its own background, which is the
    // control background (pure black on the Dark theme). Everywhere else that
    // is covered by the header / tree children, but the filter strip is not -
    // so repaint that band in the panel color before drawing the pill on it.
    int bandY = pill.y - DpiScale(hwnd, 8);
    int bandBottom = ownsBodyBelow ? rcClient.dy : pill.y + pill.dy;
    Rect band{0, bandY, rcClient.dx, std::max(0, bandBottom - bandY)};
    HdcFillRect(hdc, band, ThemeHotBackgroundColor());
    COLORREF fieldBg = ThemeTouchSurfaceColor();
    FillTocPill(hdc, pill, pill.dy / 2, fieldBg, ThemeEdgeColor());

    int iconDy = DpiScale(hwnd, kPanelFilterIconDy);
    HIMAGELIST iml = GetTintedToolbarImageList(iconDy, ThemeWindowDarkerTextColor(), fieldBg);
    if (iml) {
        int ix = pill.x + DpiScale(hwnd, 16);
        int iy = pill.y + ((pill.dy - iconDy) / 2);
        ImageList_Draw(iml, (int)TbIcon::Search, hdc, ix, iy, ILD_NORMAL);
    }

    if (GetWindowTextLengthW(edit->hwnd) > 0) {
        Rect clear = TouchFilterClearRect(win, false);
        FillTocPill(hdc, clear, clear.dy / 2, RGB(234, 229, 222), RGB(234, 229, 222));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ThemeWindowDarkerTextColor());
        HdcDrawText(hdc, StrL("×"), clear, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                    HdcGetUiFont(hdc, 11, FW_MEDIUM));
    }
    ReleaseDC(hwnd, hdc);
    HwndInvalidate(edit->hwnd, false);
}

constexpr UINT_PTR kTouchPanelPressTimerId = 22;


// --- panel press feedback ------------------------------------------------
// The panel's rows are pure tap targets and answered a tap with nothing until
// the view changed, which on a touchscreen is the one place feedback matters
// most. One overlay, same idea as the Library's.
static MainWindow* gPanelPressWin = nullptr;
static Rect gPanelPressRect;
static AnimVal gPanelPressVal;

// Which row/card sits under a point, for the modes whose rows are a simple
// list. Bookmarks is a real TreeView with its own selection drawing, so it is
// deliberately left alone.
static bool TouchPanelHitRect(MainWindow* win, Point pt, Rect* out) {
    if (!win || !win->hwndTocBox) {
        return false;
    }
    HWND hwnd = win->hwndTocBox;
    Rect client = HwndClientRect(hwnd);
    TouchPanelMode mode = win->touchPanelMode;

    if (mode == TouchPanelMode::Thumbnails && win->ctrl) {
        int count = win->ctrl->PageCount();
        for (int i = 0; i < count; i++) {
            Rect r = TouchThumbnailRect(win, i);
            if (r.Contains(pt)) {
                *out = r;
                return true;
            }
        }
        return false;
    }
    if (mode == TouchPanelMode::Search) {
        for (int i = 0; i < len(win->findMatches); i++) {
            Rect r = TouchSearchResultRect(win, i);
            if (r.Contains(pt)) {
                *out = r;
                return true;
            }
        }
        return false;
    }
    // Favorites / Annotations / Attachments all lay rows out the same way
    if (mode == TouchPanelMode::Favorites || mode == TouchPanelMode::Annotations ||
        mode == TouchPanelMode::Attachments) {
        int rowDy = DpiScale(hwnd, TouchSidebarListRowDy());
        int y0 = DpiScale(hwnd, kPanelHeaderDy + 12) - win->touchPanelScrollY;
        if (rowDy <= 0 || pt.y < y0) {
            return false;
        }
        int idx = (pt.y - y0) / rowDy;
        if (idx < 0) {
            return false;
        }
        if (mode == TouchPanelMode::Favorites) {
            // the per-document group headings are not tap targets, so they must
            // not light up either - feedback has to mean something will happen
            Vec<TouchFavRow> rows;
            CollectTouchFavRows(rows);
            if (idx >= len(rows) || rows[idx].isHeader) {
                return false;
            }
        }
        *out = Rect{DpiScale(hwnd, 12), y0 + idx * rowDy, client.dx - DpiScale(hwnd, 24), rowDy};
        return true;
    }
    return false;
}

static void UpdatePanelPressTimer(MainWindow* win) {
    if (!win || !win->hwndTocBox) {
        return;
    }
    if (gPanelPressVal.IsAnimating()) {
        SetTimer(win->hwndTocBox, kTouchPanelPressTimerId, kAnimTickMs, nullptr);
    } else {
        KillTimer(win->hwndTocBox, kTouchPanelPressTimerId);
    }
}

static void SetTouchPanelPressed(MainWindow* win, Point pt, bool down) {
    if (!win) {
        return;
    }
    Rect r;
    bool hit = down && TouchPanelHitRect(win, pt, &r);
    if (hit) {
        gPanelPressWin = win;
        gPanelPressRect = r;
    }
    float target = hit ? 1.0f : 0.0f;
    if (AnimEnabled()) {
        gPanelPressVal.SetTarget(target, hit ? kAnimPressMs : kAnimPressReleaseMs);
        UpdatePanelPressTimer(win);
    } else {
        gPanelPressVal.Set(target);
    }
    HwndInvalidate(win->hwndTocBox, false);
}

static void PaintTouchPanelPress(MainWindow* win, HDC hdc) {
    if (gPanelPressWin != win) {
        return;
    }
    float amt = gPanelPressVal.Value();
    if (amt <= 0.01f || gPanelPressRect.IsEmpty()) {
        return;
    }
    Rect r = gPanelPressRect;
    int inset = (int)((float)DpiScale(win->hwndTocBox, 2) * amt + 0.5f);
    r.x += inset;
    r.y += inset;
    r.dx -= inset * 2;
    r.dy -= inset * 2;
    if (r.dx <= 0 || r.dy <= 0) {
        return;
    }
    Gdiplus::Graphics gfx(hdc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    COLORREF col = ThemeWindowTextColor();
    Gdiplus::Color c((u8)(38.0f * amt), GetRValue(col), GetGValue(col), GetBValue(col));
    Gdiplus::SolidBrush br(c);
    int d = std::min(DpiScale(win->hwndTocBox, 10) * 2, std::min(r.dx, r.dy));
    Gdiplus::GraphicsPath path;
    path.AddArc(r.x, r.y, d, d, 180.0f, 90.0f);
    path.AddArc(r.x + r.dx - d, r.y, d, d, 270.0f, 90.0f);
    path.AddArc(r.x + r.dx - d, r.y + r.dy - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.x, r.y + r.dy - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    gfx.FillPath(&br, &path);
}

static bool ActivateTouchPanelAt(MainWindow* win, Point pt) {
    HWND hwnd = win->hwndTocBox;
    if (win->tocFilterEdit && HwndIsVisible(win->tocFilterEdit->hwnd) &&
        GetWindowTextLengthW(win->tocFilterEdit->hwnd) > 0 && TouchFilterClearRect(win, true).Contains(pt)) {
        win->tocFilterEdit->SetText({});
        HwndSetFocus(win->tocFilterEdit->hwnd);
        return true;
    }
    if (win->touchPanelMode == TouchPanelMode::Favorites) {
        Vec<TouchFavRow> rows;
        CollectTouchFavRows(rows);
        int rowDy = DpiScale(hwnd, TouchSidebarListRowDy());
        int y0 = TouchFavRowsTop(win);
        if (TouchFavToolbarToggleRect(win).Contains(pt)) {
            gGlobalPrefs->favoritesInToolbar = !gGlobalPrefs->favoritesInToolbar;
            SaveSettings();
            UpdateTopBarForWindow(win);
            HwndInvalidate(hwnd, false);
            return true;
        }
        // add the current page
        if (win->IsDocLoaded() && TouchFavAddRect(win).Contains(pt)) {
            int pageNo = win->ctrl->CurrentPageNo();
            AddFavoriteQuiet(win, pageNo, FavoriteDefaultNameTemp(win, pageNo));
            UpdateTopBarForWindow(win);
            HwndInvalidate(hwnd, false);
            return true;
        }
        // same origin and row height the paint pass used, so the row under the
        // finger is the row that was drawn there
        if (rowDy > 0 && pt.y >= y0) {
            int idx = (pt.y - y0) / rowDy;
            if (idx >= 0 && idx < len(rows) && !rows[idx].isHeader) {
                Rect row{DpiScale(hwnd, 12), y0 + idx * rowDy, HwndClientRect(hwnd).dx - DpiScale(hwnd, 24), rowDy};
                if (TouchFavDeleteRect(win, row).Contains(pt)) {
                    DelFavorite(rows[idx].fs->filePath, rows[idx].fav->pageNo);
                    UpdateTopBarForWindow(win);
                    HwndInvalidate(hwnd, false);
                    return true;
                }
                // GoToFavorite opens the document first when it is not the
                // current one, so a favorite in another PDF just works
                GoToFavorite(win, rows[idx].fs, rows[idx].fav);
                return true;
            }
        }
        return false;
    }
    if (win->touchPanelMode == TouchPanelMode::Thumbnails && win->ctrl) {
        int count = win->ctrl->PageCount();
        for (int i = 0; i < count; i++) {
            if (TouchThumbnailRect(win, i).Contains(pt)) {
                win->ctrl->GoToPage(i + 1, true);
                HwndInvalidate(hwnd, false);
                return true;
            }
        }
    }
    if (win->touchPanelMode == TouchPanelMode::Search) {
        int controlsY = DpiScale(hwnd, kPanelHeaderDy + kPanelFilterDy + 4);
        int navDy = DpiScale(hwnd, 36);
        Rect prevRc{HwndClientRect(hwnd).dx - DpiScale(hwnd, 92), controlsY + DpiScale(hwnd, 4), navDy, navDy};
        Rect nextRc{HwndClientRect(hwnd).dx - DpiScale(hwnd, 48), prevRc.y, navDy, navDy};
        if (prevRc.Contains(pt)) {
            FindPrev(win);
            return true;
        }
        if (nextRc.Contains(pt)) {
            FindNext(win);
            return true;
        }
        int rowDy = DpiScale(hwnd, TouchSearchResultRowDy());
        int idx = (pt.y - TouchSearchResultsY(win)) / rowDy;
        if (pt.y >= TouchSearchResultsY(win) && idx >= 0 && idx < len(win->findMatches)) {
            Rect hit = TouchSearchResultRect(win, idx);
            hit.Inflate(0, DpiScale(hwnd, 4));
            if (hit.Contains(pt)) {
                const FindMatch& match = win->findMatches[idx];
                GoToFindMatch(win, match.startPage, match.startGlyph, match.endPage, match.endGlyph);
                HwndInvalidate(hwnd, false);
                return true;
            }
        }
    }
    if (win->touchPanelMode == TouchPanelMode::Annotations && win->AsFixed()) {
        int y = DpiScale(hwnd, kPanelHeaderDy + 12) - win->touchPanelScrollY;
        int rowDy = DpiScale(hwnd, TouchSidebarListRowDy());
        int idx = (pt.y - y) / rowDy;
        Vec<Annotation*> annotations;
        EngineMupdfGetAnnotations(win->AsFixed()->GetEngine(), annotations);
        if (pt.y >= y && idx >= 0 && idx < len(annotations)) {
            win->ctrl->GoToPage(PageNo(annotations[idx]), true);
            return true;
        }
    }
    return false;
}

// Drives the panel list's scroll easing / fling while it is moving. Same
// KineticScroll the Library columns use, so both decelerate identically.
constexpr UINT_PTR kTouchPanelScrollTimerId = 21;

static void UpdateTouchPanelScrollTimer(MainWindow* win, HWND hwnd) {
    if (KsIsMoving(win->touchPanelKs)) {
        SetTimer(hwnd, kTouchPanelScrollTimerId, kAnimTickMs, nullptr);
    } else {
        KillTimer(hwnd, kTouchPanelScrollTimerId);
    }
}

// The list is remeasured on every layout and other code paths set the scroll
// position directly (mode switch, document change), so resync before feeding it.
static void SyncTouchPanelScroll(MainWindow* win) {
    KsSetBounds(win->touchPanelKs, 0, TouchPanelMaxScroll(win));
    if (!KsIsMoving(win->touchPanelKs) && KsPos(win->touchPanelKs) != win->touchPanelScrollY) {
        KsSetPos(win->touchPanelKs, win->touchPanelScrollY);
    }
}

#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#endif

#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

static LRESULT CALLBACK WndProcTocBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*subclassId*/,
                                      DWORD_PTR /*data*/) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    // WC_STATIC without SS_NOTIFY answers WM_NCHITTEST with HTTRANSPARENT, so
    // every click and every touch fell straight through the panel to the frame
    // underneath and WM_LBUTTONUP / WM_POINTERDOWN never arrived here at all.
    // That is why tapping a search result, a thumbnail or the filter's clear X
    // did nothing, and why the panel could not be dragged to scroll. Under the
    // touch chrome the panel draws and hit-tests its own content, so it has to
    // be solid. Classic chrome keeps the pass-through behavior it always had.
    if (msg == WM_NCHITTEST && IsTouchChrome(win)) {
        return HTCLIENT;
    }

    // The panel is a WC_STATIC: it paints its own background, so drawing the
    // filter pill on WM_ERASEBKGND gets wiped. Draw after the default paint;
    // the edit control paints itself afterwards inside its own rect, and only
    // the pill's rounded ends and border show around it.
    if (msg == WM_PAINT && IsTouchChrome(win)) {
        if (win->touchPanelMode != TouchPanelMode::Bookmarks) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            HdcFillRect(hdc, HwndClientRect(hwnd), ThemeHotBackgroundColor());
            PaintTouchPanelMode(win, hdc);
            EndPaint(hwnd, &ps);
            if (win->touchPanelMode == TouchPanelMode::Search) {
                // false: PaintTouchPanelMode() just drew the results list under
                // the field, so the chrome must not erase down to the bottom
                PaintTouchFilterChrome(win, false);
            }
            return 0;
        }
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        PaintTouchFilterChrome(win, true);
        if (HasTocFilter(win)) {
            HDC hdc = GetDC(hwnd);
            if (hdc) {
                WindowTab* tab = win->CurrentTab();
                int total = tab && tab->currToc ? CountTocLeaves(tab->currToc->root) : 0;
                int found = win->tocFilteredTree ? CountTocLeaves(win->tocFilteredTree->root) : 0;
                int dy = DpiScale(hwnd, 32);
                Rect chip{HwndClientRect(hwnd).dx - DpiScale(hwnd, 20) - DpiScale(hwnd, 92), DpiScale(hwnd, 16),
                          DpiScale(hwnd, 92), dy};
                FillTocPill(hdc, chip, dy / 2, RGB(234, 229, 222), RGB(234, 229, 222));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, ThemeWindowDarkerTextColor());
                HdcDrawText(hdc, fmt("%d of %d", found, total), chip,
                            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX,
                            HdcGetUiFont(hdc, kFontSizeMeta, FW_MEDIUM));
                ReleaseDC(hwnd, hdc);
            }
        }
        return r;
    }

    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    switch (msg) {
        case WM_SIZE:
            win->touchPanelScrollY = std::min(win->touchPanelScrollY, TouchPanelMaxScroll(win));
            LayoutTocContainer(win);
            break;

        case WM_MOUSEWHEEL:
            if (win->touchPanelMode != TouchPanelMode::Bookmarks) {
                int step = DpiScale(hwnd, TouchSidebarRowDy());
                int direction = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
                SyncTouchPanelScroll(win);
                KsScrollBy(win->touchPanelKs, direction * step);
                win->touchPanelScrollY = KsPos(win->touchPanelKs);
                UpdateTouchPanelScrollTimer(win, hwnd);
                HwndInvalidate(hwnd, false);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wp == kTouchPanelPressTimerId) {
                HwndInvalidate(hwnd, false);
                UpdatePanelPressTimer(win);
                return 0;
            }
            if (wp == kTouchPanelScrollTimerId) {
                bool moving = KsTick(win->touchPanelKs);
                win->touchPanelScrollY = KsPos(win->touchPanelKs);
                HwndInvalidate(hwnd, false);
                if (!moving) {
                    UpdateTouchPanelScrollTimer(win, hwnd);
                }
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_TOC_LABEL_WITH_CLOSE) {
                ToggleTocBox(win);
            }
            break;

        case WM_LBUTTONDOWN:
            SetTouchPanelPressed(win, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, true);
            if (win->touchPanelMode == TouchPanelMode::Favorites) {
                Vec<TouchFavRow> rows;
                CollectTouchFavRows(rows);
                int idx = TouchFavRowAt(win, GET_Y_LPARAM(lp), len(rows));
                // headers are not draggable, and neither is the delete target
                if (idx >= 0 && !rows[idx].isHeader) {
                    Rect client = HwndClientRect(hwnd);
                    int rowDy = DpiScale(hwnd, TouchSidebarListRowDy());
                    Rect row{DpiScale(hwnd, 12), TouchFavRowsTop(win) + idx * rowDy,
                             client.dx - DpiScale(hwnd, 24), rowDy};
                    if (!TouchFavDeleteRect(win, row).Contains(Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
                        gFavDragWin = win;
                        gFavDragFromRow = idx;
                        gFavDragOverRow = idx;
                        gFavDragActive = false; // becomes a drag once it moves
                        SetCapture(hwnd);
                    }
                }
            }
            CloseTouchDocumentOverlays(win);
            break;

        case WM_MOUSEMOVE:
            if (gFavDragWin == win && gFavDragFromRow >= 0) {
                Vec<TouchFavRow> rows;
                CollectTouchFavRows(rows);
                int idx = TouchFavRowAt(win, GET_Y_LPARAM(lp), len(rows));
                // only within the same document, and never onto a heading
                if (idx >= 0 && !rows[idx].isHeader && rows[idx].fs == rows[gFavDragFromRow].fs) {
                    if (idx != gFavDragOverRow) {
                        gFavDragOverRow = idx;
                        HwndInvalidate(hwnd, false);
                    }
                    if (idx != gFavDragFromRow) {
                        gFavDragActive = true;
                        SetTouchPanelPressed(win, Point{}, false);
                    }
                }
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            SetTouchPanelPressed(win, Point{}, false);
            if (gFavDragWin == win && gFavDragFromRow >= 0) {
                bool wasDrag = gFavDragActive;
                int from = gFavDragFromRow;
                int to = gFavDragOverRow;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                ResetFavDrag();
                if (wasDrag && from != to) {
                    Vec<TouchFavRow> rows;
                    CollectTouchFavRows(rows);
                    if (from < len(rows) && to < len(rows) && rows[from].fs == rows[to].fs) {
                        // flattened rows include headings, so convert to
                        // indices within this document's own favorites
                        int fromIdx = 0, toIdx = 0, seen = 0;
                        for (int i = 0; i < len(rows); i++) {
                            if (rows[i].isHeader || rows[i].fs != rows[from].fs) {
                                continue;
                            }
                            if (i == from) {
                                fromIdx = seen;
                            }
                            if (i == to) {
                                toIdx = seen;
                            }
                            seen++;
                        }
                        MoveFavorite(rows[from].fs->filePath, fromIdx, toIdx);
                    }
                    UpdateTopBarForWindow(win);
                    HwndInvalidate(hwnd, false);
                    return 0; // a drag is not a tap
                }
            }
            if (ActivateTouchPanelAt(win, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
                return 0;
            }
            break;

        case WM_POINTERDOWN:
            if (win->touchPanelMode != TouchPanelMode::Bookmarks && win->touchPanelPointerId == 0) {
                win->touchPanelPointerId = LOWORD(wp);
                win->touchPanelPointerStart = HwndScreenToClient(hwnd, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                win->touchPanelPointerStartScrollY = win->touchPanelScrollY;
                win->touchPanelPointerMoved = false;
                SetTouchPanelPressed(win, win->touchPanelPointerStart, true);
                // touching a coasting list catches it
                SyncTouchPanelScroll(win);
                KsStop(win->touchPanelKs);
                UpdateTouchPanelScrollTimer(win, hwnd);
                KsDragBegin(win->touchPanelKs, win->touchPanelPointerStart.y);
                CloseTouchDocumentOverlays(win);
                return 0;
            }
            break;

        case WM_POINTERUPDATE:
            if (LOWORD(wp) == win->touchPanelPointerId) {
                Point pt = HwndScreenToClient(hwnd, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                int dy = pt.y - win->touchPanelPointerStart.y;
                int threshold = DpiScale(hwnd, 6);
                if (!win->touchPanelPointerMoved && abs(dy) >= threshold) {
                    win->touchPanelPointerMoved = true;
                }
                if (win->touchPanelPointerMoved) {
                    // became a drag, so it is no longer a press on a row
                    SetTouchPanelPressed(win, Point{}, false);
                    KsDragUpdate(win->touchPanelKs, pt.y);
                    win->touchPanelScrollY = KsPos(win->touchPanelKs);
                    HwndInvalidate(hwnd, false);
                }
                return 0;
            }
            break;

        case WM_POINTERUP:
            if (LOWORD(wp) == win->touchPanelPointerId) {
                Point pt = HwndScreenToClient(hwnd, Point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                bool activate = !win->touchPanelPointerMoved;
                SetTouchPanelPressed(win, Point{}, false);
                win->touchPanelPointerId = 0;
                win->touchPanelPointerMoved = false;
                // let go of a flick and the list coasts to a stop
                KsDragEnd(win->touchPanelKs);
                UpdateTouchPanelScrollTimer(win, hwnd);
                if (activate) {
                    ActivateTouchPanelAt(win, pt);
                }
                return 0;
            }
            break;

        case WM_POINTERCAPTURECHANGED:
            if (LOWORD(wp) == win->touchPanelPointerId) {
                win->touchPanelPointerId = 0;
                win->touchPanelPointerMoved = false;
                return 0;
            }
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SubclassToc(MainWindow* win) {
    HWND hwndTocBox = win->hwndTocBox;

    if (win->tocBoxSubclassId == 0) {
        win->tocBoxSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(hwndTocBox, WndProcTocBox, win->tocBoxSubclassId, (DWORD_PTR)win);
        if (!ok) {
            // can fail under low memory / desktop heap exhaustion, so don't assert
            logf("SubclassToc: SetWindowSubclass() failed, err: %d\n", (int)GetLastError());
            win->tocBoxSubclassId = 0;
        }
    }
}

void UnsubclassToc(MainWindow* win) {
    if (win->tocBoxSubclassId != 0) {
        RemoveWindowSubclass(win->hwndTocBox, WndProcTocBox, win->tocBoxSubclassId);
        win->tocBoxSubclassId = 0;
    }
}

// Append a TocItem linked list onto resultFirst/resultLast (updates last).
static void AppendTocSiblingList(TocItem*& resultFirst, TocItem*& resultLast, TocItem* list) {
    if (!list) {
        return;
    }
    if (!resultFirst) {
        resultFirst = list;
    } else {
        resultLast->next = list;
    }
    resultLast = list;
    while (resultLast->next) {
        resultLast = resultLast->next;
    }
}

// Search results are deliberately flat: only leaves are actionable results,
// and a match can come from either the leaf title or its immediate parent.
static TocItem* FilterTocLeaves(TocItem* item, const StrVec& words) {
    TocItem* resultFirst = nullptr;
    TocItem* resultLast = nullptr;
    for (TocItem* si = item; si; si = si->next) {
        if (si->child) {
            AppendTocSiblingList(resultFirst, resultLast, FilterTocLeaves(si->child, words));
            continue;
        }
        bool titleMatches = si->title && FilterMatches(si->title, words);
        bool parentMatches = si->parent && si->parent->title && FilterMatches(si->parent->title, words);
        if (titleMatches || parentMatches) {
            auto* copy = AllocTocItem(nullptr, si->title, si->pageNo);
            copy->id = si->id;
            copy->fontFlags = si->fontFlags;
            copy->color = si->color;
            copy->dest = si->dest;
            copy->destNotOwned = true;
            copy->isOpenDefault = true;
            copy->isOpenToggled = false;
            AppendTocSiblingList(resultFirst, resultLast, copy);
        }
    }
    return resultFirst;
}

static void ApplyTocFilter(MainWindow* win, Str filter) {
    if (!win->tocLoaded) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->currToc) {
        return;
    }
    // free previous filtered tree
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;

    TreeView* treeView = win->tocTreeView;
    TocTree* origTree = tab->currToc;

    StrVec words;
    if (filter) {
        SplitFilterToWords(filter, words);
    }
    if (len(words) == 0) {
        // restore original tree
        TreeView_SetItemHeight(treeView->hwnd, DpiScale(treeView->hwnd, TouchSidebarRowDy()));
        SetInitialExpandState(origTree->root, tab->tocState);
        treeView->SetTreeModel(origTree);
        HwndInvalidate(win->hwndTocBox, false);
        return;
    }

    TreeView_SetItemHeight(treeView->hwnd, DpiScale(treeView->hwnd, 52));
    TocItem* filteredItems = FilterTocLeaves(origTree->root, words);
    if (!filteredItems) {
        treeView->Clear();
        HwndInvalidate(win->hwndTocBox, false);
        return;
    }
    // TreeView populates Root()'s children only (the root itself is invisible).
    // Promote-filter returns a sibling list of matching items, so wrap them in
    // a dummy root — same shape every engine uses for the unfiltered TocTree.
    auto* wrapRoot = AllocTocItem(nullptr, {}, 0);
    wrapRoot->child = filteredItems;
    for (TocItem* c = filteredItems; c; c = c->next) {
        c->parent = wrapRoot;
    }
    auto* filteredTree = new TocTree(wrapRoot);
    win->tocFilteredTree = filteredTree;
    treeView->SetTreeModel(filteredTree);
    HwndInvalidate(win->hwndTocBox, false);
}

void TocFilterChanged(MainWindow* win) {
    Edit* edit = win->tocFilterEdit;
    if (!edit) {
        return;
    }
    TempStr filter = edit->GetTextTemp();
    ApplyTocFilter(win, filter);
}

static void OnTocFilterTextChanged(MainWindow* win) {
    if (IsTouchChrome(win)) {
        Str* savedQuery = TouchPanelSearchQuery(win, win->touchPanelMode);
        if (savedQuery) {
            str::ReplaceWithCopy(savedQuery, win->tocFilterEdit->GetTextTemp());
        }
    }
    if (IsTouchChrome(win) && win->touchPanelMode == TouchPanelMode::Search) {
        TempStr text = win->tocFilterEdit->GetTextTemp();
        if (text) {
            SearchDocumentFromTouchPanel(win, text);
        } else {
            AbortFinding(win, true);
            ClearSearchResult(win);
        }
        HwndInvalidate(win->hwndTocBox, false);
        return;
    }
    TocFilterChanged(win);
}

static LRESULT CALLBACK WndProcTocFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*subclassId*/,
                                             DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_LBUTTONDOWN) {
        CloseTouchDocumentOverlays(win);
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_DOWN) {
            // move into the tree: first top-level bookmark
            if (win && win->tocTreeView) {
                SelectFirstTocTreeItem(win);
                HwndSetFocus(win->tocTreeView->hwnd);
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            Edit* edit = win ? win->tocFilterEdit : nullptr;
            if (edit) {
                TempStr txt = edit->GetTextTemp();
                if (txt && len(txt) > 0) {
                    edit->SetText("");
                    // onTextChanged will fire and restore the tree
                    return 0;
                }
                // empty: move focus to the tree
                if (win->tocTreeView) {
                    SetFocus(win->tocTreeView->hwnd);
                }
                return 0;
            }
        }
        if (wp == VK_RETURN) {
            // prevent ding; navigation is done from the tree
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == '\r' || wp == '\n')) {
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static TempStr TocStickyBreadcrumbTemp(MainWindow* win);

static void UpdateTocStickyHeader(MainWindow* win) {
    if (!win || !win->hwndTocSticky || !win->tocTreeView || !HwndIsVisible(win->tocTreeView->hwnd) ||
        win->touchPanelMode != TouchPanelMode::Bookmarks || HasTocFilter(win)) {
        if (win && win->hwndTocSticky) {
            HwndHide(win->hwndTocSticky);
            str::FreePtr(&win->tocStickyText);
        }
        return;
    }
    TempStr breadcrumb = TocStickyBreadcrumbTemp(win);
    if (len(breadcrumb) == 0) {
        str::FreePtr(&win->tocStickyText);
        HwndShow(win->hwndTocSticky);
        HwndInvalidate(win->hwndTocSticky, false);
        return;
    }
    if (!str::Eq(win->tocStickyText, breadcrumb)) {
        str::ReplaceWithCopy(&win->tocStickyText, breadcrumb);
        HwndInvalidate(win->hwndTocSticky, true);
    }
    HwndShow(win->hwndTocSticky);
}

static TempStr TocStickyBreadcrumbTemp(MainWindow* win) {
    if (!win || !win->tocTreeView) {
        return str::DupTemp("");
    }
    HWND tree = win->tocTreeView->hwnd;
    HTREEITEM first = TreeView_GetNextItem(tree, nullptr, TVGN_FIRSTVISIBLE);
    if (!first) {
        return str::DupTemp("");
    }
    RECT firstRect{};
    TreeView_GetItemRect(tree, first, &firstRect, FALSE);
    HTREEITEM deepestStuck = firstRect.top < 0 ? first : TreeView_GetParent(tree, first);
    TocItem* chain[64]{};
    int count = 0;
    for (HTREEITEM handle = deepestStuck; handle && count < dimofi(chain); handle = TreeView_GetParent(tree, handle)) {
        TocItem* item = (TocItem*)win->tocTreeView->GetTreeItemByHandle(handle);
        if (item && item->title) {
            chain[count++] = item;
        }
    }
    if (count == 0) {
        return str::DupTemp("");
    }
    Str root = chain[count - 1]->title;
    if (count == 1) {
        return str::DupTemp(root);
    }

    // The sticky label identifies the document's current top-level section,
    // not every nested group above the first visible row. Prefer the complete
    // root + section breadcrumb, but shorten the root before sacrificing the
    // section name. A character-count cutoff truncated proportional fonts far
    // too early and could reduce "OCTOECHOS › Third Mode" to a deeper group.
    Str section = chain[count - 2]->title;
    TempStr full = fmt("%s › %s", root, section);
    Rect rc = HwndClientRect(win->hwndTocSticky);
    int maxDx = std::max(0, rc.dx - DpiScale(win->hwndTocSticky, 28));
    HDC hdc = GetDC(win->hwndTocSticky);
    HFONT font = HdcGetUiFont(hdc, kFontSizeMeta, FW_MEDIUM);
    int fullDx = HdcMeasureText(hdc, full, font).dx;
    ReleaseDC(win->hwndTocSticky, hdc);
    if (fullDx <= maxDx) {
        return str::DupTemp(full);
    }
    return fmt("… › %s", section);
}

static LRESULT CALLBACK WndProcTocSticky(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_ERASEBKGND) {
        return TRUE;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        Rect rc = HwndClientRect(hwnd);
        HdcFillRect(hdc, rc, ThemeHotBackgroundColor());
        Str label = win ? win->tocStickyText : Str{};
        if (len(label) > 0) {
            Rect text = rc;
            text.Inflate(-DpiScale(hwnd, 14), 0);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, label, text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                        HdcGetUiFont(hdc, kFontSizeMeta, FW_MEDIUM));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProcTocTreeStickyTracker(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                                    DWORD_PTR data) {
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_LBUTTONDOWN) {
        CloseTouchDocumentOverlays(win);
    }
    if (msg == WM_PAINT && win && HasTocFilter(win) && !win->tocFilteredTree) {
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            TempStr query = win->tocFilterEdit ? win->tocFilterEdit->GetTextTemp() : TempStr{};
            Rect text = HwndClientRect(hwnd);
            text.y += DpiScale(hwnd, 40);
            text.dy = DpiScale(hwnd, 80);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThemeWindowDarkerTextColor());
            HdcDrawText(hdc, fmt("No bookmarks match \"%s\"", query), text,
                        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS,
                        HdcGetUiFont(hdc, kFontSizeLabel));
            ReleaseDC(hwnd, hdc);
        }
    }
    if (msg == WM_PAINT || msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_GESTURE || msg == WM_KEYUP ||
        msg == WM_SIZE || msg == TVM_EXPAND || msg == TVM_ENSUREVISIBLE || msg == TVM_SELECTITEM) {
        UpdateTocStickyHeader(win);
    }
    return result;
}

void CreateToc(MainWindow* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = IsTouchChrome(win) ? DpiScale(win->hwndFrame, kPanelDx) : gGlobalPrefs->sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndTocBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, nullptr, hmod, nullptr);

    auto* l = new LabelWithCloseWnd();
    {
        LabelWithCloseWnd::CreateArgs args;
        args.parent = win->hwndTocBox;
        args.cmdId = IDC_TOC_LABEL_WITH_CLOSE;
        args.isRtl = IsUIRtl();
        args.font = GetAppSidebarLabelFont(win->hwndFrame);
        l->Create(args);
    }
    win->tocLabelWithClose = l;
    // The rail toggles the panel, so the header's close X is redundant - and a
    // hairline system glyph among custom-drawn controls. 4a's header is just
    // the title; drop the X under the touch chrome.
    l->showClose = !gGlobalPrefs->touchChrome;
    l->SetPaddingXY(gGlobalPrefs->touchChrome ? 16 : 2, gGlobalPrefs->touchChrome ? 19 : 2);
    // label is set in UpdateToolbarSidebarText()

    auto* filterEdit = new Edit();
    {
        Edit::CreateArgs eargs;
        eargs.parent = win->hwndTocBox;
        eargs.withBorder = false;
        // underline so the filter field is visible on flat sidebar backgrounds.
        // The redesigned panel draws a pill behind it instead (WndProcTocBox).
        eargs.withBottomBorder = !gGlobalPrefs->touchChrome;
        eargs.cueText = _TRA("Search Bookmarks");
        eargs.font = GetAppFont(win->hwndFrame);
        filterEdit->Create(eargs);
    }
    win->tocFilterEdit = filterEdit;
    if (gGlobalPrefs->touchChrome) {
        // Inset so the pill drawn behind it (WndProcTocBox) has room for its
        // rounded ends; stretched edge to edge there is nowhere to draw them.
        // The extra left inset leaves room for the magnifier, which the parent
        // draws - the edit paints over anything inside its own rect.
        filterEdit->SetInsetsPt(12, 46, 12, kPanelFilterIconGap);
    }
    filterEdit->onTextChanged = MkFunc0(OnTocFilterTextChanged, win);
    SetWindowSubclass(filterEdit->hwnd, WndProcTocFilterEdit, NextSubclassId(), (DWORD_PTR)win);

    auto* treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndTocBox;
    args.font = GetAppTreeFont(win->hwndFrame);
    args.fullRowSelect = true;
    args.exStyle = 0;
    args.isRtl = IsUIRtl();
    if (gGlobalPrefs->touchChrome) {
        args.itemDy = TouchSidebarRowDy();
        // 4a indents each level by its chevron column + gap, so a leaf sits
        // clear of its parent's disclosure. The stock indent is too tight.
        args.indentDx = kPanelIndentDx;
        // draw 4a's own chevron glyph instead of the system +/-
        args.noSystemButtons = true;
    }

    auto fn = MkFunc1Void(TocContextMenu);
    treeView->onContextMenu = fn;
    treeView->onSelectionChanged = MkFunc1Void(TocTreeSelectionChanged);
    treeView->onKeyDown = MkFunc1Void(TocTreeKeyDown);
    treeView->onGetTooltip = MkFunc1Void(TocCustomizeTooltip);
    treeView->onClick = MkFunc1Void(TocTreeClick);

    treeView->Create(args);
    ReportIf(!treeView->hwnd);
    win->tocTreeView = treeView;
    win->hwndTocSticky = CreateWindowExW(0, WC_STATIC, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, win->hwndTocBox,
                                         nullptr, hmod, nullptr);
    SetWindowSubclass(win->hwndTocSticky, WndProcTocSticky, NextSubclassId(), (DWORD_PTR)win);
    SetWindowSubclass(treeView->hwnd, WndProcTocTreeStickyTracker, NextSubclassId(), (DWORD_PTR)win);

    // stack label, filter edit and tree vertically; the tree flexes to fill the
    // remaining height. The VBox owns these controls/spacer (freed in ~MainWindow).
    auto* vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;
    vbox->AddChild(l);
    vbox->AddChild(filterEdit);
    vbox->AddChild(new Spacer(0, 2)); // gap under the search field
    vbox->AddChild(treeView, 1);
    win->tocLayout = vbox;

    SubclassToc(win);

    UpdateControlsColors(win);
}
