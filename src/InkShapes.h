/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// what a freehand stroke turns into when the pen is held still at its end
enum class InkShapeKind {
    None,
    Line,    // points: start, end
    Rect,    // points: two opposite corners (axis-aligned)
    Ellipse, // points: two opposite corners of its bounding box
    Polygon, // points: the vertices (a triangle or a tilted quadrilateral)
};

struct InkShape {
    InkShapeKind kind = InkShapeKind::None;
    Vec<PointF> points;
};

bool RecognizeInkShape(const Vec<PointF>& stroke, InkShape& out);
void SimplifyInkStroke(Vec<PointF>& pts, float tolerance);
