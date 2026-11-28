/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Saved site logins for the in-product browser.
//
// The page side is two scripts: BrowserLoginCaptureScript() runs in every page
// and posts a "save" message when the user signs in; BrowserLoginFillScriptTemp()
// is run on a page whose site has a saved login and fills the form in. The
// native side keeps the logins in Windows Credential Manager (generic
// credentials, encrypted by Windows with the user's login, never written to
// SumatraPDF's own files).
//
// A login belongs to an origin (scheme + host + port). The origin always comes
// from the webview (the document that sent the message, the page being
// filled), never from what a page says about itself, and the fill script checks
// location.origin again before it touches the form.

// "https://example.com" / "http://127.0.0.1:8080" for an http(s) url, lowercase,
// without user info, path, query or fragment; {} for anything else
TempStr BrowserLoginOriginTemp(Str url);

// host[:port] of an origin, for showing to the user
TempStr BrowserLoginHostTemp(Str origin);

// the message the capture script posts; parses into user/password (temp), or
// false if msg is not one
bool BrowserLoginParseSaveMsg(Str msg, TempStr* user, TempStr* password);

Str BrowserLoginCaptureScript();
TempStr BrowserLoginFillScriptTemp(Str origin, Str user, Str password);

// Credential Manager storage. A test run (-for-testing) uses its own name
// prefix, so it never sees or changes the user's real saved logins.
// the most recently saved login for origin; false if there is none. user and
// password are temp.
bool BrowserLoginFind(Str origin, TempStr* user, TempStr* password);
// true if origin already has exactly this user + password
bool BrowserLoginIsSaved(Str origin, Str user, Str password);
// true if origin has a saved login for this user (with any password)
bool BrowserLoginHasUser(Str origin, Str user);
bool BrowserLoginSave(Str origin, Str user, Str password);
// removes every saved login for origin; returns how many
int BrowserLoginForget(Str origin);

// "Never for this site": the browser stops offering to save logins for origin.
// Kept in Credential Manager too (no secret in it, just the marker), so it
// lives and dies with the user's logins rather than in the settings file.
bool BrowserLoginSetNever(Str origin);
bool BrowserLoginIsNever(Str origin);
// true if origin was marked "never"
bool BrowserLoginClearNever(Str origin);
