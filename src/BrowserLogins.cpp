/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/JsonParser.h"

#include <wincred.h>

#include "SumatraConfig.h"
#include "BrowserLogins.h"

// --- origins -----------------------------------------------------------------

TempStr BrowserLoginOriginTemp(Str url) {
    Str scheme;
    if (str::StartsWithI(url, StrL("https://"))) {
        scheme = StrL("https://");
    } else if (str::StartsWithI(url, StrL("http://"))) {
        scheme = StrL("http://");
    } else {
        return {};
    }
    int start = len(scheme);
    int end = start;
    while (end < len(url)) {
        char c = url.s[end];
        if (c == '/' || c == '?' || c == '#' || c == '\\') {
            break;
        }
        end++;
    }
    Str auth(url.s + start, end - start);
    // "user:pass@host" belongs to the url, not the site
    int at = str::LastIndexOfChar(auth, '@');
    if (at >= 0) {
        auth = Str(auth.s + at + 1, len(auth) - at - 1);
    }
    if (len(auth) == 0) {
        return {};
    }
    TempStr res = str::JoinTemp(scheme, auth);
    str::ToLowerInPlace(res);
    // the default port names the same origin as no port
    Str defPort = str::Eq(scheme, StrL("https://")) ? StrL(":443") : StrL(":80");
    if (str::EndsWith(res, defPort)) {
        res = Str(res.s, len(res) - len(defPort));
        res = str::DupTemp(res);
    }
    if (len(res) == len(scheme)) {
        return {};
    }
    return res;
}

TempStr BrowserLoginHostTemp(Str origin) {
    int idx = str::IndexOfAfter(origin, StrL("://"));
    if (idx <= 0) {
        return str::DupTemp(origin);
    }
    return str::DupTemp(Str(origin.s + idx, len(origin) - idx));
}

// --- the save message --------------------------------------------------------

namespace {
struct SaveMsgVisitor : json::ValueVisitor {
    bool isSave = false;
    TempStr user;
    TempStr password;
    bool Visit(Str path, Str value, json::Type type) override {
        if (type != json::Type::String) {
            return true;
        }
        if (str::Eq(path, StrL("/sumatraLogin"))) {
            isSave = str::Eq(value, StrL("save"));
        } else if (str::Eq(path, StrL("/user"))) {
            user = str::DupTemp(value);
        } else if (str::Eq(path, StrL("/password"))) {
            password = str::DupTemp(value);
        }
        return true;
    }
};
} // namespace

bool BrowserLoginParseSaveMsg(Str msg, TempStr* user, TempStr* password) {
    if (!str::StartsWith(msg, StrL("{\"sumatraLogin\""))) {
        return false;
    }
    SaveMsgVisitor v;
    if (!json::Parse(msg, &v) || !v.isSave || len(v.password) == 0) {
        return false;
    }
    *user = v.user ? v.user : str::DupTemp(StrL(""));
    *password = v.password;
    return true;
}

// --- page scripts ------------------------------------------------------------

// Shared by both scripts: which inputs count as a username field.
#define kLoginFieldHelpersJs                                                                                        \
    "function visible(e){return !!(e.offsetWidth||e.offsetHeight||e.getClientRects().length);}"                     \
    "function isUserField(e){if(e.tagName!=='INPUT')return false;var t=(e.type||'text').toLowerCase();"             \
    "return t==='text'||t==='email'||t==='tel';}"                                                                   \
    "function looksLikeUser(e){var ac=(e.getAttribute('autocomplete')||'').toLowerCase();"                          \
    "if(ac.indexOf('username')>=0||ac==='email')return true;if((e.type||'').toLowerCase()==='email')return "        \
    "true;var n=((e.name||'')+' '+(e.id||'')).toLowerCase();return /user|login|email|account|identifier/.test(n);}" \
    "function isNewPw(e){return (e.getAttribute('autocomplete')||'').toLowerCase().indexOf('new-password')>=0;}"

// Runs in every top-level document. When the user signs in (a form submit, a
// click on a button, or Enter in a field while a password field has a value)
// it posts the username and password to the app, which offers to save them.
// A username-first step (no password field yet) only remembers the username
// for this tab's session, for the password step that follows.
// clang-format off
static const char* kCaptureJs =
    "(function(){"
    "if(window.top!==window||!window.chrome||!window.chrome.webview||window.__sumatraLogins)return;"
    "window.__sumatraLogins=1;"
    "var KEY='__sumatra_login_user',lastSent='';"
    kLoginFieldHelpersJs
    "function findUser(pw){var ins=(pw.form||document).querySelectorAll('input'),best=null,like=null;"
    "for(var i=0;i<ins.length;i++){var e=ins[i];if(e===pw)break;"
    "if(!isUserField(e)||!e.value||!visible(e))continue;best=e;if(looksLikeUser(e))like=e;}"
    "return like||best;}"
    "function pickPw(root){var a=[].slice.call(root.querySelectorAll('input[type=password]'))"
    ".filter(function(e){return e.value;});if(!a.length)return null;"
    "var n=a.filter(isNewPw);return n.length?n[0]:a[0];}"
    "function rememberUser(root){var ins=root.querySelectorAll('input');"
    "for(var i=0;i<ins.length;i++){var e=ins[i];"
    "if(isUserField(e)&&e.value&&looksLikeUser(e)&&visible(e)){"
    "try{sessionStorage.setItem(KEY,e.value);}catch(x){}return;}}}"
    "function check(root){if(!root||!root.querySelectorAll)root=document;"
    "var pw=pickPw(root);if(!pw&&root!==document)pw=pickPw(document);"
    "if(!pw){rememberUser(root);return;}"
    "var u=findUser(pw),user=u?u.value:'';"
    "if(!user){try{user=sessionStorage.getItem(KEY)||'';}catch(x){}}"
    "var key=user+'\\n'+pw.value;if(key===lastSent)return;lastSent=key;"
    "window.chrome.webview.postMessage(JSON.stringify({sumatraLogin:'save',user:user,password:pw.value}));}"
    "document.addEventListener('submit',function(ev){check(ev.target);},true);"
    "document.addEventListener('click',function(ev){var t=ev.target;"
    "var b=t&&t.closest&&t.closest('button,input[type=submit],input[type=button],input[type=image],[role=button]');"
    "if(b)check(b.form||document);},true);"
    "document.addEventListener('keydown',function(ev){var t=ev.target;"
    "if(ev.key==='Enter'&&t&&t.tagName==='INPUT')check(t.form||document);},true);"
    "})();";

// Fills a saved login into the page: the first visible password field that is
// not a "new password" one, and the username field before it. Only empty
// fields are filled, and a form where a different username is already typed
// is left alone. A username-first page (autocomplete="username", no password
// field yet) gets the username. Forms drawn late by script are caught by
// watching the page for a few seconds.
static const char* kFillJsBody =
    "if(window.top!==window||location.origin!==o)return;"
    kLoginFieldHelpersJs
    "function usable(e){return visible(e)&&!e.disabled&&!e.readOnly;}"
    "function setVal(e,v){var d=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value');"
    "d.set.call(e,v);e.dispatchEvent(new Event('input',{bubbles:true}));"
    "e.dispatchEvent(new Event('change',{bubbles:true}));}"
    "function fill(){var pws=document.querySelectorAll('input[type=password]'),pw=null;"
    "for(var i=0;i<pws.length;i++){if(usable(pws[i])&&!isNewPw(pws[i])){pw=pws[i];break;}}"
    "if(pw){var ins=(pw.form||document).querySelectorAll('input'),uf=null,like=null;"
    "for(var j=0;j<ins.length;j++){var e=ins[j];if(e===pw)break;"
    "if(isUserField(e)&&usable(e)){uf=e;if(looksLikeUser(e))like=e;}}"
    "uf=like||uf;if(uf&&uf.value&&uf.value!==u)return true;"
    "if(uf&&!uf.value&&u)setVal(uf,u);if(!pw.value)setVal(pw,p);return true;}"
    "var un=document.querySelector('input[autocomplete~=\"username\"]');"
    "if(un&&usable(un)&&!un.value&&u)setVal(un,u);return false;}"
    "if(fill())return;var pend=0,ob=new MutationObserver(function(){if(pend)return;"
    "pend=setTimeout(function(){pend=0;if(fill()){ob.disconnect();}},150);});"
    "ob.observe(document.documentElement,{childList:true,subtree:true,attributes:true,"
    "attributeFilter:['type','style','class','hidden']});"
    "setTimeout(function(){ob.disconnect();},15000);";
// clang-format on

Str BrowserLoginCaptureScript() {
    return Str(kCaptureJs);
}

TempStr BrowserLoginFillScriptTemp(Str origin, Str user, Str password) {
    return fmt("(function(o,u,p){%s})(\"%s\",\"%s\",\"%s\");", Str(kFillJsBody), json::EscapeStrTemp(origin),
               json::EscapeStrTemp(user), json::EscapeStrTemp(password));
}

// --- Credential Manager ------------------------------------------------------

// Each login is one generic credential named "<prefix><origin>/<user>". An
// origin never contains '/', so "<prefix><origin>/*" lists exactly that site's
// logins and not those of a longer host that starts the same way.
static Str LoginPrefix() {
    return gForTesting ? StrL("SumatraPDF+test-login:") : StrL("SumatraPDF+login:");
}

static TempWStr LoginTargetTemp(Str origin, Str user) {
    return ToWStrTemp(str::JoinTemp(LoginPrefix(), origin, StrL("/"), user));
}

static TempWStr LoginFilterTemp(Str origin) {
    return ToWStrTemp(str::JoinTemp(LoginPrefix(), origin, StrL("/*")));
}

bool BrowserLoginFind(Str origin, TempStr* user, TempStr* password) {
    if (len(origin) == 0) {
        return false;
    }
    DWORD count = 0;
    PCREDENTIALW* creds = nullptr;
    if (!CredEnumerateW(LoginFilterTemp(origin).s, 0, &count, &creds)) {
        return false; // ERROR_NOT_FOUND: nothing saved for this site
    }
    PCREDENTIALW best = nullptr;
    for (DWORD i = 0; i < count; i++) {
        PCREDENTIALW c = creds[i];
        if (c->Type != CRED_TYPE_GENERIC || c->CredentialBlobSize == 0) {
            continue;
        }
        if (!best || CompareFileTime(&c->LastWritten, &best->LastWritten) > 0) {
            best = c;
        }
    }
    bool found = false;
    if (best) {
        *user = best->UserName ? ToUtf8Temp(best->UserName) : str::DupTemp(StrL(""));
        *password = str::DupTemp(Str((const char*)best->CredentialBlob, (int)best->CredentialBlobSize));
        found = true;
    }
    for (DWORD i = 0; i < count; i++) {
        if (creds[i]->CredentialBlob) {
            SecureZeroMemory(creds[i]->CredentialBlob, creds[i]->CredentialBlobSize);
        }
    }
    CredFree(creds);
    return found;
}

static PCREDENTIALW ReadLogin(Str origin, Str user) {
    PCREDENTIALW c = nullptr;
    if (!CredReadW(LoginTargetTemp(origin, user).s, CRED_TYPE_GENERIC, 0, &c)) {
        return nullptr;
    }
    return c;
}

static void FreeLogin(PCREDENTIALW c) {
    if (c->CredentialBlob) {
        SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    }
    CredFree(c);
}

bool BrowserLoginHasUser(Str origin, Str user) {
    PCREDENTIALW c = ReadLogin(origin, user);
    if (!c) {
        return false;
    }
    FreeLogin(c);
    return true;
}

bool BrowserLoginIsSaved(Str origin, Str user, Str password) {
    PCREDENTIALW c = ReadLogin(origin, user);
    if (!c) {
        return false;
    }
    bool same = (int)c->CredentialBlobSize == len(password) &&
                memcmp(c->CredentialBlob, password.s, (size_t)len(password)) == 0;
    FreeLogin(c);
    return same;
}

bool BrowserLoginSave(Str origin, Str user, Str password) {
    if (len(origin) == 0 || len(password) == 0 || len(password) > CRED_MAX_CREDENTIAL_BLOB_SIZE ||
        len(user) > CRED_MAX_USERNAME_LENGTH) {
        return false;
    }
    TempWStr target = LoginTargetTemp(origin, user);
    TempWStr wuser = ToWStrTemp(user);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.s;
    c.Comment = (WCHAR*)L"Saved by the SumatraPDF+ browser";
    c.CredentialBlobSize = (DWORD)len(password);
    c.CredentialBlob = (BYTE*)password.s;
    // this user on this PC only; not roamed to other machines
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    c.UserName = wuser.s;
    if (!CredWriteW(&c, 0)) {
        logf("BrowserLoginSave: CredWrite failed for '%s', error %d\n", origin, (int)GetLastError());
        return false;
    }
    return true;
}

// "never" markers are named "<never prefix><origin>": a prefix of their own,
// so listing a site's logins never returns one
static TempWStr NeverTargetTemp(Str origin) {
    Str prefix = gForTesting ? StrL("SumatraPDF+test-never:") : StrL("SumatraPDF+never:");
    return ToWStrTemp(str::JoinTemp(prefix, origin));
}

bool BrowserLoginSetNever(Str origin) {
    if (len(origin) == 0) {
        return false;
    }
    TempWStr target = NeverTargetTemp(origin);
    static BYTE marker[] = {'1'};
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.s;
    c.Comment = (WCHAR*)L"SumatraPDF+ browser: never offer to save passwords for this site";
    c.CredentialBlobSize = (DWORD)sizeof(marker);
    c.CredentialBlob = marker;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&c, 0)) {
        logf("BrowserLoginSetNever: CredWrite failed for '%s', error %d\n", origin, (int)GetLastError());
        return false;
    }
    return true;
}

bool BrowserLoginIsNever(Str origin) {
    if (len(origin) == 0) {
        return false;
    }
    PCREDENTIALW c = nullptr;
    if (!CredReadW(NeverTargetTemp(origin).s, CRED_TYPE_GENERIC, 0, &c)) {
        return false;
    }
    CredFree(c);
    return true;
}

bool BrowserLoginClearNever(Str origin) {
    if (len(origin) == 0) {
        return false;
    }
    return CredDeleteW(NeverTargetTemp(origin).s, CRED_TYPE_GENERIC, 0) != FALSE;
}

int BrowserLoginForget(Str origin) {
    if (len(origin) == 0) {
        return 0;
    }
    DWORD count = 0;
    PCREDENTIALW* creds = nullptr;
    if (!CredEnumerateW(LoginFilterTemp(origin).s, 0, &count, &creds)) {
        return 0;
    }
    int n = 0;
    for (DWORD i = 0; i < count; i++) {
        if (creds[i]->Type == CRED_TYPE_GENERIC && CredDeleteW(creds[i]->TargetName, CRED_TYPE_GENERIC, 0)) {
            n++;
        }
    }
    CredFree(creds);
    return n;
}
