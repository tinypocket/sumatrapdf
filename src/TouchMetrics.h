/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Sizes for the touch-friendly chrome ("icon rail" direction of the
// SumatraPDF Touch Redesign). All values are logical pixels at 100% scaling;
// call DpiScale() at the point of use, the same way ToolbarSize works.
//
// Hit targets are >= 44px so they can be hit with a finger on a convertible.
// Colors are not here: they come from the theme (see Theme.cpp, "Touch Paper").

// --- left icon rail ---------------------------------------------------------

#define kRailDx 64        // width of the rail
#define kRailBtnDy 52     // rail button is square: 52 x 52
#define kRailBtnRadius 12 // rounded corners of a rail button
#define kRailBtnGap 6     // vertical gap between rail buttons
#define kRailPadY 10      // padding above the first / below the last button
#define kRailMarkerDx 3   // accent marker on the left edge of the active button
#define kRailMarkerDy 24

// --- panel next to the rail (bookmarks, thumbnails, search, annotations) ----

#define kPanelDx 360 // default width, user-resizable via the splitter
#define kPanelMaxDx 520
#define kPanelMaxPercent 45
#define kPanelHeaderDy 64 // header holding the title and the count chip
#define kPanelTitleFontSize 18
#define kPanelChipDy 32      // "6 of 42" pill in the header
#define kPanelChipReserve 44 // gap kept at the right of the header row for it
#define kPanelFilterDy 40    // filter / search box
// left inset of the filter edit, leaving room for the magnifier the parent
// draws in the pill's left padding
#define kPanelFilterIconGap 46
#define kPanelFilterIconDy 16
#define kPanelRowDy 40 // exact 4a top-level row height
#define kPanelRowRadius 10
#define kPanelRowPadX 14
#define kPanelRowGap 12
#define kPanelChevronDx 18 // visual disclosure column; hit target remains larger
#define kPanelIndentDx 18  // compact nesting step; top-level inset is kPanelRowPadX
#define kPanelRowFontSize 15
#define kPanelSubFontSize 13 // "3 sections" under a row title
#define kPanelPageFontSize 14
#define kSidebarSplitterVisualDx 5
#define kSidebarSplitterHitDx 36
#define kSidebarSplitterDragThreshold 8
#define kSidebarCollapseDy 28
#define kSidebarToggleTop 26

// --- top bar ---------------------------------------------------------------

#define kTopBarDy 64      // height of the toolbar row
#define kTopBarBtnDy 44   // round toolbar button
#define kTopBarIconDy 24  // icon inside a toolbar button
#define kZoomLabelDx 60   // "88%", wide enough not to jump while zooming
#define kTopBarNameDx 280 // document name in the rail direction's bar (4a)
#define kPageBoxDy 44
#define kPageBoxDx 104 // "18 / 342"

// --- slim title bar (4a) ----------------------------------------------------

// Replaces the tab strip in the caption when a single document is open: just
// the document name and the window buttons.
#define kTitleBarDy 36
// Chrome's tab strip is 40dip tall with a 34dip tab in it; the pill below is
// drawn 4px shorter than the strip, so this lands on the same size.
#define kTitleBarTabsDy 40
#define kTitleBarBtnDx 46
#define kTitleBarPadX 4
#define kTouchMenuBarDy 32

// --- tabs, drawn as pills in the top bar ------------------------------------

#define kTabPillDy 40
#define kTabPillMinDx 130
#define kTabPillMaxDx 190
#define kTabPillInactiveMaxDx 190
// "larger tabs" setting: same strip, more room per label
#define kTabPillLargeMinDx 180
#define kTabPillLargeMaxDx 280
#define kTitleBarTabsLargeDy 60
// extra strip height when a tab label wraps onto a second line
#define kTabTwoRowExtraDy 16
#define kTabCloseDy 24 // close button inside a finger-sized title-bar tab
#define kTabPillGap 6  // horizontal gap between two tab pills

// floating page indicator, bottom right of the canvas. Only shown when tabs
// take up the top bar, otherwise the page box lives in the top bar.
#define kPagePillDy 52
#define kPagePillMargin 20

// --- type scale -------------------------------------------------------------

// The redesign carries its hierarchy in size *and* weight rather than size
// alone. Sizes are the same unscaled-px convention as everything else here.

#define kFontSizeTitle 22 // page and section titles ("Recent files")
#define kFontSizePanel 18 // panel headers ("Bookmarks")
#define kFontSizeBody 15  // row titles, body copy
#define kFontSizeLabel 14 // card names, page numbers
#define kFontSizeMeta 13  // secondary / muted text ("3 sections", file size)

// Weights are half the hierarchy in this design; sizes alone read flat.
#define kFontWeightStrong 600 // titles, card names, the document name
#define kFontWeightMedium 500 // page and zoom readouts

// --- home screen (recent files) ---------------------------------------------

#define kHomeHeaderDy 64
#define kHomeSearchDy 40
#define kHomeSearchMinDx 320
#define kHomeCardDx 148
#define kHomeCardDy 196
#define kHomeCardGap 20
#define kHomeBadgeDy 32 // pin / close target on a card
