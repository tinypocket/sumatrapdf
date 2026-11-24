/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "InkShapes.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// Hand-drawn input is never exact: every generated point is nudged by a small,
// repeatable wobble so the tests exercise the tolerances, not perfect geometry.
static u32 gWobbleSeed = 1;
static float Wobble(float amount) {
    gWobbleSeed = gWobbleSeed * 1103515245 + 12345;
    float r = (float)((gWobbleSeed >> 16) & 0x7fff) / 32767.0f; // 0..1
    return (r * 2 - 1) * amount;
}

static void AddSegment(Vec<PointF>& pts, PointF a, PointF b, int steps, float wobble) {
    for (int i = 0; i < steps; i++) {
        float t = (float)i / steps;
        pts.Append(PointF(a.x + (b.x - a.x) * t + Wobble(wobble), a.y + (b.y - a.y) * t + Wobble(wobble)));
    }
}

static Vec<PointF> Polyline(const PointF* corners, int n, bool close, float wobble) {
    Vec<PointF> pts;
    int segs = close ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        AddSegment(pts, corners[i], corners[(i + 1) % n], 20, wobble);
    }
    PointF end = close ? corners[0] : corners[n - 1];
    pts.Append(PointF(end.x + Wobble(wobble), end.y + Wobble(wobble)));
    return pts;
}

static Vec<PointF> EllipseStroke(float cx, float cy, float rx, float ry, float sweepDeg, float wobble) {
    Vec<PointF> pts;
    int steps = 80;
    for (int i = 0; i <= steps; i++) {
        float a = (sweepDeg * 3.14159265f / 180.0f) * i / steps - 1.3f; // start off-axis, like a hand does
        pts.Append(PointF(cx + rx * cosf(a) + Wobble(wobble), cy + ry * sinf(a) + Wobble(wobble)));
    }
    return pts;
}

static InkShapeKind ShapeKindOf(const Vec<PointF>& pts) {
    InkShape shape;
    RecognizeInkShape(pts, shape);
    return shape.kind;
}

static bool Near(float a, float b, float slack) {
    return fabsf(a - b) <= slack;
}

static void TestLines() {
    PointF diag[] = {{10, 10}, {210, 130}};
    InkShape s;
    utassert(RecognizeInkShape(Polyline(diag, 2, false, 2), s));
    utassert(s.kind == InkShapeKind::Line);
    utassert(len(s.points) == 2);

    // almost level: snapped to exactly horizontal
    PointF level[] = {{20, 100}, {300, 108}};
    utassert(RecognizeInkShape(Polyline(level, 2, false, 2), s));
    utassert(s.kind == InkShapeKind::Line);
    utassert(s.points[0].y == s.points[1].y);

    // a stroke that bends back is not a line
    PointF vee[] = {{10, 10}, {100, 150}, {190, 10}};
    utassert(ShapeKindOf(Polyline(vee, 3, false, 1)) == InkShapeKind::None);
}

static void TestRects() {
    PointF rect[] = {{50, 40}, {300, 40}, {300, 200}, {50, 200}};
    InkShape s;
    utassert(RecognizeInkShape(Polyline(rect, 4, true, 3), s));
    utassert(s.kind == InkShapeKind::Rect);
    utassert(Near(s.points[0].x, 50, 5) && Near(s.points[0].y, 40, 5));
    utassert(Near(s.points[1].x, 300, 5) && Near(s.points[1].y, 200, 5));

    // a square too
    PointF square[] = {{0, 0}, {120, 0}, {120, 120}, {0, 120}};
    utassert(ShapeKindOf(Polyline(square, 4, true, 3)) == InkShapeKind::Rect);
}

static void TestEllipses() {
    InkShape s;
    utassert(RecognizeInkShape(EllipseStroke(200, 150, 120, 60, 360, 3), s));
    utassert(s.kind == InkShapeKind::Ellipse);
    utassert(Near(s.points[1].x - s.points[0].x, 240, 12));
    utassert(Near(s.points[1].y - s.points[0].y, 120, 12));

    // nearly round becomes an exact circle
    utassert(RecognizeInkShape(EllipseStroke(100, 100, 80, 76, 360, 2), s));
    utassert(s.kind == InkShapeKind::Ellipse);
    utassert(Near(s.points[1].x - s.points[0].x, s.points[1].y - s.points[0].y, 0.01f));

    // a hand rarely closes a circle exactly: a small gap or overlap is fine
    utassert(ShapeKindOf(EllipseStroke(100, 100, 80, 80, 340, 2)) == InkShapeKind::Ellipse);
    utassert(ShapeKindOf(EllipseStroke(100, 100, 80, 80, 380, 2)) == InkShapeKind::Ellipse);

    // half a circle is an arc, not a shape
    utassert(ShapeKindOf(EllipseStroke(100, 100, 80, 80, 180, 2)) == InkShapeKind::None);
}

static void TestPolygons() {
    PointF tri[] = {{100, 20}, {200, 200}, {0, 200}};
    InkShape s;
    utassert(RecognizeInkShape(Polyline(tri, 3, true, 3), s));
    utassert(s.kind == InkShapeKind::Polygon);
    utassert(len(s.points) == 3);

    // a diamond is a tilted quadrilateral, not a box
    PointF diamond[] = {{150, 0}, {300, 150}, {150, 300}, {0, 150}};
    utassert(RecognizeInkShape(Polyline(diamond, 4, true, 3), s));
    utassert(s.kind == InkShapeKind::Polygon);
    utassert(len(s.points) == 4);
}

static void TestNotShapes() {
    // a scribble stays freehand
    Vec<PointF> scribble;
    for (int i = 0; i < 60; i++) {
        scribble.Append(PointF((float)i * 5, (i % 2) ? 0.0f : 40.0f));
    }
    utassert(ShapeKindOf(scribble) == InkShapeKind::None);

    // a dot
    PointF dot[] = {{10, 10}, {12, 11}};
    utassert(ShapeKindOf(Polyline(dot, 2, false, 0.5f)) == InkShapeKind::None);
}

static void TestSimplify() {
    PointF level[] = {{0, 0}, {400, 0}};
    Vec<PointF> pts = Polyline(level, 2, false, 0.2f);
    int before = len(pts);
    SimplifyInkStroke(pts, 1.0f);
    utassert(len(pts) >= 2 && len(pts) < before / 4);
    utassert(pts[0].x == 0 || Near(pts[0].x, 0, 0.3f));

    PointF corner[] = {{0, 0}, {100, 0}, {100, 100}};
    pts = Polyline(corner, 3, false, 0);
    SimplifyInkStroke(pts, 1.0f);
    utassert(len(pts) == 3); // the corner survives
}

void InkShapes_UnitTests() {
    TestLines();
    TestRects();
    TestEllipses();
    TestPolygons();
    TestNotShapes();
    TestSimplify();
}
