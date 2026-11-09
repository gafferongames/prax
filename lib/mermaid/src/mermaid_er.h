// Entity relationship diagrams: the model of a Mermaid ER diagram, and the parser that builds it from the source of
// `erDiagram`.

#pragma once

#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An entity, in the order that the source first names it.
struct MermaidEntity {
    MermaidSpan id;
    MermaidSpan label;                  // the alias that ["..."] gives, or the ID
    MermaidPaint style;                 // the resolved style: the entity's classes, then its style statements
};

// An attribute of an entity, in source order.
struct MermaidAttribute {
    uint32_t entity;
    MermaidSpan type;
    MermaidSpan name;
    MermaidSpan keys;                   // PK, FK and UK, separated by commas, or empty
    MermaidSpan comment;                // without its quotes, or empty
};

// How many of an entity a relationship joins, as the mark at that entity's end shows.
enum {
    MermaidCardinality_ZeroOrOne,       // |o or o|: a circle and a bar
    MermaidCardinality_ExactlyOne,      // ||: two bars
    MermaidCardinality_ZeroOrMore,      // }o or o{: a circle and a crow's foot
    MermaidCardinality_OneOrMore,       // }| or |{: a bar and a crow's foot
};
typedef uint8_t MermaidCardinality;

// A relationship between two entities.
struct MermaidErRelation {
    uint32_t source;
    uint32_t target;
    MermaidCardinality sourceCardinality;
    MermaidCardinality targetCardinality;
    bool identifying;                   // -- in place of .., drawn solid
    MermaidSpan label;
};

// An ER diagram, as its source describes it.
struct MermaidErDiagram {
    PrGraphDirection direction;
    MermaidSpan title;                  // the front matter's title, or empty
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidEntity> entities;
    RawArray<MermaidAttribute> attributes;
    RawArray<MermaidErRelation> relations;
};

// Parses the source of an ER diagram into `diagram`, emptying it first. If the source does not parse, returns Syntax
// and fills a non-null `error`.
PrStatus mermaidErParse(MermaidErDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the diagram.
PrStringView mermaidErText(const MermaidErDiagram& diagram, MermaidSpan span);

// Frees the diagram's storage.
void mermaidErReset(MermaidErDiagram& diagram);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
