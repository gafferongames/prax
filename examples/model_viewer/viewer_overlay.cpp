// The model viewer's overlay: panels of text over the scene. Every panel is white text on a dark, nearly opaque
// backing, so that it reads over a bright scene and over a dark one.

#include "examples/model_viewer/viewer_overlay.h"

#include "pr/color.h"
#include "pr/draw.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/geom.h"
#include "pr/model.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/span.h"
#include "pr/status.h"

namespace model_viewer {

namespace {

constexpr PrColor kBacking{0.04f, 0.05f, 0.07f, 0.82f};
constexpr PrColor kBright{1, 1, 1, 1};
constexpr PrColor kDim{0.74f, 0.77f, 0.82f, 1};
constexpr PrColor kWarning{1, 0.72f, 0.35f, 1};
constexpr PrColor kHighlight{1, 1, 1, 0.16f};
constexpr PrColor kAccent{0.45f, 0.72f, 1, 1};

// The space between the window's edge and a panel, and between a panel's edge and its text, in logical pixels.
constexpr float kMargin = 12;
constexpr float kPadding = 12;

// The seconds that a model may take to arrive before the overlay says in the middle of the window that it is loading. A
// model that arrives sooner replaces the one before it without a message that would only flash.
constexpr double kLoadingNoticeDelay = 0.25;

// A line of a panel: `text` in `color`, and `tail` after it in the dim color.
struct Line {
    String text;
    String tail;
    float size = 13;
    PrColor color = kDim;
};

// Shapes `text` into the overlay's glyph buffer, and returns its width at `size`, or zero if it cannot be shaped.
float shape(Overlay& overlay, PrStringView text, float size) {
    PrTextRun run{};
    run.font = overlay.font;
    run.text = text;
    run.pixelsPerEm = size;
    prTextGlyphBufferClear(overlay.glyphs);
    if (overlay.font == nullptr || prTextShaperShape(overlay.shaper, &run, overlay.glyphs) != PrStatus_Ok)
        return 0;
    return prTextGlyphBufferAdvance(overlay.glyphs).x * size;
}

// Records a line of text with its baseline's start at `origin`, in logical pixels, and returns its width.
float drawText(Overlay& overlay, PrStringView text, PrVec2 origin, float size, PrColor color) {
    float width = shape(overlay, text, size);
    Array<PrFontGlyph> placed;
    Array<PrAffine2> transforms;
    placed.resize(prTextGlyphBufferGlyphCount(overlay.glyphs));
    transforms.resize(placed.count());
    if (placed.count() == 0)
        return width;
    prTextGlyphBufferPlace(overlay.glyphs, size, origin, placed.data(), transforms.data());
    PrFontGlyphRun glyphRun{};
    glyphRun.font = overlay.font;
    glyphRun.size = size;
    glyphRun.glyphs = placed.data();
    glyphRun.glyphCount = placed.count();
    prDrawListGlyphs(overlay.list, &glyphRun, PrVec2{}, {.color = color});
    return width;
}

float lineHeight(const Line& line) {
    return prF32Round(line.size * 1.45f);
}

// Records a panel of `lines` and returns its rectangle. `anchor` is a point of the window, and `align` says which point
// of the panel sits on it: 0 for the panel's left or top, 0.5 for its middle, and 1 for its right or bottom. A panel
// wider than `maxWidth` is cut there, with its text.
PrRect drawPanel(Overlay& overlay, Span<const Line> lines, PrVec2 anchor, PrVec2 align, float maxWidth) {
    float width = 0;
    float height = 2 * kPadding - 4;
    for (const Line& line : lines) {
        float text = shape(overlay, line.text, line.size);
        if (line.tail.count() != 0)
            text += line.size * 0.6f + shape(overlay, line.tail, line.size * 0.8f);
        width = prF32Max(width, text);
        height += lineHeight(line);
    }
    width = prF32Min(width + 2 * kPadding, maxWidth);
    float left = prF32Round(anchor.x - align.x * width);
    float top = prF32Round(anchor.y - align.y * height);
    PrRect rect{left, top, left + width, top + height};
    prDrawListCornerRect(overlay.list, prCornerRectRound(rect, 8), {.color = kBacking});
    prDrawListPushClipRect(overlay.list, PrRect{left, top, rect.right - kPadding / 2, rect.bottom},
        PrDrawEdges_Antialiased);
    float y = top + kPadding - 2;
    for (const Line& line : lines) {
        PrVec2 origin{left + kPadding, y + prF32Round(line.size * 1.05f)};
        float used = drawText(overlay, line.text, origin, line.size, line.color);
        if (line.tail.count() != 0)
            drawText(overlay, line.tail, {origin.x + used + line.size * 0.6f, origin.y}, line.size * 0.8f, kDim);
        y += lineHeight(line);
    }
    prDrawListPopClip(overlay.list);
    return rect;
}

// Returns `count` with the noun that it counts, as in "1 mesh" and "5 meshes".
String counted(uint32_t count, PrStringView one, PrStringView many) {
    return format("{} {}", count, count == 1 ? one : many);
}

// Returns the fraction of the stage's model that has loaded, from 0 to 1.
float loadedFraction(const Stage& stage) {
    if (!stage.isRead.load())
        return 0;
    if (stage.model == nullptr)
        return 1;
    PrModelProgress progress = prModelProgress(stage.model);
    uint32_t total = progress.meshCount + progress.imageCount;
    uint32_t done = progress.meshesReady + progress.meshesFailed + progress.imagesReady + progress.imagesFailed;
    if (total == 0)
        return 1;
    return float(done) / float(total);
}

// Appends the lines that say how far a stage has loaded, what it plays, and what its model does not draw.
void appendStageLines(Array<Line>& lines, const Stage& stage, bool paused) {
    if (!stage.isRead.load()) {
        lines.push(Line{.text = "reading the file"});
        return;
    }
    if (stage.model == nullptr) {
        lines.push(Line{.text = "cannot be shown", .color = kWarning});
        return;
    }
    PrModelProgress progress = prModelProgress(stage.model);
    uint32_t failed = progress.meshesFailed + progress.imagesFailed;
    String meshes = counted(progress.meshCount, "mesh", "meshes");
    String images = counted(progress.imageCount, "image", "images");
    String loading = format("loading: {} of {}, {} of {}", progress.meshesReady + progress.meshesFailed, meshes,
        progress.imagesReady + progress.imagesFailed, images);
    if (prModelIsLoaded(stage.model))
        loading = format("loaded: {}, {}, {} MB of texels", meshes, images, (progress.textureBytes + (1u << 19)) >> 20);
    lines.push(Line{.text = loading});
    if (failed != 0) {
        lines.push(Line{
            .text = format("failed to load: {}, {}", counted(progress.meshesFailed, "mesh", "meshes"),
                counted(progress.imagesFailed, "image", "images")),
            .color = kWarning,
        });
    }

    String playing("no animation");
    PrModelAnimationIndex animation = prModelPlacementAnimation(stage.placement);
    if (animation != kPrModelAnimationNone) {
        PrStringView name = prModelAnimationName(stage.model, animation);
        PrStringView separator = name.size != 0 ? ": " : "";
        PrStringView state = paused ? " (paused)" : "";
        playing = format("animation {} of {}{}{}  {:.2f} s{}", animation + 1, prModelAnimationCount(stage.model),
            separator, name, double(prModelPlacementTime(stage.placement)), state);
    }
    lines.push(Line{.text = playing});

    String missing;
    for (uint32_t i = 0; i < prModelDiagnosticCount(stage.model); ++i)
        missing.append(missing.count() == 0 ? "not drawn: " : ", ").append(prModelDiagnostic(stage.model, i).feature);
    if (missing.count() != 0)
        lines.push(Line{.text = missing, .color = kWarning});
}

// Records the panel at the window's top left: the current entry's place in the list, its name, and its stage.
void drawCurrent(Overlay& overlay, const OverlayContent& content) {
    const Array<ModelEntry>& models = *content.models;
    Array<Line> lines;
    lines.push(Line{.text = format("{} of {}", content.current + 1, models.count()), .size = 12});
    lines.push(Line{
        .text = models[content.current].name,
        .tail = models[content.current].variant,
        .size = 17,
        .color = kBright,
    });
    const Stage* stage = content.next != nullptr ? content.next : content.shown;
    if (stage != nullptr)
        appendStageLines(lines, *stage, content.paused);
    PrRect panel = drawPanel(overlay, Span<const Line>(lines.data(), lines.count()), {kMargin, kMargin}, {0, 0},
        content.extent.x - 2 * kMargin);
    // A bar along the panel's foot fills as the model loads, and goes once it has.
    float fraction = stage != nullptr ? loadedFraction(*stage) : 1;
    if (fraction < 1) {
        PrRect bar{panel.left + 8, panel.bottom - 4, panel.right - 8, panel.bottom - 2};
        prDrawListRect(overlay.list, bar, {.color = kHighlight});
        bar.right = bar.left + (bar.right - bar.left) * fraction;
        prDrawListRect(overlay.list, bar, {.color = kAccent});
    }
}

// Records the list of the models around the current one, down the window's right side, with the current one marked.
void drawModels(Overlay& overlay, const OverlayContent& content) {
    constexpr float kRow = 20;
    constexpr float kWidth = 280;
    const Array<ModelEntry>& models = *content.models;
    float available = content.extent.y - 2 * kMargin - 2 * 8;
    uint32_t rows = uint32_t(prF32Max(available / kRow, 1));
    if (rows > models.count())
        rows = models.count();
    // The current entry sits in the middle of the rows, except near the ends of the list.
    uint32_t first = content.current > rows / 2 ? content.current - rows / 2 : 0;
    if (first + rows > models.count())
        first = models.count() - rows;
    float left = content.extent.x - kMargin - kWidth;
    PrRect panel{left, kMargin, left + kWidth, kMargin + 2 * 8 + float(rows) * kRow};
    prDrawListCornerRect(overlay.list, prCornerRectRound(panel, 8), {.color = kBacking});
    prDrawListPushClipRect(overlay.list, PrRect{panel.left, panel.top, panel.right - 6, panel.bottom},
        PrDrawEdges_Antialiased);
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t index = first + row;
        float top = panel.top + 8 + float(row) * kRow;
        bool isCurrent = index == content.current;
        if (isCurrent) {
            PrRect marked{panel.left + 4, top, panel.right - 4, top + kRow};
            prDrawListCornerRect(overlay.list, prCornerRectRound(marked, 4), {.color = kHighlight});
        }
        String number = format("{}", index + 1);
        float numberWidth = shape(overlay, number, 11);
        drawText(overlay, number, {panel.left + 38 - numberWidth, top + 14}, 11, isCurrent ? kAccent : kDim);
        drawText(overlay, models[index].name, {panel.left + 48, top + 14}, 13, isCurrent ? kBright : kDim);
    }
    prDrawListPopClip(overlay.list);
}

} // namespace

void overlayCreate(Overlay& overlay) {
    overlay.list = prDrawListCreate();
    overlay.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    overlay.glyphs = prTextGlyphBufferCreate();
    PrBlob* fontFile = prResourceOpen("font_bundle/NotoSans[wdth,wght].ttf");
    if (fontFile == nullptr || prFontCreate(fontFile, 0, nullptr, &overlay.font) != PrStatus_Ok)
        printError("model_viewer: the overlay's font is missing, so it shows no text\n");
    prBlobRelease(fontFile);
}

void overlayDestroy(Overlay& overlay) {
    prFontRelease(overlay.font);
    prTextGlyphBufferDestroy(overlay.glyphs);
    prTextShaperDestroy(overlay.shaper);
    prDrawListRelease(overlay.list);
    overlay = Overlay{};
}

void overlayRecord(Overlay& overlay, const OverlayContent& content) {
    const Array<ModelEntry>& models = *content.models;
    prDrawListClear(overlay.list);
    prDrawListPushTransformAffine2(overlay.list, prAffine2Scale(PrVec2{content.pixelRatio, content.pixelRatio}));
    drawCurrent(overlay, content);
    if (content.listsModels)
        drawModels(overlay, content);

    // The middle of the window says what the picture cannot: that the model shown is not the current one yet, or that
    // the current one has nothing to show.
    Array<Line> middle;
    PrStringView name = models[content.current].name;
    if (content.next != nullptr && content.waited > kLoadingNoticeDelay) {
        middle.push(Line{.text = format("Loading {}", name), .size = 15, .color = kBright});
    } else if (content.next == nullptr && content.shown != nullptr && content.shown->model == nullptr) {
        middle.push(Line{.text = format("{} cannot be shown", name), .size = 17, .color = kBright});
        middle.push(Line{.text = content.shown->reason, .color = kWarning});
    }
    PrVec2 center = content.extent * 0.5f;
    float widest = content.extent.x - 2 * kMargin;
    if (middle.count() != 0)
        drawPanel(overlay, Span<const Line>(middle.data(), middle.count()), center, {0.5f, 0.5f}, widest);

    PrVec2 foot{kMargin, content.extent.y - kMargin};
    if (content.notice.size != 0) {
        Line notice{.text = content.notice, .color = kWarning};
        PrRect panel = drawPanel(overlay, Span<const Line>(&notice, 1), foot, {0, 1}, widest);
        foot.y = panel.top - 6;
    }
    Line keys{
        .text = "Left, Right: model   Page Up, Page Down: by ten   Home, End   Tab: list   A: animation   C: camera   "
            "T: tone map   Space: pause",
        .size = 11,
    };
    drawPanel(overlay, Span<const Line>(&keys, 1), foot, {0, 1}, widest);
    prDrawListPopTransform(overlay.list);
}

} // namespace model_viewer
