/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/GuessFileType.h"

#include "BrowserUrlUtil.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// TouchBrowserUrlIsDoc (SimpleBrowserWindow.cpp) returns true only when a URL's
// last segment names a *supported* document. It layers IsSupportedFileType(ft)
// on top of TouchBrowserUrlFileType; IsSupportedFileType pulls in the whole
// engine layer and can't link into test_util, so here we assert the two
// engine-free facts the production code derives from TouchBrowserUrlFileType:
//   - no filename extension in the last segment => not a document, or
//   - the guessed FileType is Unknown (e.g. a bare host www.google.com whose
//     ".com" is not a real extension) => IsSupportedFileType(Unknown) is false.
// Both guarantee TouchBrowserUrlIsDoc returns false, which is the regression:
// ordinary web pages must not be treated as downloadable documents.
static void expectNotDoc(Str url) {
    FileType ft = FileType::PDF; // sentinel: must be overwritten or left irrelevant
    Str ext;
    bool hasExt = TouchBrowserUrlFileType(url, &ft, &ext);
    utassert(!hasExt || ft == FileType::Unknown);
}

// last segment names a supported document type: helper reports the extension and
// a FileType for which IsSupportedFileType(ft, true) is true (PDF, Epub, ...).
static void expectDoc(Str url, FileType expFt, Str expExt) {
    FileType ft = FileType::Unknown;
    Str ext;
    bool hasExt = TouchBrowserUrlFileType(url, &ft, &ext);
    utassert(hasExt);
    utassert(ft == expFt);
    utassert(str::Eq(ext, expExt));
}

static void expectChip(Str url, Str exp) {
    TempStr got = TbChipLabel(url);
    utassert(str::Eq(got, exp));
}

static void expectAddress(Str address, Str exp) {
    TempStr got = TouchBrowserAddressToUrlTemp(address);
    utassert(str::Eq(got, exp));
}

void BrowserUrl_UnitTests() {
    // regression: ordinary web pages must NOT be classified as documents
    expectNotDoc(StrL("https://www.google.com"));            // bare host -> ft Unknown
    expectNotDoc(StrL("https://en.wikipedia.org/wiki/PDF")); // last seg "PDF" has no dot
    expectNotDoc(StrL("https://example.com/path/page"));     // last seg "page" has no dot
    expectNotDoc(StrL("https://site.com/"));                 // empty last segment
    expectNotDoc(StrL("https://foo.com"));                   // bare host -> ft Unknown

    // URLs whose last segment names a supported document
    expectDoc(StrL("https://site.com/file.pdf"), FileType::PDF, StrL(".pdf"));
    expectDoc(StrL("https://a.com/b/doc.epub"), FileType::Epub, StrL(".epub"));
    expectDoc(StrL("https://s.com/x.pdf?download=1"), FileType::PDF, StrL(".pdf")); // ?query stripped
    expectDoc(StrL("https://s.com/x.pdf#page=2"), FileType::PDF, StrL(".pdf"));     // #fragment stripped

    // bookmark chip labels: scheme + path + leading "www." stripped
    expectChip(StrL("https://www.google.com/search"), StrL("google.com"));
    expectChip(StrL("http://example.org"), StrL("example.org"));
    expectChip(StrL("https://docs.example.com/a/b"), StrL("docs.example.com"));

    // Address-field entries: recognizable URLs navigate directly, while plain
    // words and phrases become URL-encoded Google searches.
    expectAddress(StrL("https://example.com/a"), StrL("https://example.com/a"));
    expectAddress(StrL("example.com/a"), StrL("https://example.com/a"));
    expectAddress(StrL("melodos.com"), StrL("https://melodos.com"));
    expectAddress(StrL("www.melodos.com"), StrL("https://www.melodos.com"));
    expectAddress(StrL("docs.example.co.uk/x?y=1"), StrL("https://docs.example.co.uk/x?y=1"));
    expectAddress(StrL("example.com:8443"), StrL("https://example.com:8443"));
    // local dev servers speak http
    expectAddress(StrL("localhost:8080/test"), StrL("http://localhost:8080/test"));
    expectAddress(StrL("localhost"), StrL("http://localhost"));
    expectAddress(StrL("127.0.0.1:8080"), StrL("http://127.0.0.1:8080"));
    expectAddress(StrL("192.168.1.10"), StrL("http://192.168.1.10"));
    expectAddress(StrL("[::1]:3000"), StrL("http://[::1]:3000"));
    // dotted things that are not sites
    expectAddress(StrL("3.14"), StrL("https://www.google.com/search?q=3.14"));
    expectAddress(StrL("e.g."), StrL("https://www.google.com/search?q=e.g."));
    expectAddress(StrL(".bashrc"), StrL("https://www.google.com/search?q=.bashrc"));
    expectAddress(StrL("1.2.3"), StrL("https://www.google.com/search?q=1.2.3"));
    expectAddress(StrL("about:blank"), StrL("about:blank"));
    expectAddress(StrL("weather"), StrL("https://www.google.com/search?q=weather"));
    expectAddress(StrL("  best PDF reader  "), StrL("https://www.google.com/search?q=best%20PDF%20reader"));
    expectAddress(StrL("sumatrapdf + touch"), StrL("https://www.google.com/search?q=sumatrapdf%20%2B%20touch"));
    expectAddress(StrL("caf\xC3\xA9"), StrL("https://www.google.com/search?q=caf%C3%A9"));
    // only a made-up https:// address can fall back to a search
    bool guessed = false;
    TouchBrowserAddressToUrlTemp(StrL("melodos.com"), &guessed);
    utassert(guessed);
    TouchBrowserAddressToUrlTemp(StrL("https://melodos.com"), &guessed);
    utassert(!guessed);
    TouchBrowserAddressToUrlTemp(StrL("localhost:3000"), &guessed);
    utassert(!guessed);
    TouchBrowserAddressToUrlTemp(StrL("weather"), &guessed);
    utassert(!guessed);

    // Which file types the browser pulls out and opens as a document.
    // Regression: the engine also opens .html/.txt/images, so using
    // IsSupportedFileType here hijacked ordinary web links - a link to
    // index.html was downloaded and opened as a document tab.
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::HTML));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Txt));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Markdown));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Svg));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Png));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Jpeg));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Webp));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Zip));
    utassert(!TouchBrowserFileTypeIsDownloadableDoc(FileType::Unknown));
    // real documents still are
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::PDF));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::Epub));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::Mobi));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::DjVu));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::Cbz));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::Xps));
    utassert(TouchBrowserFileTypeIsDownloadableDoc(FileType::Chm));

    // Content-Type based detection: the fallback for URLs with no ".pdf" in
    // them (query-driven downloads, extension-less routes, redirects), which is
    // what made a PDF link render inside Edge's built-in viewer instead of
    // opening as a SumatraPDF tab.
    utassert(TouchBrowserFileTypeFromContentType(StrL("application/pdf")) == FileType::PDF);
    utassert(TouchBrowserFileTypeFromContentType(StrL("APPLICATION/PDF")) == FileType::PDF);
    utassert(TouchBrowserFileTypeFromContentType(StrL("application/pdf; charset=binary")) == FileType::PDF);
    utassert(TouchBrowserFileTypeFromContentType(StrL("  application/pdf  ")) == FileType::PDF);
    utassert(TouchBrowserFileTypeFromContentType(StrL("application/epub+zip")) == FileType::Epub);
    utassert(TouchBrowserFileTypeFromContentType(StrL("image/vnd.djvu")) == FileType::DjVu);
    // ordinary web content must never be taken over
    utassert(TouchBrowserFileTypeFromContentType(StrL("text/html")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(StrL("text/html; charset=utf-8")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(StrL("image/png")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(StrL("text/plain")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(StrL("application/json")) == FileType::Unknown);
    // octet-stream says nothing: servers use it for everything
    utassert(TouchBrowserFileTypeFromContentType(StrL("application/octet-stream")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(StrL("")) == FileType::Unknown);
    utassert(TouchBrowserFileTypeFromContentType(Str()) == FileType::Unknown);
    // every mapped type is one the browser is allowed to pull out, and has an
    // extension to save it under (its URL has none, or we would not be here)
    utassert(TouchBrowserFileTypeIsDownloadableDoc(TouchBrowserFileTypeFromContentType(StrL("application/pdf"))));
    utassert(str::Eq(TouchBrowserExtForFileType(FileType::PDF), StrL(".pdf")));
    utassert(str::Eq(TouchBrowserExtForFileType(FileType::Epub), StrL(".epub")));
}
