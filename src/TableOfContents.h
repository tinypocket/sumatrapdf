/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

void CreateToc(MainWindow*);
void ClearTocBox(MainWindow*);
void ToggleTocBox(MainWindow*);
void LoadTocTree(MainWindow*);
void UpdateTocSelection(MainWindow*, int currPageNo);
void ExpandTocToCurrentPage(MainWindow*);
void UpdateTocExpansionState(Vec<int>& tocState, TreeView*, TocTree*);
void UnsubclassToc(MainWindow*);
void TocFilterChanged(MainWindow*);
void UpdateTouchPanelMode(MainWindow*);
bool IsTouchSearchPanelVisible(MainWindow*);
// Ctrl+F and the find bar's pop-out in the touch chrome: the Search panel is
// the find UI. Carries the find bar's text over and focuses the field.
void OpenTouchSearchPanel(MainWindow*);
// the panel's collapse: back to the compact find bar with the query
void CollapseTouchSearchPanelToBar(MainWindow*);
// drops the Annotations panel's cached list (the document changed, or its
// annotations did); the panel re-gathers it on a worker thread
void InvalidateTouchAnnotations(MainWindow*);
// the touch Search panel's current query, or {} when that panel isn't the one
// driving the find (then win->hwndFindEdit is the source, as it always was)
TempStr TouchSearchPanelQueryTemp(MainWindow*);
void SetTouchPanelModeAndRestoreSearch(MainWindow*, TouchPanelMode);
int TouchSidebarRowDy();

// When true (default), the bookmarks pane highlights every TOC entry that
// matches the current page (same page number as the best match, plus the
// ancestor chain), not only the single TreeView selection (issue #4642).
// Flip to false to restore single-highlight-only behavior.
extern bool gShowAllMatchingTOC;

// navigate to a TocItem (used by the command palette's TOC mode)
void GoToTocItem(MainWindow*, TocItem*);

// shared with Favorites.cpp
// void TocCustomizeTooltip(TreeItem::GetTooltipEvent*);
// LRESULT TocTreeKeyDown2(TreeKeyDownEvent*);
