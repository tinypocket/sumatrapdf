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
}
