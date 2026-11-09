// Requirement diagrams: the model of a Mermaid requirement diagram, and the parser that builds it from the source of
// `requirementDiagram`.

#pragma once

#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The kinds of block: the six kinds of requirement, and an element.
enum {
    MermaidRequirementKind_Requirement,
    MermaidRequirementKind_Functional,      // functionalRequirement
    MermaidRequirementKind_Interface,       // interfaceRequirement
    MermaidRequirementKind_Performance,     // performanceRequirement
    MermaidRequirementKind_Physical,        // physicalRequirement
    MermaidRequirementKind_DesignConstraint,
    MermaidRequirementKind_Element,
};
typedef uint8_t MermaidRequirementKind;

// A requirement's risk.
enum {
    MermaidRisk_None,
    MermaidRisk_Low,
    MermaidRisk_Medium,
    MermaidRisk_High,
};
typedef uint8_t MermaidRisk;

// How a requirement is verified.
enum {
    MermaidVerifyMethod_None,
    MermaidVerifyMethod_Analysis,
    MermaidVerifyMethod_Demonstration,
    MermaidVerifyMethod_Inspection,
    MermaidVerifyMethod_Test,
};
typedef uint8_t MermaidVerifyMethod;

// A requirement or an element, in source order. A requirement has an ID, a text, a risk and a verification method, and
// an element has a type and a document reference. Each of them may be absent.
struct MermaidRequirementBlock {
    MermaidRequirementKind kind;
    MermaidRisk risk;
    MermaidVerifyMethod verifyMethod;
    MermaidSpan name;
    MermaidSpan id;
    MermaidSpan text;
    MermaidSpan type;
    MermaidSpan docRef;
    MermaidPaint style;                     // the resolved style: the block's classes, then its style statements
};

// The kinds of relation.
enum {
    MermaidRequirementRelation_Contains,
    MermaidRequirementRelation_Copies,
    MermaidRequirementRelation_Derives,
    MermaidRequirementRelation_Satisfies,
    MermaidRequirementRelation_Verifies,
    MermaidRequirementRelation_Refines,
    MermaidRequirementRelation_Traces,
    MermaidRequirementRelation_Count,
};
typedef uint8_t MermaidRequirementRelation;

// A relation from one block to another, by their indices. `b <- type - a` relates a to b, as `a - type -> b` does.
struct MermaidRequirementLink {
    uint32_t source;
    uint32_t target;
    MermaidRequirementRelation relation;
};

// A requirement diagram, as its source describes it.
struct MermaidRequirementDiagram {
    PrGraphDirection direction;
    MermaidSpan title;                      // the front matter's title, or empty
    RawArray<char> text;                    // the characters of every span
    RawArray<MermaidRequirementBlock> blocks;
    RawArray<MermaidRequirementLink> links;
};

// Parses the source of a requirement diagram into `diagram`, emptying it first. If the source does not parse, or a
// relation names a block that the source does not define, returns Syntax and fills a non-null `error`.
PrStatus mermaidRequirementParse(MermaidRequirementDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the diagram.
PrStringView mermaidRequirementText(const MermaidRequirementDiagram& diagram, MermaidSpan span);

// The name of a relation, such as "satisfies".
PrStringView mermaidRequirementRelationName(MermaidRequirementRelation relation);

// Frees the diagram's storage.
void mermaidRequirementReset(MermaidRequirementDiagram& diagram);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
