/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Draw mode: draw on PDF pages with the mouse or a pen. A floating toolbar
// picks the tool (pen, line, arrow, rectangle, ellipse), the color and the
// thickness. Holding the pen still at the end of a freehand stroke snaps it to
// the clean shape it resembles (see InkShapes.cpp). Each finished mark becomes
// a standard PDF annotation straight away, so it is saved with the document's
// other annotations; Undo removes this session's marks one by one.
//
// Finger touch is deliberately not drawing input: a finger drag keeps
// scrolling (WM_GESTURE pan) and a finger tap stays a tap, so a palm or a
// scrolling hand on a Surface never leaves marks. Mouse and pen draw.

#include "base/Base.h"
#include "base/ScopedWin.h"
#include "base/Win.h"
#include "base/Dpi.h"
#include "base/GdiPlusUtil.h"
#include "base/GuessFileType.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "Annotation.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "EditAnnotations.h"
#include "TableOfContents.h"
#include "Toolbar.h"
#include "TopBar.h"
#include "Rail.h"
#include "Theme.h"
#include "InkShapes.h"
#include "DrawMode.h"


enum class DrawTool {
    Pen,
    Line,
    Arrow,
    Rect,
    Ellipse,
};

// the colors and thicknesses offered on the toolbar; thickness is in page
// points, so a mark keeps its weight relative to the page at any zoom
static const COLORREF kDrawColors[] = {
    RGB(0x1f, 0x1f, 0x1f), // ink black
    RGB(0xe0, 0x31, 0x31), // red
    RGB(0x19, 0x71, 0xc2), // blue
    RGB(0x2f, 0x9e, 0x44), // green
    RGB(0xf0, 0x8c, 0x00), // orange
};
static const float kDrawWidths[] = {1.5f, 3.0f, 6.0f};

// holding the pen within this many pixels for this long snaps the stroke
constexpr int kHoldSlackPx = 4;
constexpr uint kHoldDelayMs = 550;
// a committed mark stays painted on top of the page until the re-render that
// includes its annotation has had time to arrive
constexpr uint kPendingShowMs = 1500;

// what the user chose; kept across documents and windows for the session
static DrawTool gDrawTool = DrawTool::Pen;
static int gDrawColorIdx = 0;
static int gDrawWidthIdx = 1;

// a mark as painted on the canvas: in page coordinates, so it stays put if the
// page scrolls while it waits for the re-render
struct DrawnMark {
    int pageNo = 0;
    AnnotationType type = AnnotationType::Ink;
    bool arrow = false;
    Vec<PointF> pts;
    COLORREF col = 0;
    float widthPt = 2;
    Annotation* annot = nullptr; // the annotation it became
    DWORD shownAt = 0;
};

enum class BarItemKind {
    Tool,
    Color,
    Width,
    Undo,
    Done,
    Separator,
};

struct BarItem {
    BarItemKind kind;
    int idx; // tool / color / width index
};

static const BarItem gBarItems[] = {
    {BarItemKind::Tool, (int)DrawTool::Pen},
    {BarItemKind::Tool, (int)DrawTool::Line},
    {BarItemKind::Tool, (int)DrawTool::Arrow},
    {BarItemKind::Tool, (int)DrawTool::Rect},
    {BarItemKind::Tool, (int)DrawTool::Ellipse},
    {BarItemKind::Separator, 0},
    {BarItemKind::Color, 0},
    {BarItemKind::Color, 1},
    {BarItemKind::Color, 2},
    {BarItemKind::Color, 3},
    {BarItemKind::Color, 4},
    {BarItemKind::Separator, 0},
    {BarItemKind::Width, 0},
    {BarItemKind::Width, 1},
    {BarItemKind::Width, 2},
    {BarItemKind::Separator, 0},
    {BarItemKind::Undo, 0},
    {BarItemKind::Done, 0},
};
constexpr int kBarItemCount = (int)dimof(gBarItems);

// logical sizes, DpiScale'd at use (TouchMetrics convention)
constexpr int kBarBtnDx = 44;
constexpr int kBarSepDx = 13;
constexpr int kBarPad = 6;
constexpr int kBarTopMargin = 12;

struct DrawModeState {
    MainWindow* win = nullptr;
    bool on = false;

    // the stroke being drawn, in canvas coordinates
    bool drawing = false;
    int pageNo = 0;
    Vec<Point> pts;
    Point holdAnchor;
    bool snapped = false;
    InkShape shape; // when snapped, in canvas coordinates

    Vec<DrawnMark*> pending;

    // marks made in this draw session, newest last
    Vec<Annotation*> undo;
    EngineBase* undoEngine = nullptr;

    HWND hwndBar = nullptr;
    Rect itemRects[kBarItemCount];
    int hot = -1;
    int pressed = -1;
    bool trackingMouse = false;
};

static DrawModeState* GetState(MainWindow* win) {
    return win ? win->drawMode : nullptr;
}

bool IsDrawModeOn(MainWindow* win) {
    DrawModeState* st = GetState(win);
    return st && st->on;
}

static float CurrentWidthPt() {
    return kDrawWidths[gDrawWidthIdx];
}

static COLORREF CurrentColor() {
    return kDrawColors[gDrawColorIdx];
}

static EngineBase* CurrentEngine(MainWindow* win) {
    DisplayModel* dm = win->AsFixed();
    return dm ? dm->GetEngine() : nullptr;
}

static bool CanDrawOn(MainWindow* win) {
    if (!win || !win->IsDocLoaded() || win->touchView != TouchView::Doc) {
        return false;
    }
    EngineBase* engine = CurrentEngine(win);
    return engine && EngineSupportsAnnotations(engine);
}

// Undo entries point at annotations of one engine. Saving reloads the document
// into a new engine, after which they mean nothing: drop them rather than test
// (and dereference) stale pointers.
static void DropStaleUndo(DrawModeState* st) {
    EngineBase* engine = CurrentEngine(st->win);
    if (st->undoEngine != engine) {
        st->undo.Reset();
        st->undoEngine = engine;
    }
}

static void FreePending(DrawModeState* st) {
    for (DrawnMark* m : st->pending) {
        delete m;
    }
    st->pending.Reset();
}

// ----- painting marks

static Gdiplus::Color GdipCol(COLORREF c, u8 alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void DrawArrowHead(Gdiplus::Graphics& g, Gdiplus::Pen& pen, Gdiplus::PointF from, Gdiplus::PointF to,
                          float widthPx) {
    float dx = to.X - from.X;
    float dy = to.Y - from.Y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) {
        return;
    }
    float head = std::max(10.0f, widthPx * 4.5f);
    float ux = dx / len;
    float uy = dy / len;
    // 30 degrees either side of the shaft, like MuPDF's open arrow
    float c = 0.866f;
    float s = 0.5f;
    Gdiplus::PointF a(to.X - head * (ux * c - uy * s), to.Y - head * (uy * c + ux * s));
    Gdiplus::PointF b(to.X - head * (ux * c + uy * s), to.Y - head * (uy * c - ux * s));
    g.DrawLine(&pen, to, a);
    g.DrawLine(&pen, to, b);
}

// draws a shape given in canvas coordinates
static void PaintShape(Gdiplus::Graphics& g, AnnotationType type, bool arrow, const Vec<Gdiplus::PointF>& pts,
                       COLORREF col, float widthPx) {
    int n = len(pts);
    if (n == 0) {
        return;
    }
    Gdiplus::Pen pen(GdipCol(col), widthPx);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    switch (type) {
        case AnnotationType::Ink:
            if (n == 1) {
                Gdiplus::SolidBrush br(GdipCol(col));
                g.FillEllipse(&br, pts[0].X - widthPx / 2, pts[0].Y - widthPx / 2, widthPx, widthPx);
            } else {
                g.DrawLines(&pen, pts.LendData(), n);
            }
            break;
        case AnnotationType::Line:
            if (n >= 2) {
                g.DrawLine(&pen, pts[0], pts[1]);
                if (arrow) {
                    DrawArrowHead(g, pen, pts[0], pts[1], widthPx);
                }
            }
            break;
        case AnnotationType::Square:
        case AnnotationType::Circle:
            if (n >= 2) {
                float x0 = std::min(pts[0].X, pts[1].X);
                float y0 = std::min(pts[0].Y, pts[1].Y);
                float dx = fabsf(pts[1].X - pts[0].X);
                float dy = fabsf(pts[1].Y - pts[0].Y);
                if (type == AnnotationType::Square) {
                    g.DrawRectangle(&pen, x0, y0, dx, dy);
                } else {
                    g.DrawEllipse(&pen, x0, y0, dx, dy);
                }
            }
            break;
        case AnnotationType::Polygon:
            if (n >= 3) {
                g.DrawPolygon(&pen, pts.LendData(), n);
            }
            break;
        default:
            break;
    }
}

static void PaintMark(Gdiplus::Graphics& g, DisplayModel* dm, DrawnMark* m) {
    if (!dm->PageVisible(m->pageNo)) {
        return;
    }
    Vec<Gdiplus::PointF> pts;
    for (PointF p : m->pts) {
        Point s = dm->CvtToScreen(m->pageNo, p);
        pts.Append(Gdiplus::PointF((float)s.x, (float)s.y));
    }
    float widthPx = std::max(1.0f, m->widthPt * dm->GetZoomReal(m->pageNo));
    PaintShape(g, m->type, m->arrow, pts, m->col, widthPx);
}

static AnnotationType ShapeToAnnotType(InkShapeKind kind) {
    switch (kind) {
        case InkShapeKind::Line:
            return AnnotationType::Line;
        case InkShapeKind::Rect:
            return AnnotationType::Square;
        case InkShapeKind::Ellipse:
            return AnnotationType::Circle;
        case InkShapeKind::Polygon:
            return AnnotationType::Polygon;
        default:
            return AnnotationType::Ink;
    }
}

static AnnotationType ToolAnnotType(DrawTool tool) {
    switch (tool) {
        case DrawTool::Line:
        case DrawTool::Arrow:
            return AnnotationType::Line;
        case DrawTool::Rect:
            return AnnotationType::Square;
        case DrawTool::Ellipse:
            return AnnotationType::Circle;
        default:
            return AnnotationType::Ink;
    }
}

// the stroke in progress, as it should look right now (canvas coordinates)
static void CurrentStrokeGeometry(DrawModeState* st, AnnotationType& type, bool& arrow, Vec<Gdiplus::PointF>& out) {
    out.Reset();
    arrow = false;
    if (st->snapped) {
        type = ShapeToAnnotType(st->shape.kind);
        for (PointF p : st->shape.points) {
            out.Append(Gdiplus::PointF(p.x, p.y));
        }
        return;
    }
    type = ToolAnnotType(gDrawTool);
    if (type == AnnotationType::Ink) {
        for (Point p : st->pts) {
            out.Append(Gdiplus::PointF((float)p.x, (float)p.y));
        }
        return;
    }
    // a shape tool drags from the first point to the current one
    Point a = st->pts[0];
    Point b = st->pts.Last();
    out.Append(Gdiplus::PointF((float)a.x, (float)a.y));
    out.Append(Gdiplus::PointF((float)b.x, (float)b.y));
    arrow = gDrawTool == DrawTool::Arrow;
}

static float StrokeWidthPx(DrawModeState* st) {
    DisplayModel* dm = st->win->AsFixed();
    float zoom = dm ? dm->GetZoomReal(st->pageNo) : 1.0f;
    return std::max(1.0f, CurrentWidthPt() * zoom);
}

// Called from DrawDocument after the pages and other overlays are painted.
void PaintDrawModeOverlay(MainWindow* win, HDC hdc) {
    DrawModeState* st = GetState(win);
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    if (!st || !dm || (!st->drawing && len(st->pending) == 0)) {
        return;
    }
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    for (DrawnMark* m : st->pending) {
        PaintMark(g, dm, m);
    }
    if (st->drawing && len(st->pts) > 0) {
        AnnotationType type;
        bool arrow;
        Vec<Gdiplus::PointF> pts;
        CurrentStrokeGeometry(st, type, arrow, pts);
        PaintShape(g, type, arrow, pts, CurrentColor(), StrokeWidthPx(st));
    }
}

// The newest freehand segment, drawn straight onto the window so the ink
// follows the pen without waiting for a repaint of the whole canvas.
static void PaintLatestSegment(DrawModeState* st) {
    int n = len(st->pts);
    if (n < 2) {
        return;
    }
    HWND hwnd = st->win->hwndCanvas;
    HDC hdc = GetDC(hwnd);
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::Pen pen(GdipCol(CurrentColor()), StrokeWidthPx(st));
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        Point a = st->pts[n - 2];
        Point b = st->pts[n - 1];
        g.DrawLine(&pen, (float)a.x, (float)a.y, (float)b.x, (float)b.y);
    }
    ReleaseDC(hwnd, hdc);
}

// ----- the toolbar

static constexpr const WCHAR* kDrawBarClassName = L"SumatraDrawModeToolbar";

static bool BarIsDark() {
    return !IsLightColor(ThemeWindowBackgroundColor());
}

static COLORREF BarBg() {
    if (BarIsDark()) {
        return ThemeWindowBackgroundColor();
    }
    COLORREF contentBg;
    ThemePageRenderColors(contentBg);
    return AccentColor(contentBg, 6);
}

static COLORREF BarFg() {
    return BarIsDark() ? ThemeWindowTextColor() : RGB(27, 29, 33);
}

static COLORREF BarMuted() {
    return BarIsDark() ? ThemeWindowTextDisabledColor() : RGB(150, 154, 160);
}

static COLORREF BarBorder() {
    return BarIsDark() ? AccentColor(ThemeWindowControlBackgroundColor(), 35) : AccentColor(BarBg(), 14);
}

static bool IsItemActive(const BarItem& it) {
    switch (it.kind) {
        case BarItemKind::Tool:
            return it.idx == (int)gDrawTool;
        case BarItemKind::Color:
            return it.idx == gDrawColorIdx;
        case BarItemKind::Width:
            return it.idx == gDrawWidthIdx;
        default:
            return false;
    }
}

static bool IsItemEnabled(DrawModeState* st, const BarItem& it) {
    if (it.kind == BarItemKind::Undo) {
        return DrawModeCanUndo(st->win);
    }
    return it.kind != BarItemKind::Separator;
}

static Size BarSize(HWND hwnd) {
    int dx = 0;
    for (const BarItem& it : gBarItems) {
        dx += DpiScale(hwnd, it.kind == BarItemKind::Separator ? kBarSepDx : kBarBtnDx);
    }
    int pad = DpiScale(hwnd, kBarPad);
    return Size(dx + 2 * pad, DpiScale(hwnd, kBarBtnDx) + 2 * pad);
}

static void LayoutBarItems(DrawModeState* st) {
    HWND hwnd = st->hwndBar;
    int pad = DpiScale(hwnd, kBarPad);
    int btn = DpiScale(hwnd, kBarBtnDx);
    int x = pad;
    for (int i = 0; i < kBarItemCount; i++) {
        int dx = gBarItems[i].kind == BarItemKind::Separator ? DpiScale(hwnd, kBarSepDx) : btn;
        st->itemRects[i] = Rect(x, pad, dx, btn);
        x += dx;
    }
}

static int BarItemFromPoint(DrawModeState* st, int x, int y) {
    for (int i = 0; i < kBarItemCount; i++) {
        if (gBarItems[i].kind != BarItemKind::Separator && st->itemRects[i].Contains(Point(x, y))) {
            return i;
        }
    }
    return -1;
}

static void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float dx, float dy, float d) {
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + dx - d, y, d, d, 270, 90);
    path.AddArc(x + dx - d, y + dy - d, d, d, 0, 90);
    path.AddArc(x, y + dy - d, d, d, 90, 90);
    path.CloseFigure();
}

// the glyph for an item, drawn around (cx, cy); s is 1 logical pixel
static void PaintBarGlyph(Gdiplus::Graphics& g, const BarItem& it, float cx, float cy, float s, COLORREF fg,
                          bool active) {
    Gdiplus::Pen pen(GdipCol(fg), 1.8f * s);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    auto P = [&](float x, float y) { return Gdiplus::PointF(cx + x * s, cy + y * s); };
    switch (it.kind) {
        case BarItemKind::Tool:
            switch ((DrawTool)it.idx) {
                case DrawTool::Pen:
                    g.DrawBezier(&pen, P(-9, 5), P(-4, -11), P(3, 14), P(9, -5));
                    break;
                case DrawTool::Line:
                    g.DrawLine(&pen, P(-8, 8), P(8, -8));
                    break;
                case DrawTool::Arrow:
                    g.DrawLine(&pen, P(-8, 8), P(8, -8));
                    g.DrawLine(&pen, P(8, -8), P(1, -8));
                    g.DrawLine(&pen, P(8, -8), P(8, -1));
                    break;
                case DrawTool::Rect:
                    g.DrawRectangle(&pen, cx - 9 * s, cy - 7 * s, 18 * s, 14 * s);
                    break;
                case DrawTool::Ellipse:
                    g.DrawEllipse(&pen, cx - 10 * s, cy - 7 * s, 20 * s, 14 * s);
                    break;
            }
            break;
        case BarItemKind::Color: {
            Gdiplus::SolidBrush br(GdipCol(kDrawColors[it.idx]));
            float r = 9 * s;
            g.FillEllipse(&br, cx - r, cy - r, 2 * r, 2 * r);
            if (active) {
                Gdiplus::Pen ring(GdipCol(fg), 2 * s);
                float rr = 13 * s;
                g.DrawEllipse(&ring, cx - rr, cy - rr, 2 * rr, 2 * rr);
            }
        } break;
        case BarItemKind::Width: {
            static const float radii[] = {2.0f, 3.5f, 5.5f};
            Gdiplus::SolidBrush br(GdipCol(fg));
            float r = radii[it.idx] * s;
            g.FillEllipse(&br, cx - r, cy - r, 2 * r, 2 * r);
        } break;
        case BarItemKind::Undo:
            // a curved arrow pointing back to the left
            g.DrawArc(&pen, cx - 7 * s, cy - 5 * s, 15 * s, 12 * s, 180, 225);
            g.DrawLine(&pen, P(-7, 1), P(-10, -3));
            g.DrawLine(&pen, P(-7, 1), P(-2, -1));
            break;
        case BarItemKind::Done:
            g.DrawLine(&pen, P(-8, 0), P(-3, 6));
            g.DrawLine(&pen, P(-3, 6), P(8, -6));
            break;
        default:
            break;
    }
}

static void PaintBar(DrawModeState* st, HDC hdcWin) {
    HWND hwnd = st->hwndBar;
    Rect rc = HwndClientRect(hwnd);
    DoubleBuffer buffer(hwnd, rc);
    HDC hdc = buffer.GetDC();
    COLORREF bg = BarBg();
    COLORREF fg = BarFg();
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // what's behind the rounded ends is the canvas background
        Gdiplus::SolidBrush canvasBr(GdipCol(ThemeMainWindowBackgroundColor()));
        g.FillRectangle(&canvasBr, 0, 0, rc.dx, rc.dy);

        Gdiplus::GraphicsPath pill;
        AddRoundRect(pill, 0.5f, 0.5f, (float)rc.dx - 1, (float)rc.dy - 1, (float)rc.dy - 1);
        Gdiplus::SolidBrush bgBr(GdipCol(bg));
        g.FillPath(&bgBr, &pill);
        Gdiplus::Pen border(GdipCol(BarBorder()), 1);
        g.DrawPath(&border, &pill);

        float s = (float)DpiScale(hwnd, 100) / 100.0f;
        COLORREF activeBg, activeFg;
        ThemeAccentSurfaceColors(&activeBg, &activeFg);
        for (int i = 0; i < kBarItemCount; i++) {
            const BarItem& it = gBarItems[i];
            Rect r = st->itemRects[i];
            float cx = (float)r.x + (float)r.dx / 2;
            float cy = (float)r.y + (float)r.dy / 2;
            if (it.kind == BarItemKind::Separator) {
                Gdiplus::Pen sep(GdipCol(BarBorder()), 1);
                g.DrawLine(&sep, cx, (float)r.y + 10 * s, cx, (float)(r.y + r.dy) - 10 * s);
                continue;
            }
            bool enabled = IsItemEnabled(st, it);
            bool active = IsItemActive(it);
            // a color shows itself as the selection ring; the other items get a
            // filled accent chip like the top bar's active buttons
            bool chip = active && it.kind != BarItemKind::Color;
            COLORREF itemFg = !enabled ? BarMuted() : (chip ? activeFg : fg);
            COLORREF wash = 0;
            bool hasWash = false;
            if (chip) {
                wash = activeBg;
                hasWash = true;
            }
            if (enabled && (i == st->hot || i == st->pressed)) {
                wash = AccentColor(hasWash ? wash : bg, i == st->pressed ? 18 : 10);
                hasWash = true;
            }
            if (hasWash) {
                Gdiplus::GraphicsPath chipPath;
                float inset = 2 * s;
                AddRoundRect(chipPath, (float)r.x + inset, (float)r.y + inset, (float)r.dx - 2 * inset,
                             (float)r.dy - 2 * inset, 20 * s);
                Gdiplus::SolidBrush washBr(GdipCol(wash));
                g.FillPath(&washBr, &chipPath);
            }
            if (it.kind == BarItemKind::Width) {
                // thickness dots take the current color, so the bar previews the pen
                itemFg = chip ? activeFg : CurrentColor();
                if (BarIsDark() && gDrawColorIdx == 0 && !chip) {
                    itemFg = fg;
                }
            }
            PaintBarGlyph(g, it, cx, cy, s, itemFg, active);
        }
    }
    buffer.Flush(hdcWin);
}

static void UpdateBarRgn(HWND hwnd, Size sz) {
    HRGN rgn = CreateRoundRectRgn(0, 0, sz.dx + 1, sz.dy + 1, sz.dy, sz.dy);
    if (!SetWindowRgn(hwnd, rgn, TRUE)) {
        DeleteObject(rgn);
    }
}

static void RepaintBar(DrawModeState* st) {
    if (st->hwndBar) {
        InvalidateRect(st->hwndBar, nullptr, FALSE);
    }
}

static void OnBarItemClicked(DrawModeState* st, int idx) {
    const BarItem& it = gBarItems[idx];
    MainWindow* win = st->win;
    switch (it.kind) {
        case BarItemKind::Tool:
            gDrawTool = (DrawTool)it.idx;
            break;
        case BarItemKind::Color:
            gDrawColorIdx = it.idx;
            break;
        case BarItemKind::Width:
            gDrawWidthIdx = it.idx;
            break;
        case BarItemKind::Undo:
            DrawModeUndo(win);
            break;
        case BarItemKind::Done:
            ExitDrawMode(win);
            return;
        default:
            break;
    }
    RepaintBar(st);
}

static LRESULT CALLBACK WndProcDrawBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DrawModeState* st;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<LPCREATESTRUCT>(lp);
        st = reinterpret_cast<DrawModeState*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    st = reinterpret_cast<DrawModeState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!st) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return TRUE;

        case WM_MOUSEMOVE: {
            int idx = BarItemFromPoint(st, x, y);
            if (idx >= 0 && !IsItemEnabled(st, gBarItems[idx])) {
                idx = -1;
            }
            if (idx != st->hot) {
                st->hot = idx;
                RepaintBar(st);
            }
            if (!st->trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                st->trackingMouse = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            st->trackingMouse = false;
            if (st->hot != -1 || st->pressed != -1) {
                st->hot = -1;
                st->pressed = -1;
                RepaintBar(st);
            }
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int idx = BarItemFromPoint(st, x, y);
            st->pressed = (idx >= 0 && IsItemEnabled(st, gBarItems[idx])) ? idx : -1;
            if (st->pressed >= 0) {
                SetCapture(hwnd);
            }
            RepaintBar(st);
            return 0;
        }

        case WM_LBUTTONUP: {
            int pressed = st->pressed;
            st->pressed = -1;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            int idx = BarItemFromPoint(st, x, y);
            if (pressed >= 0 && idx == pressed) {
                OnBarItemClicked(st, idx);
            } else {
                RepaintBar(st);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintBar(st, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void CreateBar(DrawModeState* st) {
    if (st->hwndBar) {
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEX wcex{};
        FillWndClassEx(wcex, WStrL(kDrawBarClassName), WndProcDrawBar);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.style |= CS_DBLCLKS;
        RegisterClassEx(&wcex);
        registered = true;
    }
    // a child of the frame placed over the canvas: it moves with the window,
    // and the canvas (WS_CLIPSIBLINGS) doesn't paint over it
    DWORD style = WS_CHILD | WS_CLIPSIBLINGS;
    st->hwndBar = CreateWindowExW(0, kDrawBarClassName, nullptr, style, 0, 0, 0, 0, st->win->hwndFrame, nullptr,
                                  GetModuleHandle(nullptr), st);
}

// Places the toolbar at the top centre of the canvas, or hides it. Called from
// RelayoutFrame with the canvas rect (frame client coordinates).
void LayoutDrawModeToolbar(MainWindow* win, Rect canvasRc) {
    DrawModeState* st = GetState(win);
    if (!st || !st->hwndBar) {
        return;
    }
    bool show = st->on && CanDrawOn(win);
    if (!show) {
        ShowWindow(st->hwndBar, SW_HIDE);
        return;
    }
    Size sz = BarSize(st->hwndBar);
    int x = canvasRc.x + (canvasRc.dx - sz.dx) / 2;
    x = std::max(canvasRc.x, x);
    int y = canvasRc.y + DpiScale(st->hwndBar, kBarTopMargin);
    SetWindowPos(st->hwndBar, HWND_TOP, x, y, sz.dx, sz.dy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateBarRgn(st->hwndBar, sz);
    LayoutBarItems(st);
    RepaintBar(st);
}

static void RelayoutBar(MainWindow* win) {
    if (!win->hwndCanvas) {
        return;
    }
    LayoutDrawModeToolbar(win, ChildPosWithinParent(win->hwndCanvas));
}

// ----- mode on / off

static void UpdateChrome(MainWindow* win) {
    // the top bar's pen button shows whether draw mode is on
    UpdateTopBarForWindow(win);
}

void ToggleDrawMode(MainWindow* win) {
    if (IsDrawModeOn(win)) {
        ExitDrawMode(win);
        return;
    }
    if (!CanDrawOn(win)) {
        return;
    }
    DrawModeState* st = GetState(win);
    if (!st) {
        st = new DrawModeState();
        st->win = win;
        win->drawMode = st;
    }
    CreateBar(st);
    st->on = true;
    DropStaleUndo(st);
    logf("draw: on (tool %d, color %d, width %d)\n", (int)gDrawTool, gDrawColorIdx, gDrawWidthIdx);
    RelayoutBar(win);
    UpdateChrome(win);
}

void ExitDrawMode(MainWindow* win) {
    DrawModeState* st = GetState(win);
    if (!st || !st->on) {
        return;
    }
    DrawModeCancelStroke(win);
    st->on = false;
    st->undo.Reset();
    FreePending(st);
    if (st->hwndBar) {
        ShowWindow(st->hwndBar, SW_HIDE);
    }
    logf("draw: off\n");
    ScheduleRepaint(win, 0);
    UpdateChrome(win);
}

void DeleteDrawMode(MainWindow* win) {
    DrawModeState* st = GetState(win);
    if (!st) {
        return;
    }
    if (st->hwndBar) {
        DestroyWindow(st->hwndBar);
    }
    FreePending(st);
    delete st;
    win->drawMode = nullptr;
}

// ----- input

// A tap or drag with a finger reaches the canvas as a mouse message that
// Windows synthesized from touch; it carries this signature.
static bool IsMouseMessageFromTouch() {
    LPARAM extra = GetMessageExtraInfo();
    constexpr LPARAM kSignatureMask = (LPARAM)0xFFFFFF00;
    constexpr LPARAM kSignature = (LPARAM)0xFF515700;
    return (extra & kSignatureMask) == kSignature && (extra & 0x80) != 0;
}

bool DrawModeCancelStroke(MainWindow* win) {
    DrawModeState* st = GetState(win);
    if (!st || !st->drawing) {
        return false;
    }
    st->drawing = false;
    st->snapped = false;
    st->pts.Reset();
    KillTimer(win->hwndCanvas, kDrawHoldTimerID);
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    ScheduleRepaint(win, 0);
    return true;
}

static void CommitStroke(DrawModeState* st);

bool DrawModeOnMouseDown(MainWindow* win, int x, int y) {
    DrawModeState* st = GetState(win);
    if (!st || !st->on || IsMouseMessageFromTouch() || !CanDrawOn(win)) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (st->drawing) {
        // the up for the last stroke never arrived (capture lost): keep it
        CommitStroke(st);
    }
    int pageNo = dm->GetPageNoByPoint(Point(x, y));
    if (pageNo <= 0) {
        // off the page: let the usual drag (panning) happen
        return false;
    }
    DropStaleUndo(st);
    st->drawing = true;
    st->snapped = false;
    st->pageNo = pageNo;
    st->pts.Reset();
    st->pts.Append(Point(x, y));
    st->holdAnchor = Point(x, y);
    SetCapture(win->hwndCanvas);
    HwndSetFocus(win->hwndFrame);
    if (gDrawTool == DrawTool::Pen) {
        SetTimer(win->hwndCanvas, kDrawHoldTimerID, kHoldDelayMs, nullptr);
        // a dot shows as soon as the pen touches down
        ScheduleRepaint(win, 0);
    }
    return true;
}

bool DrawModeOnMouseMove(MainWindow* win, int x, int y) {
    DrawModeState* st = GetState(win);
    if (!st || !st->drawing) {
        return false;
    }
    Point pt(x, y);
    if (st->snapped) {
        // the shape is set; moving on only matters for a line, whose end follows
        if (st->shape.kind == InkShapeKind::Line && len(st->shape.points) == 2) {
            st->shape.points[1] = PointF((float)x, (float)y);
            ScheduleRepaint(win, 0);
        }
        return true;
    }
    if (pt == st->pts.Last()) {
        return true;
    }
    st->pts.Append(pt);
    if (gDrawTool == DrawTool::Pen) {
        PaintLatestSegment(st);
        if (abs(x - st->holdAnchor.x) > kHoldSlackPx || abs(y - st->holdAnchor.y) > kHoldSlackPx) {
            // still moving: restart the hold countdown from here
            st->holdAnchor = pt;
            SetTimer(win->hwndCanvas, kDrawHoldTimerID, kHoldDelayMs, nullptr);
        }
    } else {
        // shapes are rubber-banded: the previous outline has to go
        ScheduleRepaint(win, 0);
    }
    return true;
}

static void SnapHeldStroke(DrawModeState* st) {
    KillTimer(st->win->hwndCanvas, kDrawHoldTimerID);
    if (!st->drawing || st->snapped || gDrawTool != DrawTool::Pen) {
        return;
    }
    Vec<PointF> pts;
    for (Point p : st->pts) {
        pts.Append(PointF((float)p.x, (float)p.y));
    }
    InkShape shape;
    if (!RecognizeInkShape(pts, shape)) {
        return;
    }
    st->snapped = true;
    st->shape = shape;
    logf("draw: held stroke snapped to shape %d\n", (int)shape.kind);
    ScheduleRepaint(st->win, 0);
}

static void PendingTimerTick(DrawModeState* st) {
    DWORD now = GetTickCount();
    bool removed = false;
    for (int i = len(st->pending) - 1; i >= 0; i--) {
        if (now - st->pending[i]->shownAt >= kPendingShowMs) {
            delete st->pending[i];
            st->pending.RemoveAt(i);
            removed = true;
        }
    }
    if (len(st->pending) == 0) {
        KillTimer(st->win->hwndCanvas, kDrawPendingTimerID);
    }
    if (removed) {
        ScheduleRepaint(st->win, 0);
    }
}

void DrawModeOnTimer(MainWindow* win, UINT_PTR timerId) {
    DrawModeState* st = GetState(win);
    if (!st) {
        KillTimer(win->hwndCanvas, timerId);
        return;
    }
    if (timerId == kDrawHoldTimerID) {
        SnapHeldStroke(st);
    } else if (timerId == kDrawPendingTimerID) {
        PendingTimerTick(st);
    }
}

// Turns the stroke into a mark in page coordinates. Returns false when there
// is nothing worth keeping (a shape dragged to nothing).
static bool StrokeToMark(DrawModeState* st, DrawnMark& m) {
    DisplayModel* dm = st->win->AsFixed();
    m.pageNo = st->pageNo;
    m.col = CurrentColor();
    m.widthPt = CurrentWidthPt();
    auto toPage = [&](float x, float y) {
        return dm->CvtFromScreen(Point((int)lroundf(x), (int)lroundf(y)), m.pageNo);
    };

    if (st->snapped) {
        m.type = ShapeToAnnotType(st->shape.kind);
        for (PointF p : st->shape.points) {
            m.pts.Append(toPage(p.x, p.y));
        }
        return true;
    }
    m.type = ToolAnnotType(gDrawTool);
    if (m.type == AnnotationType::Ink) {
        Vec<PointF> pts;
        for (Point p : st->pts) {
            pts.Append(PointF((float)p.x, (float)p.y));
        }
        // input arrives far denser than the line needs; keep it within half a pixel
        SimplifyInkStroke(pts, 0.5f);
        for (PointF p : pts) {
            m.pts.Append(toPage(p.x, p.y));
        }
        if (len(m.pts) == 1) {
            // a tap is a dot: MuPDF needs two points to draw it
            PointF p = m.pts[0];
            m.pts.Append(PointF(p.x + 0.01f, p.y));
        }
        return true;
    }
    Point a = st->pts[0];
    Point b = st->pts.Last();
    if (abs(a.x - b.x) < 3 && abs(a.y - b.y) < 3) {
        return false; // a click, not a shape
    }
    m.arrow = gDrawTool == DrawTool::Arrow;
    m.pts.Append(toPage((float)a.x, (float)a.y));
    m.pts.Append(toPage((float)b.x, (float)b.y));
    return true;
}

static void AfterAnnotationsChanged(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->editAnnotsWindow) {
        UpdateAnnotationsList(tab->editAnnotsWindow);
    }
    InvalidateTouchAnnotations(win);
    MainWindowRerender(win);
    ToolbarUpdateStateForWindow(win, false);
    RepaintBar(GetState(win));
}

static void CommitStroke(DrawModeState* st) {
    MainWindow* win = st->win;
    KillTimer(win->hwndCanvas, kDrawHoldTimerID);
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    auto* m = new DrawnMark();
    bool keep = StrokeToMark(st, *m);
    st->drawing = false;
    st->snapped = false;
    st->pts.Reset();
    if (!keep) {
        delete m;
        ScheduleRepaint(win, 0);
        return;
    }
    DrawnAnnotArgs args;
    args.annotType = m->type;
    args.points = m->pts;
    args.arrowEnd = m->arrow;
    args.col = MkPdfColor(GetRValue(m->col), GetGValue(m->col), GetBValue(m->col));
    args.borderWidth = m->widthPt;
    EngineBase* engine = CurrentEngine(win);
    Annotation* annot = EngineMupdfCreateDrawnAnnotation(engine, m->pageNo, args);
    if (!annot) {
        logf("draw: creating a type %d annotation on page %d failed\n", (int)m->type, m->pageNo);
        delete m;
        ScheduleRepaint(win, 0);
        return;
    }
    logf("draw: added type %d annotation on page %d (%d points)\n", (int)m->type, m->pageNo, len(m->pts));
    DropStaleUndo(st);
    st->undo.Append(annot);
    // keep painting the mark until the page re-renders with its annotation
    m->annot = annot;
    m->shownAt = GetTickCount();
    st->pending.Append(m);
    SetTimer(win->hwndCanvas, kDrawPendingTimerID, kPendingShowMs / 3, nullptr);
    AfterAnnotationsChanged(win);
}

bool DrawModeOnMouseUp(MainWindow* win, int x, int y) {
    DrawModeState* st = GetState(win);
    if (!st || !st->drawing) {
        return false;
    }
    DrawModeOnMouseMove(win, x, y);
    CommitStroke(st);
    return true;
}

bool DrawModeSetCursor(MainWindow* win, int x, int y) {
    DrawModeState* st = GetState(win);
    if (!st || !st->on || !CanDrawOn(win)) {
        return false;
    }
    if (!st->drawing && win->AsFixed()->GetPageNoByPoint(Point(x, y)) <= 0) {
        return false;
    }
    SetCursorCached(IDC_CROSS);
    return true;
}

// ----- undo

bool DrawModeCanUndo(MainWindow* win) {
    DrawModeState* st = GetState(win);
    if (!st || !st->on) {
        return false;
    }
    return len(st->undo) > 0 && st->undoEngine == CurrentEngine(win);
}

// Removes the newest mark made in this draw session that still exists (one
// may already have been deleted from the annotations list).
void DrawModeUndo(MainWindow* win) {
    DrawModeState* st = GetState(win);
    if (!st || !st->on) {
        return;
    }
    if (st->drawing) {
        DrawModeCancelStroke(win);
        return;
    }
    DropStaleUndo(st);
    EngineBase* engine = CurrentEngine(win);
    WindowTab* tab = win->CurrentTab();
    while (len(st->undo) > 0) {
        Annotation* annot = st->undo.Pop();
        if (!tab || !EngineHasAnnotation(engine, annot)) {
            continue;
        }
        for (int i = len(st->pending) - 1; i >= 0; i--) {
            if (st->pending[i]->annot == annot) {
                delete st->pending[i];
                st->pending.RemoveAt(i);
            }
        }
        DeleteAnnotationAndUpdateUI(tab, annot);
        logf("draw: undid a mark, %d left\n", len(st->undo));
        AfterAnnotationsChanged(win);
        return;
    }
    RepaintBar(st);
}
