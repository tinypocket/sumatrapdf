/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/GuessFileType.h"

#include "BrowserUrlUtil.h"

// Engine-free URL parsing shared by the in-product browser (SimpleBrowserWindow.cpp)
// and its unit tests. Keeping it here means the production doc-detection and the
// tests exercise the exact same parsing (no duplicated logic).

// Inspect a URL's LAST path segment and, if it looks like a filename with an
// extension, report the guessed FileType and the extension (e.g. ".pdf").
// The ?query and #fragment are stripped first, then everything up to and
// including the last '/' is dropped, so host dots (www.google.com) and
// extension-less page paths (/wiki/PDF) never look like a filename. Only the
// last segment is examined - this is the check whose absence made the browser
// treat ordinary web pages as downloadable documents and cancel navigation.
// Returns false when the last segment has no '.' at all. When it returns true
// the caller still decides doc-ness via IsSupportedFileType(*ftOut): a bare host
// like www.google.com yields FileType::Unknown and is not a document.
bool TouchBrowserUrlFileType(Str url, FileType* ftOut, Str* extOut) {
    if (!url) {
        return false;
    }
    Str s = url;
    int cut = str::IndexOfChar(s, '?');
    if (cut >= 0) {
        s = Str(s.s, cut);
    }
    cut = str::IndexOfChar(s, '#');
    if (cut >= 0) {
        s = Str(s.s, cut);
    }
    int slash = str::LastIndexOfChar(s, '/');
    Str seg = (slash >= 0) ? Str(s.s + slash + 1, s.len - slash - 1) : s;
    int dot = str::LastIndexOfChar(seg, '.');
    if (dot < 0) {
        return false; // no filename extension -> an ordinary page, not a document
    }
    if (ftOut) {
        *ftOut = GuessFileTypeFromName(seg);
    }
    if (extOut) {
        *extOut = str::DupTemp(Str(seg.s + dot, seg.len - dot));
    }
    return true;
}

// Should a link to this file type be pulled out of the browser and opened as a
// document? Only formats a PDF reader is clearly the right home for.
//
// Deliberately EXCLUDES types SumatraPDF can open but that are ordinary web
// content: HTML/Markdown/Txt/Svg and every image format - the engine opens
// .html natively, so without this an ordinary link to a page was downloaded and
// opened as a document tab instead of being browsed. Bare archives (zip/rar/7z/
// tar) are excluded too; only their comic-book variants are documents.
bool TouchBrowserFileTypeIsDownloadableDoc(FileType ft) {
    switch (ft) {
        case FileType::PDF:
        case FileType::PS:
        case FileType::Xps:
        case FileType::DjVu:
        case FileType::Chm:
        case FileType::Cbz:
        case FileType::Cbr:
        case FileType::Cb7:
        case FileType::Cbt:
        case FileType::Fb2:
        case FileType::Fb2z:
        case FileType::Epub:
        case FileType::Mobi:
        case FileType::PalmDoc:
            return true;
        default:
            return false;
    }
}

// Short host label for a bookmark chip: strip the scheme, then the path
// (everything from the first '/'), then a leading "www." so chips stay short.
// E.g. "https://www.google.com/search" -> "google.com".
TempStr TbChipLabel(Str url) {
    Str s = url;
    if (str::StartsWithI(s, StrL("https://"))) {
        s = Str(s.s + 8, s.len - 8);
    } else if (str::StartsWithI(s, StrL("http://"))) {
        s = Str(s.s + 7, s.len - 7);
    }
    int slash = str::IndexOfChar(s, '/');
    if (slash >= 0) {
        s = Str(s.s, slash);
    }
    if (str::StartsWithI(s, StrL("www."))) {
        s = Str(s.s + 4, s.len - 4);
    }
    return str::DupTemp(s);
}
