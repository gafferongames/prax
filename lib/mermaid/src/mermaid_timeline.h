// Timelines: the model of a Mermaid timeline, the parser that builds it from the source of `timeline`, and its drawing,
// laid out as mermaid.js lays out its classic timeline.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A period of time, in source order, with its events.
struct MermaidTimelinePeriod {
    MermaidSpan name;
    uint32_t section;                   // in MermaidTimeline::sections, or kMermaidNone before any section
    uint32_t firstEvent;                // in MermaidTimeline::events
    uint32_t eventCount;
};

// A timeline, as its source describes it.
struct MermaidTimeline {
    MermaidSpan title;
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidSpan> sections;
    RawArray<MermaidTimelinePeriod> periods;
    RawArray<MermaidSpan> events;       // each period's events, in source order
};

// Parses the source of a timeline into `timeline`, emptying it first. If the source does not parse, returns Syntax and
// fills a non-null `error`.
PrStatus mermaidTimelineParse(MermaidTimeline& timeline, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the timeline.
PrStringView mermaidTimelineText(const MermaidTimeline& timeline, MermaidSpan span);

// Frees the timeline's storage.
void mermaidTimelineReset(MermaidTimeline& timeline);

// Parses the timeline `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error`
// if the source does not parse.
PrStatus mermaidTimelineDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
