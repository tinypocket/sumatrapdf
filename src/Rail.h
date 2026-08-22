/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
enum class TouchPanelMode;
enum class TouchView;

// The icon rail and its five mutually-exclusive 4a panel modes.

void CreateRail(MainWindow*);
void DestroyRail(MainWindow*);
void UpdateRailForWindow(MainWindow*);
void UpdateRailAfterThemeChange(MainWindow*);
int GetRailDx(MainWindow*);
bool IsRailVisible(MainWindow*);
void SetTouchPanelMode(MainWindow*, TouchPanelMode);
void SetTouchView(MainWindow*, TouchView);
void SetTouchSidebarCollapsed(MainWindow*, bool collapsed);
void SetTouchDocumentTab(MainWindow*, int tabIndex);
