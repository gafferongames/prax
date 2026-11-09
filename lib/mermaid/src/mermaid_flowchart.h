// Flowcharts: the model of a Mermaid flowchart, and the parser that builds it from the source of `graph` or
// `flowchart`.

#pragma once

#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The shape of a node, with the brackets that select it, or the short name that selects it in a node's metadata, as in
// `A@{ shape: cyl }`. Each shape also has mermaid.js's longer aliases.
enum {
    MermaidShape_Rect,               // [text], rect
    MermaidShape_Round,              // (text), rounded
    MermaidShape_Stadium,            // ([text]), stadium
    MermaidShape_Subroutine,         // [[text]], fr-rect
    MermaidShape_Cylinder,           // [(text)], cyl
    MermaidShape_Circle,             // ((text)), circle
    MermaidShape_DoubleCircle,       // (((text))), dbl-circ
    MermaidShape_Asymmetric,         // >text], odd
    MermaidShape_Diamond,            // {text}, diam
    MermaidShape_Hexagon,            // {{text}}, hex
    MermaidShape_LeanRight,          // [/text/], lean-r
    MermaidShape_LeanLeft,           // [\text\], lean-l
    MermaidShape_Trapezoid,          // [/text\], trap-b
    MermaidShape_InvTrapezoid,       // [\text/], trap-t
    MermaidShape_Text,               // text: the label alone
    MermaidShape_NotchedRect,        // notch-rect
    MermaidShape_LinedRect,          // lin-rect
    MermaidShape_DividedRect,        // div-rect
    MermaidShape_SlopedRect,         // sl-rect
    MermaidShape_StackedRect,        // st-rect
    MermaidShape_TaggedRect,         // tag-rect
    MermaidShape_BowTieRect,         // bow-rect
    MermaidShape_WindowPane,         // win-pane
    MermaidShape_NotchedPentagon,    // notch-pent
    MermaidShape_CurvedTrapezoid,    // curv-trap
    MermaidShape_Delay,              // delay
    MermaidShape_HorizontalCylinder, // h-cyl
    MermaidShape_LinedCylinder,      // lin-cyl
    MermaidShape_Document,           // doc
    MermaidShape_LinedDocument,      // lin-doc
    MermaidShape_StackedDocument,    // docs
    MermaidShape_TaggedDocument,     // tag-doc
    MermaidShape_PaperTape,          // flag
    MermaidShape_Triangle,           // tri
    MermaidShape_FlippedTriangle,    // flip-tri
    MermaidShape_BraceLeft,          // brace
    MermaidShape_BraceRight,         // brace-r
    MermaidShape_Braces,             // braces
    MermaidShape_SmallCircle,        // sm-circ, without its label
    MermaidShape_FramedCircle,       // fr-circ, without its label
    MermaidShape_FilledCircle,       // f-circ, without its label
    MermaidShape_CrossedCircle,      // cross-circ, without its label
    MermaidShape_Fork,               // fork, without its label: a bar across the flow
    MermaidShape_Hourglass,          // hourglass, without its label
    MermaidShape_Bolt,               // bolt, without its label
    MermaidShape_DataStore,          // datastore
    MermaidShape_Folder,             // folder
    MermaidShape_Bucket,             // bucket
    MermaidShape_Console,            // console
    MermaidShape_Browser,            // browser
    MermaidShape_Person,             // person
    MermaidShape_Bang,               // bang
    MermaidShape_Cloud,              // cloud
};
typedef uint8_t MermaidShape;

// The marks at an end of a link.
enum {
    MermaidHead_None,
    MermaidHead_Arrow,              // > or <
    MermaidHead_Circle,             // o
    MermaidHead_Cross,              // x
};
typedef uint8_t MermaidHead;

// The strokes of a link.
enum {
    MermaidLinkStroke_Normal,             // --
    MermaidLinkStroke_Thick,              // ==
    MermaidLinkStroke_Dotted,             // -.-
    MermaidLinkStroke_Invisible,          // ~~~
};
typedef uint8_t MermaidLinkStroke;

// A node or a subgraph. Nodes and subgraphs share one set of IDs.
struct MermaidNode {
    MermaidSpan id;
    MermaidSpan label;              // the label's source, without its quotes and backticks
    MermaidTextFormat format;       // Markdown for a Markdown string, and HTML otherwise
    MermaidShape shape;
    bool isSubgraph;
    uint32_t parent;                // the subgraph that holds the node, or kMermaidNone
    PrGraphDirection direction;     // the direction of a subgraph's layout, or Inherit
    MermaidPaint style;             // the default class, then the node's classes, then its style statements
    MermaidSpan link;               // the URL that a click statement gives the node, or empty
};

// A link between two nodes.
struct MermaidEdge {
    uint32_t source;
    uint32_t target;
    MermaidSpan id;                 // the name that the source gives the link, as in `A e1@--> B`, or empty
    MermaidSpan label;              // as a node's label is
    MermaidTextFormat format;
    MermaidLinkStroke line;
    MermaidHead start;
    MermaidHead end;
    uint32_t length;                // the minimum number of layers that the link spans
    MermaidPaint style;             // the link's classes, then its linkStyle or else the default linkStyle
};

// A flowchart, as its source describes it.
struct MermaidFlowchart {
    PrGraphDirection direction;
    MermaidSpan title;              // the front matter's title, as plain text, or empty
    RawArray<char> text;            // the characters of every span
    RawArray<MermaidNode> nodes;
    RawArray<MermaidEdge> edges;
};

// Parses the source of a flowchart into `chart`, emptying it first. If the source does not parse, returns Syntax and
// fills a non-null `error`.
PrStatus mermaidFlowchartParse(MermaidFlowchart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the flowchart.
PrStringView mermaidFlowchartText(const MermaidFlowchart& chart, MermaidSpan span);

// Returns true if `node` is the subgraph `ancestor` or one of its descendants.
bool mermaidFlowchartHolds(const MermaidFlowchart& chart, uint32_t ancestor, uint32_t node);

// Frees the flowchart's storage.
void mermaidFlowchartReset(MermaidFlowchart& chart);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
