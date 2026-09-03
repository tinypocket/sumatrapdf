/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct TopBarWnd;

// The custom-drawn top bar of the redesigned chrome: the document name, a page
// stepper and a zoom stepper, each in its own pill-shaped group, plus a
// settings button. It replaces the rebar toolbar while the rail is on.
//
// The redesign deliberately carries fewer controls than the classic toolbar:
// anything the rail or the page itself already offers is dropped from here.

// true when the touch chrome is in use: the custom toolbar, the touch-sized
// sidebar and the surfaces drawn to match. Independent of the rail, which is
// one of the two directions the touch chrome can take.
bool IsTouchChrome(MainWindow*);

void CreateTopBar(MainWindow*);
void DestroyTopBar(MainWindow*);
void UpdateTopBarForWindow(MainWindow*);
void UpdateTopBarAfterThemeChange(MainWindow*);
int GetTopBarDy(MainWindow*);
bool IsTopBarVisible(MainWindow*);

// The open-documents preview is shared by the 40px toolbar trigger and the
// 28px title-bar trigger. anchorRect is in anchorHwnd client coordinates.
void ShowTouchDocumentPreview(MainWindow*, HWND anchorHwnd, Rect anchorRect);
void HoverTouchDocumentPreview(MainWindow*, HWND anchorHwnd, Rect anchorRect, bool isOver);
void CloseTouchDocumentPreview(MainWindow*);
void CloseTouchDocumentOverlays(MainWindow*, bool commitEdits = true);
