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

// --- content-type based detection ------------------------------------------
// The URL is only a guess: a link like /download?id=42 or an extension-less
// route serves a PDF with no ".pdf" anywhere in it, and WebView2 then renders it
// in Edge's built-in PDF viewer instead of letting SumatraPDF open it. The HTTP
// response's Content-Type is the authoritative answer, so the browser also
// checks the main document's response header (SimpleBrowserWindow.cpp) and maps
// it here.
//
// Only MIME types whose file type TouchBrowserFileTypeIsDownloadableDoc()
// accepts are listed: text/html, images and friends must keep browsing
// normally. Parameters after ';' (charset=...) are ignored, and matching is
// case-insensitive because header values are.

struct TouchBrowserMimeMap {
    const char* mime;
    FileType ft;
    const char* ext;
};

// application/octet-stream is deliberately absent: servers use it for
// everything, so it says nothing about the content.
static const TouchBrowserMimeMap gTouchBrowserMimes[] = {
    {"application/pdf", FileType::PDF, ".pdf"},
    {"application/x-pdf", FileType::PDF, ".pdf"},
    {"application/acrobat", FileType::PDF, ".pdf"},
    {"application/vnd.pdf", FileType::PDF, ".pdf"},
    {"text/pdf", FileType::PDF, ".pdf"},
    {"application/postscript", FileType::PS, ".ps"},
    {"application/vnd.ms-xpsdocument", FileType::Xps, ".xps"},
    {"application/oxps", FileType::Xps, ".xps"},
    {"image/vnd.djvu", FileType::DjVu, ".djvu"},
    {"image/x-djvu", FileType::DjVu, ".djvu"},
    {"application/vnd.ms-htmlhelp", FileType::Chm, ".chm"},
    {"application/x-chm", FileType::Chm, ".chm"},
    {"application/epub+zip", FileType::Epub, ".epub"},
    {"application/x-mobipocket-ebook", FileType::Mobi, ".mobi"},
    {"application/x-fictionbook+xml", FileType::Fb2, ".fb2"},
    {"application/x-cbz", FileType::Cbz, ".cbz"},
    {"application/vnd.comicbook+zip", FileType::Cbz, ".cbz"},
    {"application/x-cbr", FileType::Cbr, ".cbr"},
    {"application/vnd.comicbook-rar", FileType::Cbr, ".cbr"},
};

// "application/pdf; charset=binary" -> FileType::PDF. FileType::Unknown when the
// type is not one the reader should take over (including an empty header).
FileType TouchBrowserFileTypeFromContentType(Str contentType) {
    if (!contentType) {
        return FileType::Unknown;
    }
    Str s = contentType;
    int semi = str::IndexOfChar(s, ';');
    if (semi >= 0) {
        s = Str(s.s, semi);
    }
    // header values may be padded; trim both ends
    while (s.len > 0 && str::IsWs(s.s[0])) {
        s = Str(s.s + 1, s.len - 1);
    }
    while (s.len > 0 && str::IsWs(s.s[s.len - 1])) {
        s = Str(s.s, s.len - 1);
    }
    if (s.len == 0) {
        return FileType::Unknown;
    }
    for (const TouchBrowserMimeMap& m : gTouchBrowserMimes) {
        if (str::EqI(s, Str(m.mime))) {
            return m.ft;
        }
    }
    return FileType::Unknown;
}

// the extension to save a content-type-detected document under, since its URL
// has none (that is the whole reason we fell back to the header)
TempStr TouchBrowserExtForFileType(FileType ft) {
    for (const TouchBrowserMimeMap& m : gTouchBrowserMimes) {
        if (m.ft == ft) {
            return str::DupTemp(Str(m.ext));
        }
    }
    return str::DupTemp(StrL(".dat"));
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

static TempStr EncodeSearchQueryTemp(Str query) {
    static constexpr char hex[] = "0123456789ABCDEF";
    str::Builder encoded(len(query) * 3);
    for (int i = 0; i < len(query); i++) {
        u8 c = (u8)query.s[i];
        bool unreserved = str::IsAlNum((char)c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            encoded.AppendChar((char)c);
        } else {
            encoded.AppendChar('%');
            encoded.AppendChar(hex[c >> 4]);
            encoded.AppendChar(hex[c & 0x0f]);
        }
    }
    return ToStrTemp(encoded);
}

TempStr TouchBrowserSearchUrlTemp(Str text) {
    return str::JoinTemp(StrL("https://www.google.com/search?q="), EncodeSearchQueryTemp(text));
}

static bool IsAllDigits(Str s) {
    if (len(s) == 0) {
        return false;
    }
    for (int i = 0; i < len(s); i++) {
        if (s.s[i] < '0' || s.s[i] > '9') {
            return false;
        }
    }
    return true;
}

// host without its ":port"
static Str HostNameOnly(Str host) {
    int colon = str::LastIndexOfChar(host, ':');
    return colon >= 0 ? Str(host.s, colon) : host;
}

// "192.168.1.10": four all-digit labels
static bool IsIpv4(Str name) {
    int labels = 0;
    int start = 0;
    for (int i = 0; i <= len(name); i++) {
        if (i == len(name) || name.s[i] == '.') {
            if (!IsAllDigits(Str(name.s + start, i - start))) {
                return false;
            }
            labels++;
            start = i + 1;
        }
    }
    return labels == 4;
}

// "example.com", "docs.example.co.uk": dotted, no empty label, and the last
// label (the top-level domain) has a letter in it. Rules out "3.14", "e.g."
// and ".bashrc", which are searches.
static bool LooksLikeDomain(Str name) {
    if (!str::ContainsChar(name, '.') || name.s[0] == '.' || name.s[len(name) - 1] == '.') {
        return false;
    }
    if (str::Contains(name, StrL(".."))) {
        return false;
    }
    int dot = str::LastIndexOfChar(name, '.');
    Str tld(name.s + dot + 1, len(name) - dot - 1);
    for (int i = 0; i < len(tld); i++) {
        char c = tld.s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            return true;
        }
    }
    return false;
}

// Turns what the user typed in the browser address field into a navigable URL.
// Explicit schemes and recognizable host names are addresses; everything else
// is a Google query. This mirrors the useful part of a normal browser omnibox
// without treating a single word such as "weather" as an HTTPS host name.
// Local hosts (localhost, IP addresses) get http://, which is what a local dev
// server speaks; real sites get https://.
TempStr TouchBrowserAddressToUrlTemp(Str address, bool* guessedHost) {
    if (guessedHost) {
        *guessedHost = false;
    }
    Str s = address;
    while (len(s) > 0 && str::IsWs(s.s[0])) {
        s = Str(s.s + 1, len(s) - 1);
    }
    while (len(s) > 0 && str::IsWs(s.s[len(s) - 1])) {
        s = Str(s.s, len(s) - 1);
    }
    if (!s) {
        return {};
    }

    if (str::Contains(s, StrL("://")) || str::StartsWithI(s, StrL("about:")) || str::StartsWithI(s, StrL("mailto:"))) {
        return str::DupTemp(s);
    }

    bool hasWhitespace = false;
    for (int i = 0; i < len(s); i++) {
        if (str::IsWs(s.s[i])) {
            hasWhitespace = true;
            break;
        }
    }

    Str host = s;
    int hostEnd = len(host);
    for (int i = 0; i < 3; i++) {
        int idx = str::IndexOfChar(host, "/?#"[i]);
        if (idx >= 0 && idx < hostEnd) {
            hostEnd = idx;
        }
    }
    host = Str(host.s, hostEnd);

    bool isLocalhost = str::EqI(host, StrL("localhost")) || str::StartsWithI(host, StrL("localhost:"));
    bool isIpv6 = len(host) >= 2 && host.s[0] == '[' && str::ContainsChar(host, ']');
    Str name = isIpv6 ? host : HostNameOnly(host);
    bool isIpv4 = IsIpv4(name);
    if (!hasWhitespace && (isLocalhost || isIpv6 || isIpv4)) {
        return str::JoinTemp(StrL("http://"), s);
    }
    if (!hasWhitespace && LooksLikeDomain(name)) {
        if (guessedHost) {
            *guessedHost = true;
        }
        return str::JoinTemp(StrL("https://"), s);
    }

    return TouchBrowserSearchUrlTemp(s);
}
