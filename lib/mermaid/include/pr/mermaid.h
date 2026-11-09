// Mermaid diagrams: text in Mermaid's language, parsed, laid out and drawn.
//
// Creating a diagram parses its source, shapes its text and lays it out, once. The client can then draw it any number
// of times, in a light or a dark color scheme. Diagrams use Mermaid's classic look. A source's configuration is read
// only for its theme, and the rest is ignored.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/geom.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Kinds and themes

// The kinds of diagram that a source can name.
enum {
    PrMermaidKind_Flowchart,
    PrMermaidKind_Sequence,
    PrMermaidKind_Class,
    PrMermaidKind_State,
    PrMermaidKind_Er,
    PrMermaidKind_Gantt,
    PrMermaidKind_Pie,
    PrMermaidKind_GitGraph,
    PrMermaidKind_Mindmap,
    PrMermaidKind_Timeline,
    PrMermaidKind_Journey,
    PrMermaidKind_Quadrant,
    PrMermaidKind_XyChart,
    PrMermaidKind_Sankey,
    PrMermaidKind_Radar,
    PrMermaidKind_Treemap,
    PrMermaidKind_Block,
    PrMermaidKind_Packet,
    PrMermaidKind_Kanban,
    PrMermaidKind_Requirement,
    PrMermaidKind_C4,
    PrMermaidKind_Architecture,
    PrMermaidKind_Venn,
    PrMermaidKind_UseCase,
    PrMermaidKind_Swimlane,
    PrMermaidKind_Agentflow,
    PrMermaidKind_TreeView,
    PrMermaidKind_Ishikawa,
    PrMermaidKind_Wardley,
    PrMermaidKind_Cynefin,
    PrMermaidKind_EventModeling,
    PrMermaidKind_Railroad,
    PrMermaidKind_ZenUml,
    PrMermaidKind_Info,
    PrMermaidKind_Error,            // a diagram that shows why its source failed (prMermaidCreateError)
    PrMermaidKind_Count,
};
typedef uint8_t PrMermaidKind;

// Returns true if `source` names a kind of diagram, after its front matter, directives and comments, and writes the
// kind to `out`.
bool prMermaidKindFromSource(PrStringView source, PrMermaidKind* out);

// The name of a kind, as a source writes it, such as "sequenceDiagram".
PrStringView prMermaidKindName(PrMermaidKind kind);

// ---------------------------------------------------------------------------------------------------------------------
// Diagrams

// A diagram, laid out and drawn.
typedef struct PrMermaid PrMermaid;

// The inputs to a diagram.
typedef struct PrMermaidDesc {
    PrStringView source;                        // UTF-8

    // The fonts of the diagram's text, and what shapes it. Both are used only while the diagram is created.
    PrFontSet* fonts;
    PrTextShaper* shaper;
} PrMermaidDesc;

// What a host draws in a diagram's text: a draw list with its origin at the top left of its extent, in px. The diagram
// takes the list's reference. `baseline` is the distance from the top to the baseline that aligns it with text.
typedef struct PrMermaidEmbed {
    PrDrawList* PR_NULLABLE list;
    PrVec2 extent;
    float baseline;
} PrMermaidEmbed;

// Typesets the TeX of a label's $$...$$ in display style, at `fontSize` px in `color`. Returns false if the host cannot
// typeset it, and the diagram then shows the TeX as text.
typedef bool (*PrMermaidMathFn)(void* PR_NULLABLE context, PrStringView tex, float fontSize, PrColor color,
    PrMermaidEmbed* out);

// How a diagram's text is set beyond what its source says.
typedef struct PrMermaidOptions {
    PrMermaidMathFn PR_NULLABLE math;           // null shows math as its TeX
    void* PR_NULLABLE mathContext;
} PrMermaidOptions;

// No math.
PrMermaidOptions prMermaidOptionsDefault(void);

// Where a source failed to parse, and why. Lines and columns count from one, and columns count bytes.
typedef struct PrMermaidError {
    uint32_t line;
    uint32_t column;
    PrStringView message;                       // static text
} PrMermaidError;

// Creates the diagram that `desc` describes. A null `options` means the defaults. Returns Syntax if the source does not
// parse, NotSupported if the engine does not draw that kind of diagram, and Ok otherwise. On failure, `*out` is null,
// and a non-null `error` receives the location and reason.
PrStatus prMermaidCreate(const PrMermaidDesc* desc, const PrMermaidOptions* PR_NULLABLE options,
    PrMermaid* PR_NULLABLE* PR_NONNULL out, PrMermaidError* PR_NULLABLE error);

// Creates the diagram that shows `error`, which a failed prMermaidCreate reported for `desc`, as mermaid.js shows a
// source that it cannot draw.
PrMermaid* prMermaidCreateError(const PrMermaidDesc* desc, const PrMermaidOptions* PR_NULLABLE options,
    const PrMermaidError* error);

// Frees the diagram.
void prMermaidDestroy(PrMermaid* PR_NULLABLE diagram);

// The kind of the diagram.
PrMermaidKind prMermaidKind(const PrMermaid* diagram);

// The extent of the diagram, in px, from its top left.
PrVec2 prMermaidExtent(const PrMermaid* diagram);

// The color scheme that the diagram's source names, as `theme: dark` or %%{init: {'theme': 'dark'}}%% does. Returns
// false if it names no theme. A source that names another theme than dark names the light scheme.
bool prMermaidScheme(const PrMermaid* diagram, PrColorScheme* out);

// The background that the diagram is drawn on in `scheme`, which its drawing does not fill. A host that has no
// background of its own fills it behind the diagram.
PrColor prMermaidBackground(const PrMermaid* diagram, PrColorScheme scheme);

// The text that the source gives for readers of assistive technology: its accTitle, or its title, and its accDescr.
// Either may be empty. The text is valid until the diagram is destroyed.
PrStringView prMermaidAccessibleTitle(const PrMermaid* diagram);
PrStringView prMermaidAccessibleDescription(const PrMermaid* diagram);

// Records the diagram into `list`, with its top left at `origin`, in `scheme`: Mermaid's default theme for a light
// scheme and its dark theme for a dark one. A source that names a theme is drawn in that theme's scheme instead.
void prMermaidDraw(const PrMermaid* diagram, PrDrawList* list, PrVec2 origin, PrColorScheme scheme);

// ---------------------------------------------------------------------------------------------------------------------
// Items

// The kinds of item.
enum {
    PrMermaidItemKind_Node,         // a box that the diagram places, such as a flowchart's node or a sequence's actor
    PrMermaidItemKind_Group,        // a box that encloses nodes, such as a subgraph or a composite state
    PrMermaidItemKind_Edge,         // a line between nodes
    PrMermaidItemKind_Label,        // the box of a piece of text
};
typedef uint8_t PrMermaidItemKind;

// A part of the diagram as the layout placed it, in the diagram's coordinates.
typedef struct PrMermaidItem {
    PrMermaidItemKind kind;
    PrRect rect;                    // the box, or for an edge the bounds of its route
    PrStringView id;                // the source's name for the item, or empty
    PrStringView link;              // the URL that a click statement gives the item, or empty

    // An edge's route as a polyline, from its source to its target.
    const PrVec2* PR_NULLABLE points;
    uint32_t pointCount;
} PrMermaidItem;

// The items, in the order that the diagram draws them. Views and points are valid until the diagram is destroyed.
uint32_t prMermaidItemCount(const PrMermaid* diagram);
PrMermaidItem prMermaidItem(const PrMermaid* diagram, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
