/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

enum class FileType : u8;

bool TouchBrowserUrlFileType(Str url, FileType* ftOut, Str* extOut);
bool TouchBrowserFileTypeIsDownloadableDoc(FileType ft);
FileType TouchBrowserFileTypeFromContentType(Str contentType);
TempStr TouchBrowserExtForFileType(FileType ft);
TempStr TbChipLabel(Str url);
// guessedHost: set when the result is an address made up from typed text
// with no scheme ("example.com" -> https://example.com), so a host that turns
// out not to exist can fall back to searching for the text
TempStr TouchBrowserAddressToUrlTemp(Str address, bool* guessedHost = nullptr);
TempStr TouchBrowserSearchUrlTemp(Str text);
