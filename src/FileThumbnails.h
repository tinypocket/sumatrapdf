/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// thumbnails are 150px high and have a ratio of sqrt(2) : 1
// Portrait cards, so one shows a whole page (kHomeCardDx / kHomeCardDy in
// TouchMetrics.h). These are the card size in *unscaled* px: the home grid
// DpiScales them, like every other layout value.
constexpr int kThumbnailDx = 168;
constexpr int kThumbnailDy = 220;

// The size thumbnails are rendered and cached at. Deliberately independent of
// display DPI: the on-disk cache is keyed by file only, so a DPI-dependent
// render size would make a second monitor at a different scaling regenerate
// the whole history on every move. 2x covers displays up to 200% sharply.
constexpr int kThumbnailRenderDx = kThumbnailDx * 2;
constexpr int kThumbnailRenderDy = kThumbnailDy * 2;

RenderedBitmap* LoadThumbnail(FileState* fs);
bool HasThumbnail(FileState* fs);
void SetThumbnail(FileState* fs, RenderedBitmap* bmp);
void SaveThumbnail(FileState* fs);
RenderedBitmap* LoadThumbnailForFile(Str filePath);
void SaveThumbnailForFile(Str filePath, RenderedBitmap* thumbnail);
void RemoveThumbnail(FileState* fs);

TempStr GetThumbnailCacheDirTemp();
TempStr GetThumbnailPathTemp(Str filePath);
void DeleteThumbnailForFile(Str path);
void EmptyThumbnailCacheDirectory();
