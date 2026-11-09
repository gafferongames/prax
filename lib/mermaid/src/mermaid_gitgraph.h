// Git graphs: the model of a Mermaid git graph, the parser that builds it from the source of `gitGraph`, and its
// drawing, laid out as mermaid.js lays it out.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How a commit is drawn.
enum {
    MermaidCommitType_Normal,           // a filled circle
    MermaidCommitType_Reverse,          // a circle with a cross
    MermaidCommitType_Highlight,        // a square in a square
    MermaidCommitType_Merge,            // a ring
    MermaidCommitType_CherryPick,       // a circle with a cherry
};
typedef uint8_t MermaidCommitType;

// A commit, in source order.
struct MermaidCommit {
    MermaidSpan id;
    uint32_t branch;
    uint32_t parents[2];                // the first parent, and a merge's or cherry-pick's other, or kMermaidNone
    MermaidCommitType type;             // the shape that the commit is drawn as
    bool merge;                         // the commit merges a branch, whatever its shape
    bool customId;                      // the source gives the commit's ID
    uint32_t firstTag;                  // in MermaidGitGraph::tags
    uint32_t tagCount;
};

// A branch, in the order that the source creates it.
struct MermaidGitBranch {
    MermaidSpan name;
    uint32_t head;                      // the branch's latest commit, or kMermaidNone
    double order;                       // the branch's place among the lanes: its `order`, or its index
};

// A git graph, as its source describes it.
struct MermaidGitGraph {
    MermaidSpan title;
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidCommit> commits;
    RawArray<MermaidGitBranch> branches;
    RawArray<MermaidSpan> tags;
};

// Parses the source of a git graph into `graph`, emptying it first. If the source does not parse, or uses a command as
// mermaid.js forbids, returns Syntax and fills a non-null `error`.
PrStatus mermaidGitGraphParse(MermaidGitGraph& graph, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the graph.
PrStringView mermaidGitGraphText(const MermaidGitGraph& graph, MermaidSpan span);

// Frees the graph's storage.
void mermaidGitGraphReset(MermaidGitGraph& graph);

// Parses the git graph `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error`
// if the source does not parse.
PrStatus mermaidGitGraphDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
