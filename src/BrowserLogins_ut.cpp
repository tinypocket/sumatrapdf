/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "BrowserLogins.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// A saved login is filed under, and only ever filled into, its origin. These
// pin down what counts as the same site, and that the page's own message can't
// smuggle anything past the parser. The Credential Manager side is exercised by
// tests/tmp GUI runs, which use the -for-testing name prefix.

static void expectOrigin(const char* url, const char* want) {
    TempStr got = BrowserLoginOriginTemp(Str(url));
    if (!want) {
        utassert(!got);
        return;
    }
    utassert(str::Eq(got, Str(want)));
}

static void expectOrigins() {
    expectOrigin("https://example.com/login?next=/", "https://example.com");
    expectOrigin("https://Example.COM", "https://example.com");
    expectOrigin("HTTPS://example.com/", "https://example.com");
    expectOrigin("https://example.com:443/x", "https://example.com");
    expectOrigin("http://example.com:80/x", "http://example.com");
    expectOrigin("https://example.com:8443/x", "https://example.com:8443");
    expectOrigin("http://127.0.0.1:5000/login", "http://127.0.0.1:5000");
    expectOrigin("https://example.com?x=1", "https://example.com");
    expectOrigin("https://example.com#frag", "https://example.com");
    // user info belongs to the url, not the site
    expectOrigin("https://evil.com@example.com/", "https://example.com");
    expectOrigin("https://a:b@example.com/", "https://example.com");
    // a different scheme or host is a different site
    expectOrigin("http://example.com/", "http://example.com");
    expectOrigin("https://login.example.com/", "https://login.example.com");
    // nothing to tie a login to
    expectOrigin("about:blank", nullptr);
    expectOrigin("file:///C:/x.html", nullptr);
    expectOrigin("data:text/html,hi", nullptr);
    expectOrigin("https://", nullptr);
    expectOrigin("https:///path", nullptr);
    expectOrigin("", nullptr);
}

static void expectHost() {
    utassert(str::Eq(BrowserLoginHostTemp(StrL("https://example.com")), StrL("example.com")));
    utassert(str::Eq(BrowserLoginHostTemp(StrL("http://127.0.0.1:5000")), StrL("127.0.0.1:5000")));
}

static void expectSaveMsg() {
    TempStr u, p;
    utassert(BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"save","user":"bob","password":"pw1"})"), &u, &p));
    utassert(str::Eq(u, StrL("bob")));
    utassert(str::Eq(p, StrL("pw1")));

    // escapes come back unescaped
    // a variable: MSVC trips on a raw string with escapes inside the utassert macro
    Str escaped = StrL(R"({"sumatraLogin":"save","user":"a\"b","password":"x\\y"})");
    utassert(BrowserLoginParseSaveMsg(escaped, &u, &p));
    utassert(str::Eq(u, StrL("a\"b")));
    utassert(str::Eq(p, StrL("x\\y")));

    // no username is still a login
    utassert(BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"save","user":"","password":"pw"})"), &u, &p));
    utassert(len(u) == 0);

    // not ours, not a save, or nothing to save
    utassert(!BrowserLoginParseSaveMsg(StrL(R"({"other":1})"), &u, &p));
    utassert(!BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"fill","user":"a","password":"b"})"), &u, &p));
    utassert(!BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"save","user":"a","password":""})"), &u, &p));
    utassert(!BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"save","user":"a"})"), &u, &p));
    utassert(!BrowserLoginParseSaveMsg(StrL("hello"), &u, &p));
    utassert(!BrowserLoginParseSaveMsg(StrL(R"({"sumatraLogin":"save","user":)"), &u, &p));
}

static void expectFillScript() {
    // values are JSON-escaped, so a password can't end the string and run code
    TempStr js = BrowserLoginFillScriptTemp(StrL("https://example.com"), StrL("bob"), StrL("p\"w);alert(1)//"));
    utassert(str::Contains(js, StrL("\"p\\\"w);alert(1)//\"")));
    // and the script refuses to run anywhere but the login's own origin
    utassert(str::Contains(js, StrL("location.origin!==o")));
    utassert(str::Contains(js, StrL("\"https://example.com\"")));
}

void BrowserLogins_UnitTests() {
    expectOrigins();
    expectHost();
    expectSaveMsg();
    expectFillScript();
}
