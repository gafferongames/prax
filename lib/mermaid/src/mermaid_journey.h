// User journeys: the model of a Mermaid user journey, the parser that builds it from the source of `journey`, and its
// drawing, laid out as mermaid.js lays it out.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A task, in source order.
struct MermaidJourneyTask {
    MermaidSpan name;
    MermaidSpan section;                // the name of the section that the task is in, empty before any section
    double score;                       // how the actors feel about the task, from 1 for sad to 5 for happy
    uint32_t firstActor;                // the names of the task's actors, in MermaidJourney::actors
    uint32_t actorCount;
};

// A user journey, as its source describes it.
struct MermaidJourney {
    MermaidSpan title;
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidJourneyTask> tasks;
    RawArray<MermaidSpan> actors;       // each task's actors, in the order that the task lists them
};

// Parses the source of a user journey into `journey`, emptying it first. If the source does not parse, returns Syntax
// and fills a non-null `error`.
PrStatus mermaidJourneyParse(MermaidJourney& journey, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the journey.
PrStringView mermaidJourneyText(const MermaidJourney& journey, MermaidSpan span);

// Frees the journey's storage.
void mermaidJourneyReset(MermaidJourney& journey);

// Parses the user journey `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null
// `error` if the source does not parse.
PrStatus mermaidJourneyDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
