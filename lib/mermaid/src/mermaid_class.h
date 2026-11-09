// Class diagrams: the model of a Mermaid class diagram, and the parser that builds it from the source of
// `classDiagram`.

#pragma once

#include "pr/graph.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a node of a class diagram is.
enum {
    MermaidClassNodeKind_Class,         // a box with the class's name, attributes and methods
    MermaidClassNodeKind_Interface,     // the name at the far end of a lollipop, without a box
    MermaidClassNodeKind_Note,
};
typedef uint8_t MermaidClassNodeKind;

// A node: a class, an interface or a note.
struct MermaidClassNode {
    MermaidSpan id;                     // the class's name without its generic type, or empty for other nodes
    MermaidSpan label;                  // the name as drawn, with its generic type, or a note's text
    MermaidSpan annotation;             // the first annotation, such as interface, without its brackets, or empty
    MermaidSpan link;                   // the URL that the last click or link statement gives a class, or empty
    MermaidClassNodeKind kind;
    uint32_t space;                     // the namespace that holds the node, or kMermaidNone
    MermaidPaint style;                 // the resolved style: the node's classes, then its style statements
};

// How a member's text is styled.
enum {
    MermaidClassifier_None,
    MermaidClassifier_Static,           // $: underlined
    MermaidClassifier_Abstract,         // *: italic
};
typedef uint8_t MermaidClassifier;

// An attribute or a method of a class, in the order that the source gives them.
struct MermaidClassMember {
    uint32_t node;
    MermaidSpan text;                   // as drawn: a method's parameters in parentheses, then its return type
    bool method;
    MermaidClassifier classifier;
};

// The mark at an end of a relation.
enum {
    MermaidRelationEnd_None,
    MermaidRelationEnd_Extension,       // <| or |>: a hollow triangle
    MermaidRelationEnd_Composition,     // *: a filled diamond
    MermaidRelationEnd_Aggregation,     // o: a hollow diamond
    MermaidRelationEnd_Dependency,      // < or >: an arrowhead
    MermaidRelationEnd_Lollipop,        // (): a circle
};
typedef uint8_t MermaidRelationEnd;

// A relation between two nodes, or the link from a note to its class.
struct MermaidRelation {
    uint32_t source;
    uint32_t target;
    MermaidRelationEnd start;
    MermaidRelationEnd end;
    bool dashed;                        // .. in place of --
    bool note;                          // the dotted link from a note to its class
    MermaidSpan label;
    MermaidSpan sourceCardinality;
    MermaidSpan targetCardinality;
};

// A namespace, which groups the classes declared inside it.
struct MermaidNamespace {
    MermaidSpan name;
    uint32_t parent;                    // the enclosing namespace, or kMermaidNone
};

// A class diagram, as its source describes it.
struct MermaidClassDiagram {
    PrGraphDirection direction;
    MermaidSpan title;                  // the front matter's title, or empty
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidClassNode> nodes;
    RawArray<MermaidClassMember> members;
    RawArray<MermaidRelation> relations;
    RawArray<MermaidNamespace> namespaces;
};

// Parses the source of a class diagram into `diagram`, emptying it first. If the source does not parse, returns Syntax
// and fills a non-null `error`.
PrStatus mermaidClassParse(MermaidClassDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the diagram.
PrStringView mermaidClassText(const MermaidClassDiagram& diagram, MermaidSpan span);

// Frees the diagram's storage.
void mermaidClassReset(MermaidClassDiagram& diagram);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
