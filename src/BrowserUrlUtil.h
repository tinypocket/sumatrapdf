/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

enum class FileType : u8;

bool TouchBrowserUrlFileType(Str url, FileType* ftOut, Str* extOut);
bool TouchBrowserFileTypeIsDownloadableDoc(FileType ft);
TempStr TbChipLabel(Str url);
