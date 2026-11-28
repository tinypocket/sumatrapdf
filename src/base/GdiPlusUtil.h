/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct RenderedBitmap;
struct Pixmap;

Gdiplus::RectF RectToRectF(Gdiplus::Rect r);

Gdiplus::Bitmap* NewGdiplusBitmapFromPixmap(Pixmap* px);
Gdiplus::Bitmap* WrapPixmapGdiplus(const Pixmap* px);
Pixmap* PixmapFromGdiplus(Gdiplus::Bitmap* bmp);
Pixmap* PixmapApplyExifOrientation(Pixmap* px, int orientation);

typedef RectF (*TextMeasureAlgorithm)(Gdiplus::Graphics* g, Gdiplus::Font* f, WStr s);

RectF MeasureTextAccurate(Gdiplus::Graphics* g, Gdiplus::Font* f, WStr s);
RectF MeasureTextStandard(Gdiplus::Graphics* g, Gdiplus::Font* f, WStr s);
RectF MeasureTextQuick(Gdiplus::Graphics* g, Gdiplus::Font* f, WStr s);
RectF MeasureText(Gdiplus::Graphics* g, Gdiplus::Font* f, WStr s, TextMeasureAlgorithm algo = nullptr);
// float     GetSpaceDx(Graphics *g, Font *f, TextMeasureAlgorithm algo=nullptr);
// int   StringLenForWidth(Graphics *g, Font *f, const WCHAR *s, size_t len, float dx, TextMeasureAlgorithm
// algo=nullptr);

void GetBaseTransform(Gdiplus::Matrix& m, Gdiplus::RectF pageRect, float zoom, int rotation);

void ApplyExifOrientation(Gdiplus::Bitmap* bmp, int exifOrientation);
CLSID GetGdiPlusEncoderClsid(WStr format);

// Anti-aliased stand-ins for GDI's RoundRect and Ellipse, which stair-step every
// curve they draw. `radius` is in pixels and is clamped to half the shorter
// side, so half the height makes a pill and half a square's side a circle.
// Straight edges stay on pixel boundaries, so a card's sides are as crisp as a
// FillRect's. Shapes cover exactly the pixels of `r`, like RoundRect.
void AddRoundRectPath(Gdiplus::GraphicsPath& path, const Rect& r, int radius);
// a `border` borderDx pixels wide inside the shape, kColorUnset for none
void FillRoundRectAA(Gdiplus::Graphics& gfx, const Rect& r, int radius, COLORREF fill, COLORREF border = kColorUnset,
                     int borderDx = 1);
void FillRoundRectAA(HDC hdc, const Rect& r, int radius, COLORREF fill, COLORREF border = kColorUnset,
                     int borderDx = 1);
// an outline `width` pixels wide, kept inside `r` (as a PS_INSIDEFRAME pen is),
// so a one-pixel outline is one pixel rather than a smear across two
void StrokeRoundRectAA(HDC hdc, const Rect& r, int radius, COLORREF col, int width);
void FillEllipseAA(HDC hdc, const Rect& r, COLORREF fill);
// connected line segments with round caps and joins: chevrons, checkmarks
void DrawPolylineAA(HDC hdc, const Point* pts, int nPts, COLORREF col, int width);
// sets the anti-aliased, pixel-aligned mode the functions above draw in
void SetSmoothPixelAligned(Gdiplus::Graphics& gfx);
