// State diagrams: the model of a Mermaid state diagram, and the parser that builds it from the source of `stateDiagram`
// or `stateDiagram-v2`.

#pragma once

#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a node of a state diagram is.
enum {
    MermaidStateKind_State,             // a rounded box, or a composite state if other nodes are inside it
    MermaidStateKind_Start,             // [*] as a transition's source: a filled dot
    MermaidStateKind_End,               // [*] as a transition's target: a ringed dot
    MermaidStateKind_Fork,              // <<fork>>: a bar
    MermaidStateKind_Join,              // <<join>>: a bar
    MermaidStateKind_Choice,            // <<choice>>: a diamond
    MermaidStateKind_Note,
    MermaidStateKind_Region,            // one of a composite state's concurrent regions, which -- separates
};
typedef uint8_t MermaidStateKind;

// A node: a state, a pseudostate, a note, or a concurrent region.
struct MermaidStateNode {
    MermaidSpan id;                     // empty for a start, an end, a note or a region
    MermaidSpan label;                  // the state's name as drawn, or a note's text
    MermaidSpan description;            // the lines that `State : text` adds, joined by line breaks, or empty
    MermaidStateKind kind;
    uint32_t parent;                    // the composite state or region that holds the node, or kMermaidNone
    PrGraphDirection direction;         // the direction that a composite state's `direction` sets, or Inherit
    MermaidPaint style;                 // the resolved style: the node's classes, then its style statements
    MermaidSpan link;                   // the URL that a click statement gives the state, or empty
};

// A transition between two nodes, or the link from a note to its state.
struct MermaidTransition {
    uint32_t source;
    uint32_t target;
    MermaidSpan label;
    bool note;                          // the dashed link from a note to its state, which has no arrowhead
};

// A state diagram, as its source describes it.
struct MermaidStateDiagram {
    PrGraphDirection direction;
    MermaidSpan title;                  // the front matter's title, or empty
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidStateNode> nodes;
    RawArray<MermaidTransition> transitions;
};

// Parses the source of a state diagram into `diagram`, emptying it first. If the source does not parse, returns Syntax
// and fills a non-null `error`.
PrStatus mermaidStateParse(MermaidStateDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the diagram.
PrStringView mermaidStateText(const MermaidStateDiagram& diagram, MermaidSpan span);

// Frees the diagram's storage.
void mermaidStateReset(MermaidStateDiagram& diagram);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
