/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Pixmap.h"

Pixmap* AllocPixmapDIB(int w, int h) {
    if (w <= 0 || h <= 0) {
        return nullptr;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        DeleteObject(hbmp);
        return nullptr;
    }
    Pixmap* p = new Pixmap();
    p->width = w;
    p->height = h;
    p->stride = w * 4;
    p->format = PixmapFormat::BGRA8;
    p->data = (u8*)bits;
    p->hbmp = hbmp;
    return p;
}

// Adopt an existing HBITMAP (and optional file mapping) into a Pixmap that owns them.
// If it's a DIB section, expose its pixels through data/stride/format; otherwise only
// carry the blittable handle.
Pixmap* PixmapFromHBITMAP(HBITMAP hbmp, Size size, HANDLE hMap) {
    if (!hbmp) {
        return nullptr;
    }
    Pixmap* p = new Pixmap();
    p->width = size.dx;
    p->height = size.dy;
    p->hbmp = hbmp;
    p->hMap = hMap;
    DIBSECTION ds{};
    if (GetObject(hbmp, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits) {
        p->stride = ds.dsBm.bmWidthBytes;
        p->data = (u8*)ds.dsBm.bmBits;
        p->format = (ds.dsBm.bmBitsPixel == 24) ? PixmapFormat::BGR8 : PixmapFormat::BGRA8;
        // Orientation follows the DIB. Pixel readers that care about orientation should
        // prefer the HBITMAP-based helpers, which handle it.
    }
    return p;
}

Pixmap* PixmapFromRenderedBitmap(RenderedBitmap* rb) {
    if (!rb) {
        return nullptr;
    }
    Pixmap* p = PixmapFromHBITMAP(rb->hbmp, rb->size, rb->hMap);
    rb->hbmp = nullptr;
    rb->hMap = nullptr;
    delete rb;
    return p;
}

RenderedBitmap* RenderedBitmapFromPixmap(Pixmap* px) {
    if (!px) {
        return nullptr;
    }
    if (!px->hbmp) {
        if (!px->data) {
            FreePixmap(px);
            return nullptr;
        }
        Pixmap* dib = AllocPixmapDIB(px->width, px->height);
        if (!dib) {
            FreePixmap(px);
            return nullptr;
        }
        for (int y = 0; y < px->height; y++) {
            const u8* src = px->data + ((size_t)y * px->stride);
            u8* dst = dib->data + ((size_t)y * dib->stride);
            for (int x = 0; x < px->width; x++) {
                if (px->format == PixmapFormat::BGR8) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 0xff;
                    src += 3;
                } else if (px->format == PixmapFormat::RGBA8) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                    src += 4;
                } else {
                    memcpy(dst, src, 4);
                    src += 4;
                }
                dst += 4;
            }
        }
        FreePixmap(px);
        px = dib;
    }
    auto* rb = new RenderedBitmap(px->hbmp, Size(px->width, px->height), px->hMap);
    px->hbmp = nullptr;
    px->hMap = nullptr;
    px->data = nullptr;
    FreePixmap(px);
    return rb;
}

void FreePixmapNativeBitmap(Pixmap* p) {
    if (!p) {
        return;
    }
    if (p->hbmp) {
        DeleteObject(p->hbmp);
        p->hbmp = nullptr;
    }
    if (p->hMap) {
        CloseHandle(p->hMap);
        p->hMap = nullptr;
    }
    p->data = nullptr;
}

bool BlitPixmapRegion(Pixmap* p, HDC hdc, Rect target, Rect source) {
    if (!p || !p->data || target.IsEmpty() || source.IsEmpty()) {
        return false;
    }
    SetStretchBltMode(hdc, HALFTONE);
    if (p->hbmp) {
        HDC bmpDC = CreateCompatibleDC(hdc);
        if (!bmpDC) {
            return false;
        }
        HGDIOBJ oldBmp = SelectObject(bmpDC, p->hbmp);
        bool ok = false;
        if (oldBmp && target.dx == source.dx && target.dy == source.dy) {
            ok = BitBlt(hdc, target.x, target.y, target.dx, target.dy, bmpDC, source.x, source.y, SRCCOPY) != 0;
        } else if (oldBmp) {
            ok = StretchBlt(hdc, target.x, target.y, target.dx, target.dy, bmpDC, source.x, source.y, source.dx,
                            source.dy, SRCCOPY) != 0;
        }
        if (oldBmp) {
            SelectObject(bmpDC, oldBmp);
        }
        DeleteDC(bmpDC);
        return ok;
    }
    source = Rect(0, 0, p->width, p->height).Intersect(source);
    if (source.IsEmpty()) {
        return false;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = p->width;
    bmi.bmiHeader.biHeight = -source.dy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = p->format == PixmapFormat::BGR8 ? 24 : 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const u8* rows = p->data + ((size_t)source.y * p->stride);
    int n = StretchDIBits(hdc, target.x, target.y, target.dx, target.dy, source.x, 0, source.dx, source.dy, rows, &bmi,
                          DIB_RGB_COLORS, SRCCOPY);
    return n != GDI_ERROR && n != 0;
}

bool BlitPixmap(Pixmap* p, HDC hdc, Rect target) {
    if (!p || !p->data) {
        return false;
    }
    if (p->hbmp) {
        return BlitHBITMAP(p->hbmp, hdc, target);
    }
    return BlitPixmapRegion(p, hdc, target, Rect(0, 0, p->width, p->height));
}

static bool SkipRecolorPixel(int x, int y, Vec<Rect>* skipRects) {
    if (skipRects) {
        for (Rect& r : *skipRects) {
            if (r.Contains(x, y)) {
                return true;
            }
        }
    }
    return false;
}

static int Mul255(int a, int b) {
    int n = (a * b) + 128;
    n += n >> 8;
    return n >> 8;
}

void RecolorPixmap(Pixmap* px, COLORREF textColor, COLORREF bgColor, COLORREF linkColor, Vec<Rect>* skipRects) {
    if (!px) {
        return;
    }
    if (px->hbmp) {
        UpdateBitmapColors(px->hbmp, textColor, bgColor, linkColor, skipRects);
        return;
    }
    if (!px->data || px->width <= 0 || px->height <= 0 || px->format == PixmapFormat::RGBA8) {
        return;
    }
    if ((textColor & 0xffffff) == WIN_COL_BLACK && (bgColor & 0xffffff) == WIN_COL_WHITE && !linkColor && !skipRects) {
        return;
    }
    byte linkR = 0, linkG = 0, linkB = 0;
    UnpackColor(linkColor, linkR, linkG, linkB);
    byte textR, textG, textB, bgR, bgG, bgB;
    UnpackColor(textColor, textR, textG, textB);
    UnpackColor(bgColor, bgR, bgG, bgB);
    const int base[3] = {textB, textG, textR};
    const int diff[3] = {(int)bgB - textB, (int)bgG - textG, (int)bgR - textR};
    int bpp = PixmapBytesPerPixel(px->format);
    for (int y = 0; y < px->height; y++) {
        u8* pixel = px->data + ((size_t)y * px->stride);
        for (int x = 0; x < px->width; x++, pixel += bpp) {
            if (SkipRecolorPixel(x, y, skipRects)) {
                continue;
            }
            int maxRG = pixel[2] > pixel[1] ? pixel[2] : pixel[1];
            int lum = (pixel[0] + pixel[1] + pixel[2]) / 3;
            if (linkColor && pixel[0] >= maxRG + 25 && pixel[0] >= 72 && lum <= 230) {
                pixel[0] = linkB;
                pixel[1] = linkG;
                pixel[2] = linkR;
                continue;
            }
            for (int i = 0; i < 3; i++) {
                pixel[i] = (u8)(base[i] + Mul255(pixel[i], diff[i]));
            }
        }
    }
}

// the night light's multipliers, in 1/256ths: at full strength white becomes
// (255, 205, 115), a candle-like warm; halfway it is a soft cream
static void WarmFactors(int strength, int* g, int* b) {
    strength = std::clamp(strength, 0, 100);
    *g = 256 - strength * 51 / 100;
    *b = 256 - strength * 140 / 100;
}

// A page with no colour renders into an 8-bit DIB whose pixels index a grey
// palette: warming the palette warms the page, 256 entries instead of millions
// of pixels (UpdateBitmapColors recolours those the same way).
static void WarmDibPalette(HBITMAP hbmp, int g, int b) {
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        return;
    }
    HGDIOBJ prev = SelectObject(hdc, hbmp);
    RGBQUAD palette[256];
    uint n = GetDIBColorTable(hdc, 0, dimof(palette), palette);
    for (uint i = 0; i < n; i++) {
        palette[i].rgbGreen = (u8)((palette[i].rgbGreen * g) >> 8);
        palette[i].rgbBlue = (u8)((palette[i].rgbBlue * b) >> 8);
    }
    if (n > 0) {
        SetDIBColorTable(hdc, 0, n, palette);
    }
    SelectObject(hdc, prev);
    DeleteDC(hdc);
}

void WarmPixmap(Pixmap* px, int strength) {
    if (!px || !px->data || strength <= 0) {
        return;
    }
    int g, b;
    WarmFactors(strength, &g, &b);
    int dx = px->width;
    int dy = px->height;
    int stride = px->stride;
    int bpp = PixmapBytesPerPixel(px->format);
    if (px->hbmp) {
        // A rendered tile is a DIB, and its pixel layout is the DIB's, not
        // what the Pixmap's format says: a page with no colour is 8-bit and
        // paletted, one byte a pixel, and read as four it ran off the end.
        DIBSECTION ds{};
        if (GetObject(px->hbmp, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) {
            return;
        }
        if (ds.dsBm.bmBitsPixel <= 8) {
            WarmDibPalette(px->hbmp, g, b);
            return;
        }
        if (ds.dsBm.bmBitsPixel != 24 && ds.dsBm.bmBitsPixel != 32) {
            return;
        }
        dx = ds.dsBm.bmWidth;
        dy = std::abs(ds.dsBm.bmHeight);
        stride = ds.dsBm.bmWidthBytes;
        bpp = ds.dsBm.bmBitsPixel / 8;
    } else if (px->format != PixmapFormat::BGR8 && px->format != PixmapFormat::BGRA8) {
        return;
    }
    if (dx <= 0 || dy <= 0) {
        return;
    }
    for (int y = 0; y < dy; y++) {
        u8* pixel = px->data + ((size_t)y * stride);
        for (int x = 0; x < dx; x++, pixel += bpp) {
            pixel[0] = (u8)((pixel[0] * b) >> 8);
            pixel[1] = (u8)((pixel[1] * g) >> 8);
        }
    }
}

COLORREF WarmColor(COLORREF c, int strength) {
    if (strength <= 0) {
        return c;
    }
    int g, b;
    WarmFactors(strength, &g, &b);
    return RGB(GetRValue(c), (GetGValue(c) * g) >> 8, (GetBValue(c) * b) >> 8);
}

// Returns a copy of the clipboard bitmap as a platform-independent Pixmap.
Pixmap* GetClipboardImageAsPixmap() {
    if (!IsClipboardFormatAvailable(CF_BITMAP) || !OpenClipboard(nullptr)) {
        return nullptr;
    }

    Pixmap* pixmap = nullptr;
    // CF_BITMAP is synthesized by Windows from CF_DIB and vice versa. The HBITMAP
    // returned by GetClipboardData() remains owned by the clipboard.
    HBITMAP hbmp = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (hbmp) {
        Size size = GetBitmapSize(hbmp);
        pixmap = AllocPixmap(size.dx, size.dy, PixmapFormat::BGR8);
        if (pixmap) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = size.dx;
            bmi.bmiHeader.biHeight = -size.dy;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;

            HDC hdc = GetDC(nullptr);
            if (!GetDIBits(hdc, hbmp, 0, size.dy, pixmap->data, &bmi, DIB_RGB_COLORS)) {
                FreePixmap(pixmap);
                pixmap = nullptr;
            }
            ReleaseDC(nullptr, hdc);
        }
    }
    CloseClipboard();
    return pixmap;
}
