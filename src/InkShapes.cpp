/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Draw-and-hold shape recognition: a rough hand-drawn line, rectangle,
// ellipse, triangle or quadrilateral is turned into the clean shape. This is
// plain geometry with no Windows or engine dependencies, so test_util covers it.
// Points are in whatever space the caller draws in (screen pixels in draw
// mode); thresholds are relative to the stroke's size so zoom doesn't matter.

#include "base/Base.h"

#include "InkShapes.h"

static float Dist(PointF a, PointF b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

// distance from p to the segment a-b
static float DistToSegment(PointF p, PointF a, PointF b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    if (len2 <= 0) {
        return Dist(p, a);
    }
    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return Dist(p, PointF(a.x + t * dx, a.y + t * dy));
}

// Ramer-Douglas-Peucker over pts[from..to], marking the points to keep
static void RdpMark(const Vec<PointF>& pts, int from, int to, float tolerance, Vec<bool>& keep) {
    if (to <= from + 1) {
        return;
    }
    float maxDist = -1;
    int maxIdx = -1;
    for (int i = from + 1; i < to; i++) {
        float d = DistToSegment(pts[i], pts[from], pts[to]);
        if (d > maxDist) {
            maxDist = d;
            maxIdx = i;
        }
    }
    if (maxDist > tolerance) {
        keep[maxIdx] = true;
        RdpMark(pts, from, maxIdx, tolerance, keep);
        RdpMark(pts, maxIdx, to, tolerance, keep);
    }
}

// Drops points that don't change the stroke's shape by more than tolerance,
// keeping the first and last. Freehand input arrives at ~100+ points a second;
// most of them are redundant in the saved annotation.
void SimplifyInkStroke(Vec<PointF>& pts, float tolerance) {
    int n = len(pts);
    if (n < 3) {
        return;
    }
    Vec<bool> keep;
    for (int i = 0; i < n; i++) {
        keep.Append(i == 0 || i == n - 1);
    }
    RdpMark(pts, 0, n - 1, tolerance, keep);
    Vec<PointF> res;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            res.Append(pts[i]);
        }
    }
    pts = res;
}

struct StrokeBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float Dx() const { return x1 - x0; }
    float Dy() const { return y1 - y0; }
};

static StrokeBox BoxOf(const Vec<PointF>& pts) {
    StrokeBox b{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
    for (PointF p : pts) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

// a near-horizontal or near-vertical line is meant to be exactly that
static PointF SnapLineEnd(PointF a, PointF b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float angle = atan2f(fabsf(dy), fabsf(dx)) * 180.0f / 3.14159265f;
    if (angle < 5) {
        return PointF(b.x, a.y);
    }
    if (angle > 85) {
        return PointF(a.x, b.y);
    }
    return b;
}

// mean distance of the points to the closed polygon's edges
static float PolygonFitError(const Vec<PointF>& pts, const Vec<PointF>& poly) {
    int k = len(poly);
    float sum = 0;
    for (PointF p : pts) {
        float best = FLT_MAX;
        for (int i = 0; i < k; i++) {
            best = std::min(best, DistToSegment(p, poly[i], poly[(i + 1) % k]));
        }
        sum += best;
    }
    return sum / len(pts);
}

// The corners of a closed stroke: RDP from the start point and from the point
// farthest from it, then drop vertices where the outline barely turns (the start
// usually lies mid-edge).
static void ClosedCorners(const Vec<PointF>& pts, float tolerance, Vec<PointF>& corners) {
    int n = len(pts);
    int farIdx = 0;
    float farIdxDist = -1;
    for (int i = 1; i < n; i++) {
        float d = Dist(pts[0], pts[i]);
        if (d > farIdxDist) {
            farIdxDist = d;
            farIdx = i;
        }
    }
    Vec<PointF> loop = pts;
    loop.Append(pts[0]);
    Vec<bool> keep;
    for (int i = 0; i < len(loop); i++) {
        keep.Append(i == 0 || i == farIdx || i == len(loop) - 1);
    }
    RdpMark(loop, 0, farIdx, tolerance, keep);
    RdpMark(loop, farIdx, len(loop) - 1, tolerance, keep);
    Vec<PointF> v;
    for (int i = 0; i < len(loop) - 1; i++) {
        if (keep[i]) {
            v.Append(loop[i]);
        }
    }
    // merge corners that the hand drew twice (overshoot at a corner)
    bool changed = true;
    while (changed && len(v) > 3) {
        changed = false;
        for (int i = 0; i < len(v); i++) {
            PointF prev = v[(i + len(v) - 1) % len(v)];
            PointF cur = v[i];
            PointF next = v[(i + 1) % len(v)];
            float a1 = atan2f(cur.y - prev.y, cur.x - prev.x);
            float a2 = atan2f(next.y - cur.y, next.x - cur.x);
            float turn = fabsf(a2 - a1);
            if (turn > 3.14159265f) {
                turn = 2 * 3.14159265f - turn;
            }
            bool tooClose = Dist(cur, prev) < tolerance;
            if (turn < 0.45f || tooClose) { // ~25 degrees
                v.RemoveAt(i);
                changed = true;
                break;
            }
        }
    }
    corners = v;
}

// Returns true and fills out when the stroke is close enough to a clean shape.
// An open stroke can only be a line; anything else must come back near its start.
bool RecognizeInkShape(const Vec<PointF>& stroke, InkShape& out) {
    out.kind = InkShapeKind::None;
    out.points.Reset();
    int n = len(stroke);
    if (n < 4) {
        return false;
    }
    StrokeBox box = BoxOf(stroke);
    float diag = sqrtf(box.Dx() * box.Dx() + box.Dy() * box.Dy());
    if (diag < 12) {
        return false; // a dot or a tiny scribble
    }
    float pathLen = 0;
    for (int i = 1; i < n; i++) {
        pathLen += Dist(stroke[i - 1], stroke[i]);
    }
    PointF first = stroke[0];
    PointF last = stroke[n - 1];
    float chord = Dist(first, last);

    // a line: the path is barely longer than its chord and hugs it
    if (chord / pathLen >= 0.9f) {
        float maxDev = 0;
        for (PointF p : stroke) {
            maxDev = std::max(maxDev, DistToSegment(p, first, last));
        }
        if (maxDev <= std::max(4.0f, 0.07f * chord)) {
            out.kind = InkShapeKind::Line;
            out.points.Append(first);
            out.points.Append(SnapLineEnd(first, last));
            return true;
        }
    }

    // everything else is a closed outline
    if (chord > 0.25f * diag) {
        return false;
    }
    float w = box.Dx();
    float h = box.Dy();
    if (w < 8 || h < 8) {
        return false;
    }
    float halfSize = (w + h) / 4; // a shape's typical "radius"

    // how well an axis-aligned ellipse inscribed in the box fits
    float cx = (box.x0 + box.x1) / 2;
    float cy = (box.y0 + box.y1) / 2;
    float ellipseErr = 0;
    for (PointF p : stroke) {
        float nx = (p.x - cx) / (w / 2);
        float ny = (p.y - cy) / (h / 2);
        ellipseErr += fabsf(sqrtf(nx * nx + ny * ny) - 1);
    }
    ellipseErr /= n;

    // how well the box itself fits
    float rectErr = 0;
    for (PointF p : stroke) {
        float d = std::min(std::min(fabsf(p.x - box.x0), fabsf(p.x - box.x1)),
                           std::min(fabsf(p.y - box.y0), fabsf(p.y - box.y1)));
        rectErr += d;
    }
    rectErr = rectErr / n / halfSize;

    Vec<PointF> corners;
    ClosedCorners(stroke, 0.08f * diag, corners);
    int k = len(corners);
    float polyErr = FLT_MAX;
    if (k == 3 || k == 4) {
        polyErr = PolygonFitError(stroke, corners) / halfSize;
    }

    auto setBox = [&](InkShapeKind kind) {
        out.kind = kind;
        out.points.Append(PointF(box.x0, box.y0));
        out.points.Append(PointF(box.x1, box.y1));
    };

    // the box is taken from the extremes, so wobble inflates rectErr more than
    // polyErr: only a clearly better tilted fit (a diamond) beats the box
    if (rectErr < 0.1f && rectErr <= ellipseErr && !(k == 4 && polyErr * 2.5f < rectErr)) {
        setBox(InkShapeKind::Rect);
        return true;
    }
    if (k == 3 && polyErr < 0.1f) {
        out.kind = InkShapeKind::Polygon;
        out.points = corners;
        return true;
    }
    if (ellipseErr < 0.12f && ellipseErr <= polyErr) {
        // nearly round is meant to be a circle
        if (std::max(w, h) / std::min(w, h) < 1.12f) {
            float r = (w + h) / 4;
            out.kind = InkShapeKind::Ellipse;
            out.points.Append(PointF(cx - r, cy - r));
            out.points.Append(PointF(cx + r, cy + r));
            return true;
        }
        setBox(InkShapeKind::Ellipse);
        return true;
    }
    if (k == 4 && polyErr < 0.08f) {
        out.kind = InkShapeKind::Polygon;
        out.points = corners;
        return true;
    }
    return false;
}
