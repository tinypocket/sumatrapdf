/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct SimpleBrowserCreateArgs {
    Str title;
    Rect pos{}; // if empty, will use CW_USEDEFAULT
    Str url;
    Str dataDir;
    WebViewResourceProvider resourceProvider;
    WStr resourceUriPrefix;
};

struct SimpleBrowserWindow : Wnd {
    WebviewWnd* webView = nullptr;
    Button* btnBack = nullptr;
    Button* btnForward = nullptr;
    HWND hwndUrl = nullptr;
    HFONT hFont = nullptr;
    bool webViewFocusSet = false;

    HWND Create(const SimpleBrowserCreateArgs&);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;
    bool PreTranslateMessage(MSG& msg) override;
    ~SimpleBrowserWindow() override;
};

SimpleBrowserWindow* SimpleBrowserWindowCreate(const SimpleBrowserCreateArgs&);

// The in-product embedded web browser (TouchView::Web) is implemented in
// SimpleBrowserWindow.cpp but declared in Rail.h so the rail / frame can call it
// without pulling in WebView types.