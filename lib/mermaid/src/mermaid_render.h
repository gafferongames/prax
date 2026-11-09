// Rendering: what every kind of diagram draws with. A render holds the diagram's fonts, the labels that its layout
// measures, and the scene that it records, with the items that it reports.
//
// A diagram is laid out once, as it is created: its layout measures labels, places them, and records shapes and glyphs
// into a scene, in the order that they paint. Colors are recorded as inks, so that a scene draws in any theme without
// being laid out again.

#pragma once

#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/path.h"
#include "pr/raw_array.h"
#include "pr/text_layout.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The absent index of anything that a diagram numbers.
constexpr uint32_t kMermaidNone = 0xffffffffu;

// A range of a diagram's text.
struct MermaidSpan {
    uint32_t start;
    uint32_t size;
};

// ---------------------------------------------------------------------------------------------------------------------
// Text

// How a label's source is written. Every format decodes Mermaid's entity codes, such as #quot; and #9829;, and <br>.
enum {
    MermaidTextFormat_Plain,
    MermaidTextFormat_Html,         // HTML's tags for emphasis, code, color and decoration, and its references
    MermaidTextFormat_Markdown,     // a Markdown string: strong and emphasis, with its lines' indentation removed
};
typedef uint8_t MermaidTextFormat;

// The style that a label's text starts in.
struct MermaidTextStyle {
    float size;                     // px
    float weight;                   // 400 for normal, 700 for bold
    bool italic;
    bool monospace;
    PrTextAlign align;

    // The width at which lines wrap, in px, or zero for lines that break only where the source breaks them.
    float wrapWidth;

    float lineHeight;               // from the top of one line to the next, in px, or zero for the font's own
};

// A style of `size` px, normal, centered, unwrapped, and in the font's own line height.
MermaidTextStyle mermaidTextStyle(float size);

// A label: text shaped and set in lines, which a layout measures and then draws.
typedef uint32_t MermaidLabelIndex;

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// ---------------------------------------------------------------------------------------------------------------------
// Inks

// The colors that a theme gives, by the role that they play, as mermaid.js's themes name their variables.
enum {
    MermaidRole_None,                   // no role: a color of the source's own
    MermaidRole_Background,             // the page behind the diagram, which the diagram does not fill
    MermaidRole_Text,                   // textColor
    MermaidRole_Title,                  // titleColor
    MermaidRole_Line,                   // lineColor
    MermaidRole_NodeFill,               // mainBkg
    MermaidRole_NodeStroke,             // nodeBorder
    MermaidRole_NodeText,               // the text of nodes: nodeTextColor, or textColor
    MermaidRole_ClusterFill,            // clusterBkg
    MermaidRole_ClusterStroke,          // clusterBorder
    MermaidRole_EdgeLabelBackground,    // edgeLabelBackground
    MermaidRole_ErrorIcon,              // errorBkgColor
    MermaidRole_ErrorText,              // errorTextColor
    MermaidRole_StrongText,             // the text of a sequence's actors, notes and blocks: actorTextColor and others
    MermaidRole_NoteFill,               // noteBkgColor
    MermaidRole_NoteStroke,             // noteBorderColor
    MermaidRole_ActivationFill,         // activationBkgColor
    MermaidRole_ActivationStroke,       // activationBorderColor
    MermaidRole_SequenceNumberText,     // sequenceNumberColor
    MermaidRole_AltBackground,          // altBackground: the background of a state's concurrent regions
    MermaidRole_RowOdd,                 // rowOdd: an entity's first attribute row, and every other one after it
    MermaidRole_RowEven,                // rowEven: the rows between them
    MermaidRole_GanttBandA,             // sectionBkgColor: the rows of a gantt chart's first section of every four
    MermaidRole_GanttBandB,             // sectionBkgColor2: the rows of its third section of every four
    MermaidRole_GanttGrid,              // gridColor
    MermaidRole_GanttExclude,           // excludeBkgColor: the days that excludes skips
    MermaidRole_GanttTaskFill,          // taskBkgColor
    MermaidRole_GanttTaskStroke,        // taskBorderColor
    MermaidRole_GanttTaskText,          // taskTextColor: a task's name inside its bar
    MermaidRole_GanttActiveFill,        // activeTaskBkgColor
    MermaidRole_GanttDoneFill,          // doneTaskBkgColor
    MermaidRole_GanttDoneStroke,        // doneTaskBorderColor
    MermaidRole_GanttCritFill,          // critBkgColor
    MermaidRole_GanttCritStroke,        // critBorderColor
    MermaidRole_GanttVertLine,          // vertLineColor: a vert marker's line
    MermaidRole_Pie1,                   // pie1 to pie12: the slices, in turn
    MermaidRole_Pie2,
    MermaidRole_Pie3,
    MermaidRole_Pie4,
    MermaidRole_Pie5,
    MermaidRole_Pie6,
    MermaidRole_Pie7,
    MermaidRole_Pie8,
    MermaidRole_Pie9,
    MermaidRole_Pie10,
    MermaidRole_Pie11,
    MermaidRole_Pie12,
    MermaidRole_Count,
};
typedef uint8_t MermaidRole;

// A color as a diagram records it: a theme's color for a role, or a color without a role. A color of the source's own
// is drawn alike in every theme. A diagram's own pair of colors has one for each scheme.
struct MermaidInk {
    MermaidRole role;
    bool paired;                        // the role is None and `dark` replaces `color` in the dark scheme
    PrColor8 color;                     // the color when the role is None
    PrColor8 dark;
};

// The ink of a role, of a color of the source's own, or of a diagram's colors for the light and dark schemes.
MermaidInk mermaidInk(MermaidRole role);
MermaidInk mermaidInkOfColor(PrColor8 color);
MermaidInk mermaidInkOfColors(PrColor8 light, PrColor8 dark);

// The color of `ink` in the theme of `scheme`: Mermaid's default theme, or its dark theme.
PrColor8 mermaidInkColor(MermaidInk ink, PrColorScheme scheme);

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// The properties of a paint that are set.
enum {
    MermaidPaintFlag_Fill = 1u << 0,
    MermaidPaintFlag_Stroke = 1u << 1,
    MermaidPaintFlag_Color = 1u << 2,
    MermaidPaintFlag_StrokeWidth = 1u << 3,
    MermaidPaintFlag_Dash = 1u << 4,
    MermaidPaintFlag_FontWeight = 1u << 5,
    MermaidPaintFlag_FontStyle = 1u << 6,
    MermaidPaintFlag_FontSize = 1u << 7,
    MermaidPaintFlag_Opacity = 1u << 8,
};
typedef uint16_t MermaidPaintFlags;

// The CSS properties of a shape and its text that a diagram draws: what a theme gives, and what classDef, style and
// linkStyle statements set on top of it. `flags` says which are set.
struct MermaidPaint {
    MermaidPaintFlags flags;
    MermaidInk fill;
    MermaidInk stroke;
    MermaidInk color;                   // the color of text
    float strokeWidth;
    float dash[2];                      // stroke-dasharray's first dash and gap
    float fontWeight;
    bool italic;
    float fontSize;
    float opacity;
};

// Reads CSS declarations, separated by commas or semicolons outside parentheses, into `paint`.
void mermaidPaintParse(PrStringView declarations, MermaidPaint& paint);

// Sets in `into` the properties that `from` sets.
void mermaidPaintMerge(MermaidPaint& into, const MermaidPaint& from);

// The marks at the ends of lines.
enum {
    MermaidMarker_None,
    MermaidMarker_Arrow,                // a filled triangle
    MermaidMarker_OpenArrow,            // two strokes, as a sequence diagram's async message has
    MermaidMarker_Circle,               // a filled circle
    MermaidMarker_Cross,
    MermaidMarker_Triangle,             // a hollow triangle: inheritance or realization
    MermaidMarker_Diamond,              // a filled diamond: composition
    MermaidMarker_OpenDiamond,          // a hollow diamond: aggregation
    MermaidMarker_Lollipop,             // a hollow circle at the end: a provided interface
};
typedef uint8_t MermaidMarker;

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

// What a scene's command draws.
enum {
    MermaidCommandKind_Path,            // a path, filled and then stroked
    MermaidCommandKind_Glyphs,          // a run of glyphs, filled
    MermaidCommandKind_PushTransform,   // a transform that the commands up to its pop draw in
    MermaidCommandKind_PopTransform,
};
typedef uint8_t MermaidCommandKind;

// A command of a scene.
struct MermaidCommand {
    MermaidCommandKind kind;
    bool filled;                        // Path
    bool stroked;                       // Path
    bool dashed;                        // Path
    MermaidInk fill;                    // Path, Glyphs
    MermaidInk stroke;                  // Path
    float strokeWidth;                  // Path
    float dash[2];                      // Path
    float opacity;                      // Path: of the fill
    PrPath* PR_NULLABLE path;           // Path, which the scene owns
    uint32_t run;                       // Glyphs: in MermaidScene::runs
    PrAffine2 transform;                // PushTransform
};

// A run of glyphs that a scene draws. The scene holds a reference to its font.
struct MermaidGlyphRun {
    const PrFont* font;
    float size;
    uint32_t start;                     // its glyphs and their transforms, in MermaidScene::glyphs and ::transforms
    uint32_t count;
    bool scaled;                        // true if its glyphs have transforms
    PrVec2 origin;
};

// A diagram's drawing, in any theme: what it draws, in order, in inks that each theme colors.
struct MermaidScene {
    RawArray<MermaidCommand> commands;
    RawArray<MermaidGlyphRun> runs;
    RawArray<PrFontGlyph> glyphs;
    RawArray<PrAffine2> transforms;     // by glyph, for runs that are scaled
};

// Records the scene into `list`, in the theme of `scheme`.
void mermaidSceneDraw(const MermaidScene& scene, PrDrawList* list, PrColorScheme scheme);

// Frees the scene's storage.
void mermaidSceneReset(MermaidScene& scene);

// ---------------------------------------------------------------------------------------------------------------------
// Renders

// A text font of the render's, by the style that it sets.
struct MermaidFont {
    float size;
    float weight;
    bool italic;
    bool monospace;
    PrTextFont* font;
};

// A range of a label's text that has its own color, or is underlined or struck through.
struct MermaidLabelSpan {
    uint32_t start;
    uint32_t end;
    bool underline;
    bool strike;
    bool hasColor;
    PrColor8 color;
};

// A label's layout and extent, and the text that it sets.
struct MermaidLabel {
    PrTextLayout* PR_NULLABLE layout;   // null for empty text
    PrVec2 extent;
    MermaidSpan text;                   // in MermaidRender::labelText
    uint32_t spanStart;                 // the label's spans, in MermaidRender::labelSpans
    uint32_t spanCount;
};

// An item (PrMermaidItem), with its text and points held by the render.
struct MermaidItemRecord {
    PrMermaidItemKind kind;
    PrRect rect;
    MermaidSpan id;                     // in MermaidRender::itemText
    MermaidSpan link;
    uint32_t pointStart;                // in MermaidRender::itemPoints
    uint32_t pointCount;
};

// What a diagram is laid out with, and what it records.
struct MermaidRender {
    PrFontSet* fonts;
    PrTextShaper* shaper;
    PrMermaidOptions options;
    PrMermaidKind kind;

    RawArray<MermaidFont> fontCache;
    RawArray<MermaidLabel> labels;
    RawArray<MermaidLabelSpan> labelSpans;
    RawArray<char> labelText;

    MermaidScene scene;
    RawArray<MermaidItemRecord> items;
    RawArray<char> itemText;
    RawArray<PrVec2> itemPoints;

    PrVec2 extent;                      // the diagram's extent, which its layout sets
};

// Sets up a render of `desc`, with an empty scene.
void mermaidRenderInit(MermaidRender& render, const PrMermaidDesc& desc, const PrMermaidOptions& options);

// Frees what the render holds.
void mermaidRenderReset(MermaidRender& render);

// The text font for `style`: Mermaid's font list, "trebuchet ms", verdana, arial and sans-serif, after the monospace
// family for monospace text.
PrTextFont* mermaidFont(MermaidRender& render, const MermaidTextStyle& style);

// Shapes `text`, written in `format`, in `style`. Empty text gives a label of zero extent.
MermaidLabelIndex mermaidLabelCreate(MermaidRender& render, PrStringView text, MermaidTextFormat format,
    const MermaidTextStyle& style);

// The extent of a label, and its number of lines, which is one for empty text. kMermaidNone is a label of no text,
// which has zero extent and draws nothing.
PrVec2 mermaidLabelExtent(const MermaidRender& render, MermaidLabelIndex label);
uint32_t mermaidLabelLineCount(const MermaidRender& render, MermaidLabelIndex label);

// Records a label with its top left at `origin`, in `ink` unless its text sets a color of its own.
void mermaidLabelDraw(MermaidRender& render, MermaidLabelIndex label, PrVec2 origin, MermaidInk ink);

// Records a label centered on `center`.
void mermaidLabelDrawCentered(MermaidRender& render, MermaidLabelIndex label, PrVec2 center, MermaidInk ink);

// Decodes `text` as a label in `format` would read it, and appends the characters without their styles to `out`.
void mermaidTextDecode(PrStringView text, MermaidTextFormat format, RawArray<char>& out);

// Records `path` filled and then stroked as `paint` says. A paint without a fill or a stroke records none.
void mermaidDrawPath(MermaidRender& render, const PrPath* path, const MermaidPaint& paint);

// Records `path` stroked in `ink` at `width`, dashed if `dash` is not null.
void mermaidStrokePath(MermaidRender& render, const PrPath* path, MermaidInk ink, float width, const float* PR_NULLABLE dash);

// Records `path` filled in `ink`.
void mermaidFillPath(MermaidRender& render, const PrPath* path, MermaidInk ink);

// Records a rectangle filled in `ink`.
void mermaidFillRect(MermaidRender& render, PrRect rect, MermaidInk ink);

// Records a line between two points.
void mermaidDrawLine(MermaidRender& render, PrVec2 from, PrVec2 to, MermaidInk ink, float width,
    const float* PR_NULLABLE dash);

// Records the mark `marker` with its tip at `tip`, pointing along `direction`, which must be a unit vector. A hollow
// mark is filled with the background. Returns how far back from the tip a line should stop to meet the mark.
float mermaidDrawMarker(MermaidRender& render, MermaidMarker marker, PrVec2 tip, PrVec2 direction, MermaidInk ink, float size);

// Records a transform that the drawing up to its pop is in.
void mermaidPushTransform(MermaidRender& render, PrAffine2 transform);
void mermaidPopTransform(MermaidRender& render);

// Adds an item. `id` and `link` may be empty.
void mermaidItemAdd(MermaidRender& render, PrMermaidItemKind kind, PrRect rect, PrStringView id, PrStringView link);

// Adds an edge, whose rectangle is the bounds of its points.
void mermaidItemAddEdge(MermaidRender& render, const PrVec2* points, uint32_t count, PrStringView id);

// Adds an edge along `path`, flattened.
void mermaidItemAddPath(MermaidRender& render, const PrPath* path, PrStringView id);

// Returns the unit direction in which a route arrives at its start or its end, pointing from the rest of the route
// toward that end. A route is a polyline, or a spline as PrGraphRoute lists one.
PrVec2 mermaidRouteEndDirection(const PrVec2* points, uint32_t count, bool atStart);

// Moves the start or the end of a route by `delta`. A spline's adjacent control point moves with it, so that the
// tangent at that end is unchanged.
void mermaidRouteMoveEnd(PrVec2* points, uint32_t count, bool atStart, PrVec2 delta);

// Appends a spline route, as PrGraphRoute lists one, to `path`. A segment whose control points lie on its chord is
// recorded as a line.
void mermaidRouteAppend(PrPath* path, const PrVec2* points, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// Graph diagrams
//
// What the diagrams that lib/graph lays out share: the title above the graph, the margin around it, and the edges.

// The ends of an edge that get a port of their own. The layout spreads the ports of a node along its side, so that its
// edges meet it at separate points.
enum {
    MermaidGraphPortFlag_Source = 1u << 0,
    MermaidGraphPortFlag_Target = 1u << 1,
    MermaidGraphPortFlag_Both = MermaidGraphPortFlag_Source | MermaidGraphPortFlag_Target,
};
typedef uint8_t MermaidGraphPortFlags;

// Adds an edge from `source` to `target`, with a free port at each end that `ports` names. A self-loop gets no ports.
// An end with a port must not be a group.
PrGraphEdge mermaidGraphAddEdge(PrGraph* graph, PrGraphNode source, PrGraphNode target, MermaidGraphPortFlags ports);

// Places a laid-out graph in the render, inside a margin of 8 px, below `title`. Sets the render's extent, which is
// wide enough for the title, and records the title centered at the top, `titleGap` above the graph. An empty title
// takes no space. Returns the offset from the graph's coordinates to the render's.
PrVec2 mermaidGraphPlace(MermaidRender& render, const PrGraph* graph, PrStringView title,
    const MermaidTextStyle& titleStyle, float titleGap, MermaidInk titleInk);

// Appends the route of `edge`, moved by `offset`, to `points`. Returns false if the route has fewer than two points,
// which draw nothing.
bool mermaidGraphRouteAppend(const PrGraph* graph, PrGraphEdge edge, PrVec2 offset, RawArray<PrVec2>& points);

// Records an edge's label centered in its place in the layout, moved by `offset`, over a fill of `background` if it is
// not null, and adds the label's item.
void mermaidGraphLabelDraw(MermaidRender& render, const PrGraph* graph, PrGraphLabel place, PrVec2 offset,
    MermaidLabelIndex label, MermaidInk ink, const MermaidInk* PR_NULLABLE background);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
