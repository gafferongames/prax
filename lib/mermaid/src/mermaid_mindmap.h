// Mindmaps: the model of a Mermaid mindmap, the parser that builds it from the source of `mindmap`, and its drawing,
// laid out with lib/graph as a tree from the root down.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The shape of a node, with the brackets that select it.
enum {
    MermaidMindmapShape_Default,        // text: a rounded box with a line along its bottom
    MermaidMindmapShape_Square,         // [text]
    MermaidMindmapShape_Rounded,        // (text)
    MermaidMindmapShape_Circle,         // ((text))
    MermaidMindmapShape_Bang,           // ))text((
    MermaidMindmapShape_Cloud,          // )text(
    MermaidMindmapShape_Hexagon,        // {{text}}
};
typedef uint8_t MermaidMindmapShape;

// A node, in source order, so that the root is first and each node follows its parent.
struct MermaidMindmapNode {
    MermaidSpan label;
    MermaidTextFormat format;           // Markdown for a Markdown string, and HTML otherwise
    MermaidMindmapShape shape;
    uint32_t parent;                    // kMermaidNone for the root
    uint32_t depth;                     // 0 for the root
    uint32_t section;                   // the index of the root's child that the node descends from
};

// A mindmap, as its source describes it.
struct MermaidMindmap {
    MermaidSpan title;
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidMindmapNode> nodes;
};

// Parses the source of a mindmap into `mindmap`, emptying it first. If the source does not parse, returns Syntax and
// fills a non-null `error`.
PrStatus mermaidMindmapParse(MermaidMindmap& mindmap, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the mindmap.
PrStringView mermaidMindmapText(const MermaidMindmap& mindmap, MermaidSpan span);

// Frees the mindmap's storage.
void mermaidMindmapReset(MermaidMindmap& mindmap);

// Parses the mindmap `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error` if
// the source does not parse.
PrStatus mermaidMindmapDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
