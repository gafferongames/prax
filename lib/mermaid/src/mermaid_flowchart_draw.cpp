// Flowcharts as drawn (mermaid_flowchart_draw.h). The labels are shaped first, so that the nodes can be sized around
// them. lib/graph then lays out the nodes, subgraphs and links, and the drawing clips each link to the outline of its
// nodes' shapes and adds its marks.

#include "mermaid/src/mermaid_flowchart_draw.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/raw_array.h"
#include "pr/sort.h"

namespace pr_impl {

namespace {

// Mermaid's flowchart measurements.
constexpr float kFontSize = 16;
constexpr float kTitleFontSize = 18;
constexpr float kTitleGap = 10;             // between the title and the diagram
constexpr float kNodePadding = 15;          // the padding that each shape's size starts from: flowchart.padding
constexpr float kMinNodeWidth = 120;        // the least width of a node's label: flowchart.minNodeWidth
constexpr float kClusterPadding = 12;       // between a subgraph's box and its contents
constexpr float kEdgeLabelPadding = 4;
constexpr float kDiagramPadding = 8;
constexpr float kMarkerSize = 8;            // an arrow's length and width, a circle's diameter and a cross's width
constexpr float kLineWidth = 1;
constexpr float kThickWidth = 3.5f;
constexpr float kLineHeight = 1.5f;         // a label's line height, by its font size, as Mermaid's labels set
constexpr float kWrapWidth = 120;           // the width at which labels' lines wrap: wrappingWidth
constexpr float kDotted[2] = {3, 3};

// How a shape paints its outline.
enum {
    ShapePaint_Normal,                      // filled and stroked
    ShapePaint_None,                        // neither: the outline only clips links
    ShapePaint_Fill,                        // filled, and not stroked
    ShapePaint_Solid,                       // filled and stroked in the stroke's color
};
typedef uint8_t ShapePaint;

// A node's shape, as built around its label. The paths are in the node's own space, whose origin is the center of the
// node's box: the outline, which is painted as `paint` says and which links are clipped to, the details, which are
// stroked, and the marks, which are filled in the stroke's color.
struct Shape {
    PrVec2 extent;                          // the box that the layout places
    PrVec2 label;                           // the center of the label, from the center of the box
    bool hasLabel;
    ShapePaint paint;
};

void addPolygon(PrPath* path, const PrVec2* points, uint32_t count) {
    prPathMoveTo(path, points[0]);
    for (uint32_t i = 1; i < count; ++i)
        prPathLineTo(path, points[i]);
    prPathClose(path);
}

void addLine(PrPath* path, PrVec2 from, PrVec2 to) {
    prPathMoveTo(path, from);
    prPathLineTo(path, to);
}

// Adds a sine wave from the current point, `from`, across `dx`, as mermaid.js draws the wavy edges of documents and
// tape: `waves` periods of `amplitude`, sampled at 50 steps. Returns its last point.
PrVec2 addWave(PrPath* path, PrVec2 from, float dx, float amplitude, float waves) {
    constexpr uint32_t kSteps = 50;
    PrVec2 point = from;
    for (uint32_t i = 1; i <= kSteps; ++i) {
        float t = float(i) / float(kSteps);
        point = PrVec2{from.x + t * dx, from.y + amplitude * prF32Sin(2 * kPrF32Pi * waves * t)};
        prPathLineTo(path, point);
    }
    return point;
}

// Adds an elliptical arc from the current point, `from`, to `to`, as an SVG path's arc command draws it without
// rotation: radii too small to reach are scaled up, and the flags pick one of the four arcs.
void addSvgArc(PrPath* path, PrVec2 from, PrVec2 to, PrVec2 radii, bool large, bool sweep) {
    PrVec2 half = (from - to) * 0.5f;
    float rx = prF32Abs(radii.x);
    float ry = prF32Abs(radii.y);
    if (rx == 0 || ry == 0 || (half.x == 0 && half.y == 0)) {
        prPathLineTo(path, to);
        return;
    }
    float lambda = (half.x * half.x) / (rx * rx) + (half.y * half.y) / (ry * ry);
    if (lambda > 1) {
        rx *= prF32Sqrt(lambda);
        ry *= prF32Sqrt(lambda);
    }
    float numerator = rx * rx * ry * ry - rx * rx * half.y * half.y - ry * ry * half.x * half.x;
    float denominator = rx * rx * half.y * half.y + ry * ry * half.x * half.x;
    float coefficient = prF32Sqrt(prF32Max(0.0f, numerator / denominator));
    if (large == sweep)
        coefficient = -coefficient;
    PrVec2 offset{coefficient * rx * half.y / ry, -coefficient * ry * half.x / rx};
    PrVec2 center = offset + (from + to) * 0.5f;
    float start = prF32Atan2((half.y - offset.y) / ry, (half.x - offset.x) / rx);
    float end = prF32Atan2((-half.y - offset.y) / ry, (-half.x - offset.x) / rx);
    float delta = end - start;
    if (!sweep && delta > 0)
        delta -= 2 * kPrF32Pi;
    else if (sweep && delta < 0)
        delta += 2 * kPrF32Pi;
    prPathEllipticArcTo(path, center, PrVec2{rx, ry}, 0, start, delta);
}

// Adds a curly brace whose ends are at `x`, `half` + `r` above and below `y`, and whose point is 2 `r` to the left of
// them, or to the right if `side` is -1.
void addBrace(PrPath* path, float x, float y, float half, float r, float side) {
    auto arc = [&](PrVec2 center, float start, float sweep) {
        if (side < 0) {
            start = kPrF32Pi - start;
            sweep = -sweep;
        }
        prPathEllipticArcTo(path, center, PrVec2{r, r}, 0, start, sweep);
    };
    const float quarter = kPrF32Pi * 0.5f;
    prPathMoveTo(path, PrVec2{x, y + half + r});
    arc(PrVec2{x, y + half}, quarter, quarter);
    prPathLineTo(path, PrVec2{x - side * r, y + r});
    arc(PrVec2{x - side * 2 * r, y + r}, 0, -quarter);
    arc(PrVec2{x - side * 2 * r, y - r}, quarter, -quarter);
    prPathLineTo(path, PrVec2{x - side * r, y - half});
    arc(PrVec2{x, y - half}, kPrF32Pi, quarter);
}

// The radius of the circle that a circle's label fits in, before padding.
float circleRadius(PrVec2 text) {
    return prF32Sqrt(text.x * text.x + text.y * text.y) * 0.5f;
}

// Builds a node's shape around a label whose extent is `text`, as mermaid.js's classic shapes build themselves, and
// returns its measurements. `across` is true when the flow runs across the page, which turns a fork. Every label but a
// circle's is at least kMinNodeWidth wide. Each shape pads its label in its own way, from flowchart.padding.
Shape buildShape(MermaidShape shape, PrVec2 text, bool across, PrPath* outline, PrPath* details, PrPath* marks) {
    const float p = kNodePadding;
    const float quarter = kPrF32Pi * 0.5f;
    if (shape != MermaidShape_Circle && shape != MermaidShape_DoubleCircle && text.x < kMinNodeWidth)
        text.x = kMinNodeWidth;
    Shape out{.hasLabel = true, .paint = ShapePaint_Normal};
    // The label's extent with the padding once in each direction, which most shapes start from.
    float w = text.x + p;
    float h = text.y + p;
    auto box = [&](float width, float height) {
        out.extent = PrVec2{width, height};
        return PrRect{-width * 0.5f, -height * 0.5f, width * 0.5f, height * 0.5f};
    };
    switch (shape) {
        case MermaidShape_Rect:
        case MermaidShape_DataStore: {
            PrRect r = box(text.x + 4 * p, text.y + 2 * p);
            prPathAddRect(outline, r);
            // A data store is open at its sides.
            if (shape == MermaidShape_DataStore) {
                out.paint = ShapePaint_Fill;
                addLine(details, PrVec2{r.left, r.top}, PrVec2{r.right, r.top});
                addLine(details, PrVec2{r.left, r.bottom}, PrVec2{r.right, r.bottom});
            }
            break;
        }
        case MermaidShape_Round:
            prPathAddCornerRect(outline, prCornerRectRound(box(text.x + 2 * p, text.y + 2 * p), 5));
            break;
        case MermaidShape_Stadium: {
            PrRect r = box(w + h * 0.25f, h);
            prPathAddCornerRect(outline, prCornerRectRound(r, h * 0.5f));
            break;
        }
        case MermaidShape_Subroutine: {
            PrRect r = box(w + 16, h);
            prPathAddRect(outline, r);
            addLine(details, PrVec2{r.left + 8, r.top}, PrVec2{r.left + 8, r.bottom});
            addLine(details, PrVec2{r.right - 8, r.top}, PrVec2{r.right - 8, r.bottom});
            break;
        }
        case MermaidShape_Cylinder:
        case MermaidShape_LinedCylinder: {
            // A lined cylinder pads its label twice in each direction, and adds a line below its top.
            bool lined = shape == MermaidShape_LinedCylinder;
            float width = lined ? text.x + 2 * p : w;
            float rx = width * 0.5f;
            float ry = rx / (2.5f + width / 50);
            float body = lined ? text.y + 2 * p + ry : h + ry;
            PrRect r = box(width, body + 2 * ry);
            prPathMoveTo(outline, PrVec2{r.left, r.top + ry});
            prPathEllipticArcTo(outline, PrVec2{0, r.top + ry}, PrVec2{rx, ry}, 0, kPrF32Pi, kPrF32Pi);
            prPathLineTo(outline, PrVec2{r.right, r.bottom - ry});
            prPathEllipticArcTo(outline, PrVec2{0, r.bottom - ry}, PrVec2{rx, ry}, 0, 0, kPrF32Pi);
            prPathClose(outline);
            prPathMoveTo(details, PrVec2{r.left, r.top + ry});
            prPathEllipticArcTo(details, PrVec2{0, r.top + ry}, PrVec2{rx, ry}, 0, kPrF32Pi, -kPrF32Pi);
            if (lined) {
                float line = r.top + ry + body * 0.1f;
                prPathMoveTo(details, PrVec2{r.left, line});
                prPathEllipticArcTo(details, PrVec2{0, line}, PrVec2{rx, ry}, 0, kPrF32Pi, -kPrF32Pi);
            }
            // The label sits below the middle, clear of the top.
            out.label.y = lined ? ry : p / 1.5f;
            break;
        }
        case MermaidShape_HorizontalCylinder: {
            float height = text.y + p * 0.5f;
            float ry = height * 0.5f;
            float rx = ry / (2.5f + height / 50);
            float body = text.x + rx + p * 0.5f;
            PrRect r = box(body + 2 * rx, height);
            prPathMoveTo(outline, PrVec2{r.left + rx, r.bottom});
            prPathEllipticArcTo(outline, PrVec2{r.left + rx, 0}, PrVec2{rx, ry}, 0, quarter, kPrF32Pi);
            prPathLineTo(outline, PrVec2{r.right - rx, r.top});
            prPathEllipticArcTo(outline, PrVec2{r.right - rx, 0}, PrVec2{rx, ry}, 0, -quarter, kPrF32Pi);
            prPathClose(outline);
            prPathMoveTo(details, PrVec2{r.right - rx, r.top});
            prPathEllipticArcTo(details, PrVec2{r.right - rx, 0}, PrVec2{rx, ry}, 0, -quarter, -kPrF32Pi);
            out.label.x = -rx;
            break;
        }
        case MermaidShape_Circle: {
            float radius = circleRadius(text) + p * 0.5f;
            box(2 * radius, 2 * radius);
            prPathAddEllipse(outline, PrVec2{}, PrVec2{radius, radius}, 0);
            break;
        }
        case MermaidShape_DoubleCircle: {
            float radius = circleRadius(text) + p + 5;
            box(2 * radius, 2 * radius);
            prPathAddEllipse(outline, PrVec2{}, PrVec2{radius, radius}, 0);
            prPathAddEllipse(details, PrVec2{}, PrVec2{radius - 5, radius - 5}, 0);
            break;
        }
        case MermaidShape_Asymmetric: {
            // The notch's depth widens the box, and the label sits right of the middle, clear of it.
            PrRect r = box(w + h * 0.25f, h);
            PrVec2 points[] = {
                {r.left, r.top}, {r.right, r.top}, {r.right, r.bottom}, {r.left, r.bottom},
                {r.left + h * 0.25f, 0},
            };
            addPolygon(outline, points, countOf(points));
            out.label.x = h * 0.125f;
            break;
        }
        case MermaidShape_Diamond: {
            PrRect r = box(w + h, w + h);
            PrVec2 points[] = {{0, r.top}, {r.right, 0}, {0, r.bottom}, {r.left, 0}};
            addPolygon(outline, points, countOf(points));
            break;
        }
        case MermaidShape_Hexagon: {
            PrRect r = box(w + h * 0.5f, h);
            float m = h * 0.25f;
            PrVec2 points[] = {
                {r.left + m, r.top}, {r.right - m, r.top}, {r.right, 0}, {r.right - m, r.bottom},
                {r.left + m, r.bottom}, {r.left, 0},
            };
            addPolygon(outline, points, countOf(points));
            break;
        }
        case MermaidShape_LeanRight:
        case MermaidShape_LeanLeft:
        case MermaidShape_Trapezoid:
        case MermaidShape_InvTrapezoid: {
            // An inverted trapezoid pads its label twice in each direction.
            bool inverted = shape == MermaidShape_InvTrapezoid;
            float height = inverted ? text.y + 2 * p : h;
            PrRect r = box((inverted ? text.x + 2 * p : w) + height, height);
            float s = height * 0.5f;
            // The corners that are inset. A parallelogram insets opposite corners, and a trapezoid insets the corners
            // of its narrow side.
            bool topLeft = shape == MermaidShape_LeanRight || shape == MermaidShape_Trapezoid;
            bool topRight = shape == MermaidShape_LeanLeft || shape == MermaidShape_Trapezoid;
            bool bottomRight = shape == MermaidShape_LeanRight || inverted;
            bool bottomLeft = shape == MermaidShape_LeanLeft || inverted;
            PrVec2 points[] = {
                {r.left + (topLeft ? s : 0), r.top},
                {r.right - (topRight ? s : 0), r.top},
                {r.right - (bottomRight ? s : 0), r.bottom},
                {r.left + (bottomLeft ? s : 0), r.bottom},
            };
            addPolygon(outline, points, countOf(points));
            break;
        }
        case MermaidShape_Text:
            prPathAddRect(outline, box(w, h));
            out.paint = ShapePaint_None;
            break;
        case MermaidShape_NotchedRect: {
            constexpr float kNotch = 12;
            PrRect r = box(w + kNotch, h);
            PrVec2 points[] = {
                {r.left + kNotch, r.top}, {r.right, r.top}, {r.right, r.bottom}, {r.left, r.bottom},
                {r.left, r.top + kNotch},
            };
            addPolygon(outline, points, countOf(points));
            break;
        }
        case MermaidShape_LinedRect: {
            constexpr float kLine = 8;
            PrRect r = box(text.x + 2 * p + 2 * kLine, text.y + 2 * p);
            prPathAddRect(outline, r);
            addLine(details, PrVec2{r.left + kLine, r.top}, PrVec2{r.left + kLine, r.bottom});
            out.label.x = kLine * 0.5f;
            break;
        }
        case MermaidShape_DividedRect: {
            float band = h * 0.2f;
            PrRect r = box(w, h + band);
            prPathAddRect(outline, r);
            addLine(details, PrVec2{r.left, r.top + band}, PrVec2{r.right, r.top + band});
            out.label.y = band * 0.5f;
            break;
        }
        case MermaidShape_SlopedRect: {
            // The top slopes up to the right, by half the height of the rest.
            float height = text.y + 2 * p;
            PrRect r = box(text.x + 2 * p, height * 1.5f);
            PrVec2 points[] = {
                {r.left, r.top + height * 0.5f}, {r.left, r.bottom}, {r.right, r.bottom},
                {r.right, r.top},
            };
            addPolygon(outline, points, countOf(points));
            out.label.y = height * 0.25f;
            break;
        }
        case MermaidShape_StackedRect: {
            // Three rectangles, each 5 px up and to the right of the one in front of it.
            constexpr float kStep = 5;
            PrRect r = box(text.x + 2 * p + 2 * kStep, text.y + 2 * p + 2 * kStep);
            float front = r.right - 2 * kStep;
            PrVec2 points[] = {
                {r.left, r.top + 2 * kStep}, {r.left, r.bottom}, {front, r.bottom}, {front, r.bottom - kStep},
                {front + kStep, r.bottom - kStep}, {front + kStep, r.bottom - 2 * kStep},
                {r.right, r.bottom - 2 * kStep}, {r.right, r.top}, {r.left + 2 * kStep, r.top},
                {r.left + 2 * kStep, r.top + kStep}, {r.left + kStep, r.top + kStep}, {r.left + kStep, r.top + 2 * kStep},
            };
            addPolygon(outline, points, countOf(points));
            PrVec2 edges[] = {
                {r.left + kStep, r.top + 2 * kStep}, {front, r.top + 2 * kStep}, {front, r.bottom - kStep},
                {front + kStep, r.bottom - kStep}, {front + kStep, r.top + kStep}, {r.left + kStep, r.top + kStep},
            };
            addPolygon(details, edges, countOf(edges));
            out.label = PrVec2{-kStep, kStep};
            break;
        }
        case MermaidShape_TaggedRect: {
            // A small triangle folds over the bottom right corner.
            float height = text.y + 2 * p;
            float tag = height * 0.2f;
            PrRect r = box(text.x + 2 * p + tag, height);
            prPathAddRect(outline, r);
            PrVec2 points[] = {{r.right - tag, r.bottom}, {r.right, r.bottom}, {r.right, r.bottom - tag}};
            addPolygon(details, points, countOf(points));
            break;
        }
        case MermaidShape_BowTieRect: {
            // Both sides curve to the left: the left one out of the box, and the right one into it.
            float ry = h * 0.5f;
            float rx = ry / (2.5f + h / 50);
            PrRect r = box(text.x + 2 * p + rx, h);
            prPathMoveTo(outline, PrVec2{r.right, r.top});
            prPathLineTo(outline, PrVec2{r.left + rx, r.top});
            prPathEllipticArcTo(outline, PrVec2{r.left + rx, 0}, PrVec2{rx, ry}, 0, -quarter, -kPrF32Pi);
            prPathLineTo(outline, PrVec2{r.right, r.bottom});
            prPathEllipticArcTo(outline, PrVec2{r.right, 0}, PrVec2{rx, ry}, 0, quarter, kPrF32Pi);
            prPathClose(outline);
            break;
        }
        case MermaidShape_WindowPane: {
            constexpr float kPane = 10;
            PrRect r = box(text.x + 2 * p + kPane, text.y + 2 * p + kPane);
            prPathAddRect(outline, r);
            addLine(details, PrVec2{r.left, r.top + kPane}, PrVec2{r.right, r.top + kPane});
            addLine(details, PrVec2{r.left + kPane, r.top}, PrVec2{r.left + kPane, r.bottom});
            out.label = PrVec2{kPane * 0.5f, kPane * 0.5f};
            break;
        }
        case MermaidShape_NotchedPentagon: {
            PrRect r = box(text.x + 2 * p, text.y + 2 * p);
            PrVec2 points[] = {
                {r.left * 0.8f, r.top}, {r.right * 0.8f, r.top}, {r.right, r.top * 0.6f},
                {r.right, r.bottom}, {r.left, r.bottom}, {r.left, r.top * 0.6f},
            };
            addPolygon(outline, points, countOf(points));
            break;
        }
        case MermaidShape_CurvedTrapezoid: {
            // A point at the left, and a half circle at the right.
            float height = prF32Max(5, text.y + 2 * p);
            float radius = height * 0.5f;
            float inset = radius - prF32Sqrt(prF32Max(0.0f, radius * radius - text.y * text.y * 0.25f));
            float side = prF32Max(height * 0.25f, inset);
            float padded = text.x + 2 * p;
            PrRect r = box(prF32Max(prF32Max(20, padded + 2 * side), padded * 1.25f), height);
            float round = r.right - radius;
            PrVec2 points[] = {
                {round, r.top}, {r.left + height * 0.25f, r.top}, {r.left, 0},
                {r.left + height * 0.25f, r.bottom}, {round, r.bottom},
            };
            prPathMoveTo(outline, points[0]);
            for (uint32_t i = 1; i < countOf(points); ++i)
                prPathLineTo(outline, points[i]);
            prPathEllipticArcTo(outline, PrVec2{round, 0}, PrVec2{radius, radius}, 0, quarter, -kPrF32Pi);
            prPathClose(outline);
            break;
        }
        case MermaidShape_Delay: {
            // A half circle at the right, wide enough to clear the label.
            float height = prF32Max(10, text.y) + 2 * p;
            float radius = height * 0.5f;
            float inset = radius - prF32Sqrt(prF32Max(0.0f, radius * radius - text.y * text.y * 0.25f));
            PrRect r = box(prF32Max(15, text.x) + 2 * inset + 2 * p, height);
            prPathMoveTo(outline, PrVec2{r.left, r.top});
            prPathLineTo(outline, PrVec2{r.right - radius, r.top});
            prPathEllipticArcTo(outline, PrVec2{r.right - radius, 0}, PrVec2{radius, radius}, 0, -quarter, kPrF32Pi);
            prPathLineTo(outline, PrVec2{r.left, r.bottom});
            prPathClose(outline);
            break;
        }
        case MermaidShape_Document:
        case MermaidShape_LinedDocument:
        case MermaidShape_TaggedDocument: {
            // A wave of 0.8 periods along the bottom, whose amplitude is an eighth of the height above it. A lined or
            // tagged document is a tenth wider.
            float width = text.x + 2 * p;
            float height = text.y + 2 * p;
            float wave = height / 8;
            float body = shape == MermaidShape_Document ? width : width * 1.1f;
            PrRect r = box(body, height + 2 * wave);
            float base = r.bottom - wave;
            prPathMoveTo(outline, PrVec2{r.left, base});
            addWave(outline, PrVec2{r.left, base}, body, wave, 0.8f);
            prPathLineTo(outline, PrVec2{r.right, r.top});
            prPathLineTo(outline, PrVec2{r.left, r.top});
            prPathClose(outline);
            out.label.y = -wave;
            if (shape == MermaidShape_LinedDocument) {
                float x = -width * 0.5f;
                float t = (x - r.left) / body;
                addLine(details, PrVec2{x, r.top}, PrVec2{x, base + wave * prF32Sin(2 * kPrF32Pi * 0.8f * t)});
                out.label.x = width * 0.025f;
            } else if (shape == MermaidShape_TaggedDocument) {
                // A fold across the bottom right corner, placed by mermaid.js's own proportions.
                float left = width * 0.35f;
                float right = width * 0.55f;
                float k = height * 0.92f - (height + wave) * 0.5f;
                float dy = -wave * 0.5f;
                prPathMoveTo(details, PrVec2{left, k * 1.3f + dy});
                prPathLineTo(details, PrVec2{right, k - height * 0.2f + dy});
                prPathLineTo(details, PrVec2{right, k * 0.9f + dy});
                prPathLineTo(details, PrVec2{right, k * 1.275f + dy});
                addWave(details, PrVec2{right, k * 1.275f + dy}, left - right, -height * 0.02f, 0.5f);
                prPathClose(details);
                out.label.y = dy;
            }
            break;
        }
        case MermaidShape_StackedDocument: {
            // Three documents, each 10 px up and to the right of the one in front of it.
            constexpr float kStep = 10;
            float width = text.x + 2 * p;
            float height = text.y + 3 * p;
            float wave = height / 8;
            float body = height + wave * 0.5f;
            PrRect r = box(width + 2 * kStep, body + wave + 2 * kStep);
            float base = r.bottom - wave;
            float front = r.left + width;
            prPathMoveTo(outline, PrVec2{r.left, r.top + 2 * kStep});
            prPathLineTo(outline, PrVec2{r.left, base});
            PrVec2 end = addWave(outline, PrVec2{r.left, base}, width, wave, 0.8f);
            PrVec2 points[] = {
                {front, end.y - kStep}, {front + kStep, end.y - kStep},
                {front + kStep, end.y - 2 * kStep}, {r.right, end.y - 2 * kStep}, {r.right, r.top},
                {r.left + 2 * kStep, r.top}, {r.left + 2 * kStep, r.top + kStep}, {r.left + kStep, r.top + kStep},
                {r.left + kStep, r.top + 2 * kStep},
            };
            for (PrVec2 point : points)
                prPathLineTo(outline, point);
            prPathClose(outline);
            PrVec2 edges[] = {
                {r.left + kStep, r.top + 2 * kStep}, {front, r.top + 2 * kStep}, {front, end.y - kStep},
                {front + kStep, end.y - kStep}, {front + kStep, r.top + kStep}, {r.left + kStep, r.top + kStep},
            };
            addPolygon(details, edges, countOf(edges));
            out.label = PrVec2{-kStep, kStep - wave};
            break;
        }
        case MermaidShape_PaperTape: {
            // Waves of one period along the top and the bottom, in step.
            float height = h;
            float wave = height / 8;
            PrRect r = box(text.x + 2 * p, height + 4 * wave);
            prPathMoveTo(outline, PrVec2{r.left, r.bottom - wave});
            addWave(outline, PrVec2{r.left, r.bottom - wave}, r.right - r.left, wave, 1);
            prPathLineTo(outline, PrVec2{r.right, r.top + wave});
            addWave(outline, PrVec2{r.right, r.top + wave}, r.left - r.right, wave, -1);
            prPathClose(outline);
            break;
        }
        case MermaidShape_Triangle:
        case MermaidShape_FlippedTriangle: {
            // The label sits near the base.
            float side = w + text.y;
            PrRect r = box(side, side);
            if (shape == MermaidShape_Triangle) {
                PrVec2 points[] = {{r.left, r.bottom}, {r.right, r.bottom}, {0, r.top}};
                addPolygon(outline, points, countOf(points));
                out.label.y = r.bottom - text.y * 0.5f - p * 0.5f;
            } else {
                PrVec2 points[] = {{r.left, r.top}, {r.right, r.top}, {0, r.bottom}};
                addPolygon(outline, points, countOf(points));
                out.label.y = r.top + text.y * 0.5f + p * 0.5f;
            }
            break;
        }
        case MermaidShape_BraceLeft:
        case MermaidShape_BraceRight:
        case MermaidShape_Braces: {
            // Braces beside the label, whose curls take a tenth of the height, and at least 5 px.
            float r = prF32Max(5, h * 0.1f);
            bool both = shape == MermaidShape_Braces;
            PrRect frame = box(w + (both ? 2.5f : 2) * r, h + 2 * r);
            prPathAddRect(outline, frame);
            if (shape != MermaidShape_BraceRight)
                addBrace(details, frame.left + 2 * r, 0, h * 0.5f, r, 1);
            if (shape != MermaidShape_BraceLeft)
                addBrace(details, frame.right - 2 * r, 0, h * 0.5f, r, -1);
            if (shape == MermaidShape_BraceLeft)
                out.label.x = r - p * 0.5f;
            out.paint = ShapePaint_None;
            break;
        }
        case MermaidShape_SmallCircle:
        case MermaidShape_FramedCircle:
        case MermaidShape_FilledCircle:
            box(14, 14);
            prPathAddEllipse(outline, PrVec2{}, PrVec2{7, 7}, 0);
            if (shape == MermaidShape_FramedCircle)
                prPathAddEllipse(details, PrVec2{}, PrVec2{2.5f, 2.5f}, 0);
            out.paint = shape == MermaidShape_FilledCircle ? ShapePaint_Solid : ShapePaint_Normal;
            out.hasLabel = false;
            break;
        case MermaidShape_CrossedCircle: {
            constexpr float kRadius = 30;
            box(2 * kRadius, 2 * kRadius);
            prPathAddEllipse(outline, PrVec2{}, PrVec2{kRadius, kRadius}, 0);
            float d = kRadius * prF32Cos(kPrF32Pi * 0.25f);
            addLine(details, PrVec2{-d, d}, PrVec2{d, -d});
            addLine(details, PrVec2{d, d}, PrVec2{-d, -d});
            out.hasLabel = false;
            break;
        }
        case MermaidShape_Fork:
            prPathAddRect(outline, across ? box(10, 70) : box(70, 10));
            out.hasLabel = false;
            break;
        case MermaidShape_Hourglass: {
            PrRect r = box(30, 30);
            PrVec2 points[] = {{r.left, r.top}, {r.right, r.top}, {r.left, r.bottom}, {r.right, r.bottom}};
            addPolygon(outline, points, countOf(points));
            out.hasLabel = false;
            break;
        }
        case MermaidShape_Bolt: {
            box(35, 70);
            PrVec2 points[] = {
                {17.5f, -35}, {-17.5f, 3.5f}, {3.5f, 3.5f}, {-17.5f, 35}, {17.5f, -3.5f},
                {-3.5f, -3.5f},
            };
            addPolygon(outline, points, countOf(points));
            out.hasLabel = false;
            break;
        }
        case MermaidShape_Folder: {
            // A tab at the top left, whose height is a sixth of the body's, from 8 px to 14 px.
            float width = prF32Max(text.x + 2 * p, 90);
            float body = text.y + 2 * p;
            float tab = prF32Clamp(body * 0.16f, 8, 14);
            PrRect r = box(width, body + tab);
            float tabRight = r.left + prF32Max(width * 0.38f, 28);
            PrVec2 points[] = {
                {r.left, r.top}, {tabRight, r.top}, {tabRight, r.top + tab}, {r.right, r.top + tab},
                {r.right, r.bottom}, {r.left, r.bottom},
            };
            addPolygon(outline, points, countOf(points));
            out.label.y = tab * 0.5f;
            break;
        }
        case MermaidShape_Bucket: {
            // A bucket, wider at its open top, whose rim is an ellipse. mermaid.js centers its label rather than its
            // box, and this centers both.
            float width = prF32Max(text.x + 2 * p, 80);
            float rim = prF32Clamp(width * 0.08f, 5, 12);
            float height = text.y + 2 * p + rim;
            PrRect r = box(width, height + rim);
            float top = r.top + rim;
            float bottom = r.bottom - rim;
            float base = width * 0.36f;
            prPathMoveTo(outline, PrVec2{r.left, top});
            prPathLineTo(outline, PrVec2{-base, bottom});
            prPathEllipticArcTo(outline, PrVec2{0, bottom}, PrVec2{base, rim}, 0, kPrF32Pi, -kPrF32Pi);
            prPathLineTo(outline, PrVec2{r.right, top});
            prPathEllipticArcTo(outline, PrVec2{0, top}, PrVec2{width * 0.5f, rim}, 0, 0, -kPrF32Pi);
            prPathClose(outline);
            prPathAddEllipse(details, PrVec2{0, top}, PrVec2{width * 0.5f, rim}, 0);
            break;
        }
        case MermaidShape_Console:
        case MermaidShape_Browser: {
            // A window, whose bar at the top holds a prompt, or three dots and an address bar.
            bool console = shape == MermaidShape_Console;
            float bar = console ? 20.0f : 18.0f;
            PrRect r = box(prF32Max(text.x + 2 * p, 90), text.y + 2 * p + bar);
            prPathAddCornerRect(outline, prCornerRectRound(r, 12));
            if (!console) {
                addLine(details, PrVec2{r.left, r.top + bar}, PrVec2{r.right, r.top + bar});
                PrRect address{r.left + 44, r.top + 4, r.left + 44 + prF32Max(r.right - r.left - 56, 10), r.top + bar - 4};
                prPathAddCornerRect(details, prCornerRectRound(address, 3));
                for (uint32_t i = 0; i < 3; ++i)
                    prPathAddEllipse(marks, PrVec2{r.left + 12 + float(i) * 9, r.top + bar * 0.5f}, PrVec2{2.5f, 2.5f}, 0);
            }
            out.label.y = bar * 0.5f;
            break;
        }
        case MermaidShape_Person: {
            // A head above a rounded body, which it overlaps a little.
            float width = prF32Max(text.x + 2 * p, 100);
            float head = prF32Clamp(width * 0.23f, 16, 56);
            float overlap = head * 0.27f;
            float body = text.y + 2 * p;
            float radius = prF32Min(width * 0.177f, body * 0.45f);
            PrRect r = box(width, body + 2 * head - overlap);
            PrRect torso{r.left, r.bottom - body, r.right, r.bottom};
            // The outline goes around both, and the head's lower edge shows over the body, as mermaid.js draws the head
            // on top.
            PrVec2 neck{prF32Sqrt(head * head - (head - overlap) * (head - overlap)), torso.top};
            float angle = prF32Atan2(head - overlap, neck.x);
            PrVec2 headCenter{0, r.top + head};
            prPathMoveTo(outline, neck);
            prPathLineTo(outline, PrVec2{torso.right - radius, torso.top});
            prPathEllipticArcTo(outline, PrVec2{torso.right - radius, torso.top + radius}, PrVec2{radius, radius}, 0,
                -quarter, quarter);
            prPathLineTo(outline, PrVec2{torso.right, torso.bottom - radius});
            prPathEllipticArcTo(outline, PrVec2{torso.right - radius, torso.bottom - radius}, PrVec2{radius, radius},
                0, 0, quarter);
            prPathLineTo(outline, PrVec2{torso.left + radius, torso.bottom});
            prPathEllipticArcTo(outline, PrVec2{torso.left + radius, torso.bottom - radius}, PrVec2{radius, radius},
                0, quarter, quarter);
            prPathLineTo(outline, PrVec2{torso.left, torso.top + radius});
            prPathEllipticArcTo(outline, PrVec2{torso.left + radius, torso.top + radius}, PrVec2{radius, radius}, 0,
                kPrF32Pi, quarter);
            prPathLineTo(outline, PrVec2{-neck.x, neck.y});
            prPathEllipticArcTo(outline, headCenter, PrVec2{head, head}, 0, kPrF32Pi - angle, kPrF32Pi + 2 * angle);
            prPathClose(outline);
            prPathMoveTo(details, neck);
            prPathEllipticArcTo(details, headCenter, PrVec2{head, head}, 0, angle, kPrF32Pi - 2 * angle);
            out.label.y = torso.top + body * 0.5f;
            break;
        }
        case MermaidShape_Bang:
        case MermaidShape_Cloud: {
            // Arcs around a box, as mermaid.js's mindmap draws a bang or a cloud, measured by their bounds.
            bool bang = shape == MermaidShape_Bang;
            float half = p * 0.5f;
            float width = bang ? prF32Max(text.x + 10 * half, text.x + 20) : text.x + 2 * half;
            float height = bang ? prF32Max(text.y + 8 * half, text.y + 20) : text.y + 2 * half;
            PrVec2 at{-width * 0.5f, -height * 0.5f};
            prPathMoveTo(outline, at);
            auto arcTo = [&](float rx, float ry, PrVec2 delta, bool sweep) {
                PrVec2 to = at + delta;
                addSvgArc(outline, at, to, PrVec2{rx, ry}, false, sweep);
                at = to;
            };
            if (bang) {
                float s = width;
                float t = height;
                float v = 0.15f * s;
                arcTo(v, v, PrVec2{s * 0.25f, -t * 0.1f}, false);
                arcTo(v, v, PrVec2{s * 0.25f, 0}, false);
                arcTo(v, v, PrVec2{s * 0.25f, 0}, false);
                arcTo(v, v, PrVec2{s * 0.25f, t * 0.1f}, false);
                arcTo(v, v, PrVec2{s * 0.15f, t * 0.33f}, false);
                arcTo(v * 0.8f, v * 0.8f, PrVec2{0, t * 0.34f}, false);
                arcTo(v, v, PrVec2{-s * 0.15f, t * 0.33f}, false);
                arcTo(v, v, PrVec2{-s * 0.25f, t * 0.15f}, false);
                arcTo(v, v, PrVec2{-s * 0.25f, 0}, false);
                arcTo(v, v, PrVec2{-s * 0.25f, 0}, false);
                arcTo(v, v, PrVec2{-s * 0.25f, -t * 0.15f}, false);
                arcTo(v, v, PrVec2{-s * 0.1f, -t * 0.33f}, false);
                arcTo(v * 0.8f, v * 0.8f, PrVec2{0, -t * 0.34f}, false);
                arcTo(v, v, PrVec2{s * 0.1f, -t * 0.33f}, false);
            } else {
                float m = width;
                float b = height;
                arcTo(0.15f * m, 0.15f * m, PrVec2{m * 0.25f, -m * 0.1f}, true);
                arcTo(0.35f * m, 0.35f * m, PrVec2{m * 0.4f, -m * 0.1f}, true);
                arcTo(0.25f * m, 0.25f * m, PrVec2{m * 0.35f, m * 0.2f}, true);
                arcTo(0.15f * m, 0.15f * m, PrVec2{m * 0.15f, b * 0.35f}, true);
                arcTo(0.2f * m, 0.2f * m, PrVec2{-m * 0.15f, b * 0.65f}, true);
                arcTo(0.25f * m, 0.15f * m, PrVec2{-m * 0.25f, m * 0.15f}, true);
                arcTo(0.35f * m, 0.35f * m, PrVec2{-m * 0.5f, 0}, true);
                arcTo(0.15f * m, 0.15f * m, PrVec2{-m * 0.25f, -m * 0.15f}, true);
                arcTo(0.15f * m, 0.15f * m, PrVec2{-m * 0.1f, -b * 0.35f}, true);
                arcTo(0.2f * m, 0.2f * m, PrVec2{m * 0.1f, -b * 0.65f}, true);
            }
            prPathLineTo(outline, PrVec2{-width * 0.5f, at.y});
            prPathClose(outline);
            PrRect bounds = prPathTightBounds(outline);
            out.extent = PrVec2{bounds.right - bounds.left, bounds.bottom - bounds.top};
            // Centers the outline in its box.
            PrVec2 shift{-(bounds.left + bounds.right) * 0.5f, -(bounds.top + bounds.bottom) * 0.5f};
            prPathTransform(outline, prAffine2Translate(shift));
            out.label = shift;
            break;
        }
        default:
            prPathAddRect(outline, box(text.x + 4 * p, text.y + 2 * p));
            break;
    }
    return out;
}

// Moves `end`, a point on the box of a node, along `inward` into the node until it meets the node's outline.
PrVec2 clipToOutline(const PrPath* outline, PrRect box, PrVec2 end, PrVec2 inward) {
    // The outline flattened to polygons, at a tolerance well below a pixel.
    PrPathPolyline polyline{};
    prPathFlatten(outline, prAffine2Identity(), 0.1f, PrPathFlattening_Inside, &polyline);
    float reach = (box.right - box.left) + (box.bottom - box.top);
    PrVec2 d = inward * reach;
    float best = 1;
    bool found = false;
    for (uint32_t k = 0; k < polyline.contourCount; ++k) {
        uint32_t first = polyline.contours[k];
        uint32_t last = prPathPolylineContourEnd(&polyline, k);
        for (uint32_t i = first; i < last; ++i) {
            PrVec2 a = polyline.points[i];
            PrVec2 b = polyline.points[i + 1 < last ? i + 1 : first];
            // The parameters along the ray and along a -> b where the two intersect.
            PrVec2 e = b - a;
            float denominator = d.x * e.y - d.y * e.x;
            if (prF32Abs(denominator) < 1e-6f)
                continue;
            PrVec2 f = a - end;
            float t = (f.x * e.y - f.y * e.x) / denominator;
            float u = (f.x * d.y - f.y * d.x) / denominator;
            if (t >= -1e-4f && t <= 1 && u >= -1e-4f && u <= 1 + 1e-4f && t < best) {
                best = t < 0 ? 0 : t;
                found = true;
            }
        }
    }
    prPathPolylineReset(&polyline);
    return found ? end + d * best : end;
}

// A route's points, held separately while its ends are adjusted.
struct Route {
    RawArray<PrVec2> points;
};

void moveEnd(Route& route, bool atStart, PrVec2 delta) {
    mermaidRouteMoveEnd(route.points.data, route.points.count, atStart, delta);
}

PrVec2 endDirection(const Route& route, bool atStart) {
    return mermaidRouteEndDirection(route.points.data, route.points.count, atStart);
}

MermaidMarker markerOf(MermaidHead head) {
    switch (head) {
        case MermaidHead_Arrow:
            return MermaidMarker_Arrow;
        case MermaidHead_Circle:
            return MermaidMarker_Circle;
        case MermaidHead_Cross:
            return MermaidMarker_Cross;
        default:
            return MermaidMarker_None;
    }
}

// Returns true if a link joins a node inside `subgraph` to a node outside it. A link to the subgraph itself does not
// count.
bool linksOutside(const MermaidFlowchart& chart, uint32_t subgraph) {
    for (uint32_t e = 0; e < chart.edges.count; ++e) {
        const MermaidEdge& edge = chart.edges.data[e];
        bool source = edge.source != subgraph && mermaidFlowchartHolds(chart, subgraph, edge.source);
        bool target = edge.target != subgraph && mermaidFlowchartHolds(chart, subgraph, edge.target);
        if (source != target && !(edge.source == subgraph || edge.target == subgraph))
            return true;
    }
    return false;
}

uint32_t depthOf(const MermaidFlowchart& chart, uint32_t node) {
    uint32_t depth = 0;
    for (uint32_t n = chart.nodes.data[node].parent; n != kMermaidNone; n = chart.nodes.data[n].parent)
        ++depth;
    return depth;
}

MermaidTextStyle labelStyle() {
    MermaidTextStyle style = mermaidTextStyle(kFontSize);
    style.wrapWidth = kWrapWidth;
    style.lineHeight = kFontSize * kLineHeight;
    return style;
}

// The paint of a node or subgraph: the theme's, with the node's style on top.
MermaidPaint nodePaint(const MermaidNode& node) {
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_Color,
        .fill = mermaidInk(node.isSubgraph ? MermaidRole_ClusterFill : MermaidRole_NodeFill),
        .stroke = mermaidInk(node.isSubgraph ? MermaidRole_ClusterStroke : MermaidRole_NodeStroke),
        .color = mermaidInk(node.isSubgraph ? MermaidRole_Title : MermaidRole_NodeText),
    };
    mermaidPaintMerge(paint, node.style);
    return paint;
}

// The shapes and placement of a flowchart, by node and by edge.
struct Layout {
    RawArray<MermaidLabelIndex> nodeLabels;
    RawArray<PrRect> nodeRects;
    RawArray<Shape> nodeShapes;
    RawArray<PrPath*> nodeOutlines;     // a subgraph's is built once it is placed, and a node's in its own space
    RawArray<PrPath*> nodeDetails;
    RawArray<PrPath*> nodeMarks;
    RawArray<MermaidLabelIndex> nodePrompts; // by node: a console's prompt, or kMermaidNone
    RawArray<uint32_t> drawOrder;            // the nodes by depth, then in source order
    RawArray<MermaidLabelIndex> edgeLabels;
    RawArray<PrRect> edgeLabelRects;
    RawArray<PrGraphEdge> graphEdges;   // by edge, or kMermaidNone for a link that is not laid out
    RawArray<PrGraphLabel> graphLabels; // by edge, or kMermaidNone
    RawArray<PrGraphNode> graphNodes;
    RawArray<uint8_t> hasMembers;       // by node: 1 for a subgraph that holds nodes
};

void resetLayout(Layout& l) {
    for (uint32_t i = 0; i < l.nodeOutlines.count; ++i) {
        prPathDestroy(l.nodeOutlines.data[i]);
        prPathDestroy(l.nodeDetails.data[i]);
        prPathDestroy(l.nodeMarks.data[i]);
    }
    l.nodeLabels.reset();
    l.nodeRects.reset();
    l.nodeShapes.reset();
    l.nodeOutlines.reset();
    l.nodeDetails.reset();
    l.nodeMarks.reset();
    l.nodePrompts.reset();
    l.drawOrder.reset();
    l.edgeLabels.reset();
    l.edgeLabelRects.reset();
    l.graphEdges.reset();
    l.graphLabels.reset();
    l.graphNodes.reset();
    l.hasMembers.reset();
}

// Describes the flowchart to `graph`.
void describe(MermaidRender& render, const MermaidFlowchart& chart, Layout& l, PrGraph* graph) {
    uint32_t nodeCount = chart.nodes.count;
    uint32_t edgeCount = chart.edges.count;
    l.nodeRects.resize(nodeCount);
    l.nodeLabels.resize(nodeCount);
    l.nodeShapes.resize(nodeCount);
    l.nodeOutlines.resize(nodeCount);
    l.nodeDetails.resize(nodeCount);
    l.nodeMarks.resize(nodeCount);
    l.nodePrompts.resize(nodeCount);
    // A console's prompt is mermaid.js's: bold monospace text at 14 px.
    MermaidTextStyle promptStyle = mermaidTextStyle(14);
    promptStyle.weight = 700;
    promptStyle.monospace = true;
    bool across = chart.direction == PrGraphDirection_Left || chart.direction == PrGraphDirection_Right;
    for (uint32_t n = 0; n < nodeCount; ++n) {
        const MermaidNode& node = chart.nodes.data[n];
        l.nodeLabels.data[n] =
            mermaidLabelCreate(render, mermaidFlowchartText(chart, node.label), node.format, labelStyle());
        l.nodeOutlines.data[n] = prPathCreate();
        l.nodeDetails.data[n] = prPathCreate();
        l.nodeMarks.data[n] = prPathCreate();
        l.nodeShapes.data[n] = Shape{};
        if (!node.isSubgraph) {
            l.nodeShapes.data[n] = buildShape(node.shape, mermaidLabelExtent(render, l.nodeLabels.data[n]), across,
                l.nodeOutlines.data[n], l.nodeDetails.data[n], l.nodeMarks.data[n]);
        }
        bool console = !node.isSubgraph && node.shape == MermaidShape_Console;
        l.nodePrompts.data[n] =
            console ? mermaidLabelCreate(render, ">_", MermaidTextFormat_Plain, promptStyle) : kMermaidNone;
    }
    l.edgeLabels.resize(edgeCount);
    l.edgeLabelRects.resize(edgeCount);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const MermaidEdge& edge = chart.edges.data[e];
        l.edgeLabels.data[e] =
            mermaidLabelCreate(render, mermaidFlowchartText(chart, edge.label), edge.format, labelStyle());
    }

    // The graph's nodes go by depth, then in source order, so that each node comes after its subgraph.
    l.drawOrder.resize(nodeCount);
    RawArray<uint32_t> depth{};
    depth.resize(nodeCount);
    for (uint32_t n = 0; n < nodeCount; ++n) {
        l.drawOrder.data[n] = n;
        depth.data[n] = depthOf(chart, n);
    }
    sortItems(l.drawOrder.span(), [&](uint32_t a, uint32_t b) {
        return depth.data[a] < depth.data[b] || (depth.data[a] == depth.data[b] && a < b);
    });
    depth.reset();
    l.graphNodes.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        uint32_t n = l.drawOrder.data[i];
        const MermaidNode& node = chart.nodes.data[n];
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        PrVec2 text = mermaidLabelExtent(render, l.nodeLabels.data[n]);
        desc.parent = node.parent != kMermaidNone ? l.graphNodes.data[node.parent] : kPrGraphNodeNone;
        if (node.isSubgraph) {
            // A subgraph keeps its own direction only while its nodes link among themselves. Otherwise it lays out in
            // its parent's direction.
            desc.direction = linksOutside(chart, n) ? PrGraphDirection_Inherit : node.direction;
            desc.extent = PrVec2{text.x + 2 * kClusterPadding, 0};
            for (float& side : desc.padding)
                side = kClusterPadding;
            desc.padding[PrRectSide_Top] = kClusterPadding + text.y;
        } else {
            desc.extent = l.nodeShapes.data[n].extent;
        }
        l.graphNodes.data[n] = prGraphAddNode(graph, &desc);
    }
    // A subgraph without members is laid out as a plain node.
    l.hasMembers.assignZeroed(nodeCount);
    for (uint32_t n = 0; n < nodeCount; ++n) {
        if (chart.nodes.data[n].parent != kMermaidNone)
            l.hasMembers.data[chart.nodes.data[n].parent] = 1;
    }

    l.graphEdges.resize(edgeCount);
    l.graphLabels.resize(edgeCount);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const MermaidEdge& edge = chart.edges.data[e];
        l.graphEdges.data[e] = kMermaidNone;
        l.graphLabels.data[e] = kMermaidNone;
        // Flow between a subgraph and its own members has no direction, so the layout does not join them.
        if (edge.source != edge.target && (mermaidFlowchartHolds(chart, edge.source, edge.target) ||
                mermaidFlowchartHolds(chart, edge.target, edge.source)))
            continue;
        PrGraphEdgeDesc desc = prGraphEdgeDescDefault(l.graphNodes.data[edge.source], l.graphNodes.data[edge.target]);
        desc.minLength = edge.length;
        // Each end of a link gets its own port, which the layout spreads along the side that faces the link, so that a
        // node's links meet it at separate points. Self-loops, and ends on a subgraph with members, meet the box
        // instead.
        if (edge.source != edge.target) {
            if (!(chart.nodes.data[edge.source].isSubgraph && l.hasMembers.data[edge.source] != 0)) {
                PrGraphPortDesc port{.node = desc.source, .constraint = PrGraphPortConstraint_Free};
                desc.sourcePort = prGraphAddPort(graph, &port);
            }
            if (!(chart.nodes.data[edge.target].isSubgraph && l.hasMembers.data[edge.target] != 0)) {
                PrGraphPortDesc port{.node = desc.target, .constraint = PrGraphPortConstraint_Free};
                desc.targetPort = prGraphAddPort(graph, &port);
            }
        }
        l.graphEdges.data[e] = prGraphAddEdge(graph, &desc);
        PrVec2 text = mermaidLabelExtent(render, l.edgeLabels.data[e]);
        if (text.x > 0) {
            PrGraphLabelDesc label{
                .edge = l.graphEdges.data[e],
                .extent = PrVec2{text.x + 2 * kEdgeLabelPadding, text.y + kEdgeLabelPadding},
                .placement = PrGraphLabelPlacement_Center,
            };
            l.graphLabels.data[e] = prGraphAddLabel(graph, &label);
        }
    }
}

// Records an edge's route, clipped to its nodes' outlines and ended with its marks.
void drawEdge(MermaidRender& render, const MermaidFlowchart& chart, const Layout& l, uint32_t e, Route& route) {
    const MermaidEdge& edge = chart.edges.data[e];
    // Each end meets its node's box, then continues along its route into the node until it meets the outline.
    bool loop = edge.source == edge.target;
    for (int side = 0; side < 2 && !loop; ++side) {
        uint32_t node = side == 0 ? edge.source : edge.target;
        if (chart.nodes.data[node].isSubgraph && l.hasMembers.data[node] != 0)
            continue;
        bool atStart = side == 0;
        PrVec2 end = route.points.data[atStart ? 0 : route.points.count - 1];
        PrVec2 clipped = clipToOutline(l.nodeOutlines.data[node], l.nodeRects.data[node], end, endDirection(route, atStart));
        moveEnd(route, atStart, clipped - end);
    }
    mermaidItemAddEdge(render, route.points.data, route.points.count, mermaidFlowchartText(chart, edge.id));
    if (edge.line == MermaidLinkStroke_Invisible)
        return;
    const MermaidPaint& style = edge.style;
    MermaidInk ink = (style.flags & MermaidPaintFlag_Stroke) != 0 ? style.stroke : mermaidInk(MermaidRole_Line);
    float width = edge.line == MermaidLinkStroke_Thick ? kThickWidth : kLineWidth;
    if ((style.flags & MermaidPaintFlag_StrokeWidth) != 0)
        width = style.strokeWidth;
    const float* dashes = edge.line == MermaidLinkStroke_Dotted ? kDotted : nullptr;
    if ((style.flags & MermaidPaintFlag_Dash) != 0)
        dashes = style.dash;

    // The marks, and the route shortened so that it stops at them.
    for (int side = 0; side < 2; ++side) {
        bool atStart = side == 0;
        MermaidMarker marker = markerOf(atStart ? edge.start : edge.end);
        if (marker == MermaidMarker_None || route.points.count < 2)
            continue;
        PrVec2 tip = route.points.data[atStart ? 0 : route.points.count - 1];
        PrVec2 direction = endDirection(route, atStart);
        float back = mermaidDrawMarker(render, marker, tip, direction, ink, kMarkerSize);
        moveEnd(route, atStart, direction * -back);
    }
    PrPath* path = prPathCreate();
    mermaidRouteAppend(path, route.points.data, route.points.count);
    mermaidStrokePath(render, path, ink, width, dashes);
    prPathDestroy(path);
}

} // namespace

PrStatus mermaidFlowchartDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidFlowchart chart{};
    PrStatus status = mermaidFlowchartParse(chart, source, error);
    if (status != PrStatus_Ok) {
        mermaidFlowchartReset(chart);
        return status;
    }
    Layout l{};
    PrGraph* graph = prGraphCreate();
    describe(render, chart, l, graph);
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    options.direction = chart.direction;
    options.routing = PrGraphRouting_Spline;
    prGraphLayoutCompute(graph, &options);

    // Centers the title above the diagram, and moves the diagram down to make room for it.
    MermaidTextStyle titleStyle = mermaidTextStyle(kTitleFontSize);
    MermaidLabelIndex title =
        mermaidLabelCreate(render, mermaidFlowchartText(chart, chart.title), MermaidTextFormat_Plain, titleStyle);
    PrVec2 titleExtent = mermaidLabelExtent(render, title);
    float titleSpace = titleExtent.y > 0 ? titleExtent.y + kTitleGap : 0;
    PrVec2 offset{kDiagramPadding, kDiagramPadding + titleSpace};
    PrRect bounds = prGraphBounds(graph);
    float width = bounds.right > titleExtent.x ? bounds.right : titleExtent.x;
    render.extent = PrVec2{width + 2 * kDiagramPadding, bounds.bottom + titleSpace + 2 * kDiagramPadding};
    offset.x += (width - bounds.right) * 0.5f;
    PrVec2 titleAt{(render.extent.x - titleExtent.x) * 0.5f, kDiagramPadding};
    mermaidLabelDraw(render, title, titleAt, mermaidInk(MermaidRole_Title));

    uint32_t nodeCount = chart.nodes.count;
    for (uint32_t n = 0; n < nodeCount; ++n) {
        PrRect r = prGraphNodeRect(graph, l.graphNodes.data[n]);
        r = PrRect{r.left + offset.x, r.top + offset.y, r.right + offset.x, r.bottom + offset.y};
        l.nodeRects.data[n] = r;
        if (chart.nodes.data[n].isSubgraph) {
            prPathAddRect(l.nodeOutlines.data[n], r);
            continue;
        }
        PrAffine2 place = prAffine2Translate(PrVec2{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f});
        prPathTransform(l.nodeOutlines.data[n], place);
        prPathTransform(l.nodeDetails.data[n], place);
        prPathTransform(l.nodeMarks.data[n], place);
    }

    // Subgraphs, outermost first.
    for (uint32_t i = 0; i < nodeCount; ++i) {
        uint32_t n = l.drawOrder.data[i];
        const MermaidNode& node = chart.nodes.data[n];
        if (!node.isSubgraph)
            continue;
        MermaidPaint paint = nodePaint(node);
        PrRect r = l.nodeRects.data[n];
        mermaidDrawPath(render, l.nodeOutlines.data[n], paint);
        PrVec2 extent = mermaidLabelExtent(render, l.nodeLabels.data[n]);
        PrVec2 labelAt{(r.left + r.right - extent.x) * 0.5f, r.top + kClusterPadding * 0.5f};
        mermaidLabelDraw(render, l.nodeLabels.data[n], labelAt, paint.color);
        mermaidItemAdd(render, l.hasMembers.data[n] != 0 ? PrMermaidItemKind_Group : PrMermaidItemKind_Node, r,
            mermaidFlowchartText(chart, node.id), mermaidFlowchartText(chart, node.link));
    }

    // Edges and their marks, then their labels.
    Route route{};
    for (uint32_t e = 0; e < chart.edges.count; ++e) {
        if (l.graphEdges.data[e] == kMermaidNone)
            continue;
        if (l.graphLabels.data[e] != kMermaidNone) {
            PrRect r = prGraphLabelRect(graph, l.graphLabels.data[e]);
            l.edgeLabelRects.data[e] = PrRect{r.left + offset.x, r.top + offset.y, r.right + offset.x, r.bottom + offset.y};
        }
        PrGraphRoute placed = prGraphEdgeRoute(graph, l.graphEdges.data[e]);
        route.points.clear();
        for (uint32_t i = 0; i < placed.pointCount; ++i)
            route.points.push(placed.points[i] + offset);
        if (route.points.count >= 2)
            drawEdge(render, chart, l, e, route);
    }
    route.points.reset();
    for (uint32_t e = 0; e < chart.edges.count; ++e) {
        const MermaidEdge& edge = chart.edges.data[e];
        if (l.graphLabels.data[e] == kMermaidNone || edge.line == MermaidLinkStroke_Invisible)
            continue;
        PrRect r = l.edgeLabelRects.data[e];
        mermaidFillRect(render, r, mermaidInk(MermaidRole_EdgeLabelBackground));
        bool colored = (edge.style.flags & MermaidPaintFlag_Color) != 0;
        MermaidInk ink = colored ? edge.style.color : mermaidInk(MermaidRole_Text);
        mermaidLabelDrawCentered(render, l.edgeLabels.data[e],
            PrVec2{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f}, ink);
        mermaidItemAdd(render, PrMermaidItemKind_Label, r, PrStringView{}, PrStringView{});
    }

    // Nodes, with their labels.
    for (uint32_t n = 0; n < nodeCount; ++n) {
        const MermaidNode& node = chart.nodes.data[n];
        if (node.isSubgraph)
            continue;
        const Shape& shape = l.nodeShapes.data[n];
        MermaidPaint paint = nodePaint(node);
        MermaidPaint outline = paint;
        if (shape.paint == ShapePaint_None)
            outline.flags &= MermaidPaintFlags(~(MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke));
        else if (shape.paint == ShapePaint_Fill)
            outline.flags &= MermaidPaintFlags(~MermaidPaintFlag_Stroke);
        else if (shape.paint == ShapePaint_Solid)
            outline.fill = paint.stroke;
        mermaidDrawPath(render, l.nodeOutlines.data[n], outline);
        MermaidPaint details = paint;
        details.flags &= MermaidPaintFlags(~MermaidPaintFlag_Fill);
        if (prPathVerbCount(l.nodeDetails.data[n]) > 0)
            mermaidDrawPath(render, l.nodeDetails.data[n], details);
        if (prPathVerbCount(l.nodeMarks.data[n]) > 0)
            mermaidFillPath(render, l.nodeMarks.data[n], paint.stroke);
        PrRect r = l.nodeRects.data[n];
        PrVec2 center{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f};
        // A console's prompt sits in its bar, on a baseline 16 px below the top.
        if (l.nodePrompts.data[n] != kMermaidNone) {
            PrVec2 extent = mermaidLabelExtent(render, l.nodePrompts.data[n]);
            mermaidLabelDraw(render, l.nodePrompts.data[n], PrVec2{r.left + 12, r.top + 16 - extent.y * 0.8f},
                paint.stroke);
        }
        if (shape.hasLabel)
            mermaidLabelDrawCentered(render, l.nodeLabels.data[n], center + shape.label, paint.color);
        mermaidItemAdd(render, PrMermaidItemKind_Node, r, mermaidFlowchartText(chart, node.id),
            mermaidFlowchartText(chart, node.link));
    }
    prGraphDestroy(graph);
    resetLayout(l);
    mermaidFlowchartReset(chart);
    return PrStatus_Ok;
}

} // namespace pr_impl
