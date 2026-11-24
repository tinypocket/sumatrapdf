/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct DrawModeState;

constexpr UINT_PTR kDrawHoldTimerID = 17;
constexpr UINT_PTR kDrawPendingTimerID = 18;

bool IsDrawModeOn(MainWindow*);
void ToggleDrawMode(MainWindow*);
void ExitDrawMode(MainWindow*);
void DrawModeUndo(MainWindow*);
bool DrawModeCanUndo(MainWindow*);
bool DrawModeCancelStroke(MainWindow*);
void DeleteDrawMode(MainWindow*);

bool DrawModeOnMouseDown(MainWindow*, int x, int y);
bool DrawModeOnMouseMove(MainWindow*, int x, int y);
bool DrawModeOnMouseUp(MainWindow*, int x, int y);
bool DrawModeSetCursor(MainWindow*, int x, int y);
void DrawModeOnTimer(MainWindow*, UINT_PTR timerId);
void PaintDrawModeOverlay(MainWindow*, HDC);
void LayoutDrawModeToolbar(MainWindow*, Rect canvasRc);
