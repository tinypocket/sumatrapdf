/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

TempStr GetWebView2VersionTemp();
bool HasWebView();

// TODO: maybe hide those inside a private struct
typedef interface ICoreWebView2 ICoreWebView2;
typedef interface ICoreWebView2Controller ICoreWebView2Controller;

using WebViewMsgCb = Func1<Str>;

struct WebViewResourceResult {
    const u8* data = nullptr;
    size_t dataLen = 0;
    Str contentType;
    bool ownsData = true;
};

// resolveAccelCmd return value asking the webview to forward the key press
// itself (WM_KEYDOWN/UP) to the top-level window, rather than a command. For
// keys handled by the frame's key handler instead of an accelerator (e.g. Esc).
constexpr int kWebViewForwardKey = -1;

enum class WebViewProcessFailure {
    // the whole WebView2 browser process died: the control is unusable
    BrowserExited,
    // only a renderer died or hung: reloading usually recovers
    RenderExited,
    RenderUnresponsive,
    // some other helper process (GPU, utility, ...) died
    Other,
};

struct WebViewEvents {
    void* ctx = nullptr;
    bool (*navigationStarting)(void* ctx, Str url, bool newWindow) = nullptr;
    void (*navigationCompleted)(void* ctx, Str url, bool success) = nullptr;
    void (*historyChanged)(void* ctx, bool canGoBack, bool canGoForward) = nullptr;
    // the page's document title changed (fires on load and on any later change).
    // The in-product browser labels its tabs with it.
    void (*documentTitleChanged)(void* ctx, Str title) = nullptr;
    // The HTTP response for a TOP-LEVEL document navigation arrived (headers
    // only; the body is still streaming). `contentType` is the raw Content-Type
    // header, "" when the server sent none. This is the only reliable way to
    // learn that a URL with no file extension is really a PDF - by the time the
    // page has loaded, WebView2's built-in Edge PDF viewer is already showing
    // it. Fires before the document renders, so the host can take over.
    void (*mainDocumentResponse)(void* ctx, Str url, Str contentType) = nullptr;
    // maps an accelerator key press inside the webview to an app command id to
    // post (WM_COMMAND) to the top-level window, or 0 to leave it to the
    // webview, or kWebViewForwardKey to re-post the key itself. Lets the host
    // forward its keyboard shortcuts that the webview would otherwise swallow.
    int (*resolveAccelCmd)(void* ctx, u16 vk, bool ctrl, bool shift, bool alt) = nullptr;
    // a WebView2 process died. Return true if handled; returning false runs the
    // default recovery (reload for a dead renderer, fail the control for a dead
    // browser process). Called on the UI thread.
    bool (*processFailed)(void* ctx, WebViewProcessFailure kind) = nullptr;
    // a JS call made through window.__sumatra__.call(name, ...) / a bound name.
    // paramsJson is the arguments as a JSON array. Reply with WebviewWnd::Resolve
    // (may be async); not replying leaves the JS promise pending forever.
    void (*jsCall)(void* ctx, Str id, Str method, Str paramsJson) = nullptr;
    // window.__sumatra__.notify(name, ...): one-way, no reply and no promise, so
    // it's the right channel for high-frequency events like scrolling
    void (*jsNotify)(void* ctx, Str method, Str paramsJson) = nullptr;
};

struct WebViewResourceProvider {
    void* ctx = nullptr;
    bool (*getResource)(void* ctx, Str path, WebViewResourceResult* res) = nullptr;
};

struct PendingWebViewOp {
    enum Kind {
        Init,
        SetHtml,
        Eval,
        Navigate,
    };

    Kind kind;
    Str text;
    // for Init: the token AddInitScript() already handed back to the caller, so
    // the script keeps its identity across the queue
    int token = 0;
};

// An init script (AddScriptToExecuteOnDocumentCreated). `id` is assigned
// asynchronously by WebView2 and is what RemoveScriptToExecuteOnDocumentCreated
// needs, so a script can only be removed once its id has arrived.
struct WebViewInitScript {
    int token = 0;
    WStr id;
    // remove as soon as the id arrives (RemoveInitScript ran before that)
    bool removePending = false;
};

struct CreateWebViewArgs {
    HWND parent = nullptr;
    Rect pos;
};

struct WebviewWnd : Wnd {
    WebviewWnd();
    ~WebviewWnd() override;

    HWND Create(const CreateWebViewArgs&);

    void Eval(Str js);
    void SetHtml(Str html);
    void Init(Str js);
    int AddInitScript(Str js);
    void AddInitScriptWithToken(Str js, int token);
    int FindInitScript(int token) const;
    void RemoveInitScript(int token);
    void RemoveAllInitScripts();
    void OnInitScriptAdded(int token, const WCHAR* id);
    void Navigate(Str url);
    // makes window.<name>(...) available to page JS, returning a promise that
    // WebViewEvents::jsCall resolves via Resolve()
    void Bind(Str name);
    void Unbind(Str name);
    // status 0 resolves the JS promise, non-0 rejects it. resultJson must be
    // valid JSON (or empty for undefined)
    void Resolve(Str id, int status, Str resultJson);
    void OnJsCall(Str msg);
    void OnJsNotify(Str msg);
    void RebuildBindScript();
    void GoBack();
    void GoForward();
    // the document's current title (""/empty when there is none). WebView2 only
    // raises documentTitleChanged when the title CHANGES, so a reload of the
    // same page never re-reports it - a host that dropped its copy on
    // navigationStarting has to ask for it again once the load completes.
    TempStr GetDocumentTitle() const;
    void SetZoomPercent(int zoom);
    int GetZoomPercent() const;
    bool CanGoBack() const;
    bool CanGoForward() const;
    void Focus();
    // open the WebView2 (Chromium) find-on-page bar, like a browser's Ctrl+F
    void ShowFindUI();
    void RegisterForwardingDropTarget();
    void RevokeForwardingDropTarget();
    bool Embed(WebViewMsgCb& cb);
    void OnControllerReady(ICoreWebView2Controller* controller);
    // The browser profile's cookies for url as one "Cookie: a=b; c=d\r\n"
    // request header line, delivered asynchronously on the UI thread ({} when
    // there are none or the runtime is too old). A download made outside the
    // webview needs these to pass as the logged-in session.
    void GetCookieHeaderAsync(Str url, const Func1<Str>& cb);
    void OnProcessFailed(WebViewProcessFailure kind);
    void FailInit();
    void QueuePendingOp(PendingWebViewOp::Kind kind, Str text, int token = 0);
    void FlushPendingOps();
    void SetControllerVisible(bool visible);

    virtual void OnBrowserMessage(Str msg);

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void UpdateWebviewSize();

    // this is where the webview2 control stores data
    // must be set before we call create
    // TODO: make Webview2CreateCustomArgs
    // with dataDir
    Str dataDir;
    // DWORD m_main_thread = GetCurrentThreadId();
    ICoreWebView2* webview = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    // forwards file drops to the parent window when allowExternalDrop is false;
    // registered on the host hwnd and every WebView2 child window (the Chrome_*
    // composition windows that actually sit under the cursor)
    struct IDropTarget* dropTarget = nullptr;
    Vec<HWND> dropTargetHwnds;

    bool initStarted = false;
    bool initFailed = false;
    bool isVisible = true;
    bool isSuspended = false;
    bool isInSizeMove = false;
    Rect lastBounds{};
    bool hasLastBounds = false;
    WStr userDataFolder;
    WStr resourceUriPrefix;
    // URI of the top-level navigation currently in flight, kept only so
    // WebResourceResponseReceived can recognise the main document's response
    // when Chromium's Sec-Fetch-Dest header isn't there to say so
    Str pendingNavUrl;
    WebViewResourceProvider resourceProvider;
    WebViewEvents events;
    bool forwardAppAccelerators = true;
    bool allowClipboardRead = false;
    // in-product browser: turn on WebView2's own password autosave + autofill
    // (stored in dataDir, separate from the user's Edge profile) and browser
    // chrome (default context menus). Off for the manual / AI-chat webviews.
    bool enableAutofill = false;
    bool enableBrowserChrome = false;
    // Paint white behind the page, as a browser does, instead of the default
    // transparent background. A page that draws no background of its own - an
    // empty page a site sent back after a sign-in - otherwise showed whatever
    // was on screen behind the control: the Library, in the in-app browser.
    bool opaqueBackground = false;
    // when false, WebView2 won't claim external (file) drops, so they fall
    // through to the host window's drop target (e.g. to open the file)
    bool allowExternalDrop = true;
    Vec<PendingWebViewOp> pendingOps;
    Vec<WebViewInitScript> initScripts;
    int nextInitScriptToken = 1;
    // names exposed to JS via Bind(); the bind script is rebuilt when this changes
    Vec<Str> boundNames;
    int bindScriptToken = 0;
};
