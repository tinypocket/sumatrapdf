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

// in-product web browser shown for TouchView::Web (defined in SimpleBrowserWindow.cpp)
void ShowTouchWebView(MainWindow* win, bool show);
// Ctrl+L in the in-app browser: focus the address field with the whole address
// selected (SimpleBrowserWindow.cpp)
void TouchBrowserFocusAddressBar(MainWindow* win);
bool IsTouchBrowserUrlEdit(MainWindow* win, HWND hwnd);
void LayoutTouchWebView(MainWindow* win, Rect contentRc);
void DestroyTouchWebView(MainWindow* win);
void TouchWebGoHome(MainWindow* win);
void TouchWebToggleBookmark(MainWindow* win);
