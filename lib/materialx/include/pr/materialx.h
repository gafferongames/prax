// MaterialX documents (the MaterialX Specification, v1.39, "MaterialX Documents" and "Nodes"): the elements of an XML
// document of nodes, node graphs, materials and their inputs and outputs, read as a tree of elements with their
// attributes. It interprets no node: what a node computes is its consumer's, as a translation into OpenUSD's shading
// networks (pr/usd.h) or a renderer's material.
//
// What a document guarantees. prMaterialxDocumentCreate checks the source as it reads it and makes a document only if
// every check passes:
//
// - Element 0 is the document's root, a `materialx` element. Every other element has a parent of a lower index, and
//   the elements form one tree, each parent's children in the order of the source.
// - Every element below the root has a name, which none of its siblings has.
// - An element's attributes are those of its tag, in the order of the source, each name once.
//
// The module checks structure, not meaning: a type that the specification does not define, a value that does not parse
// as its type, and a connection to a node that does not exist are its consumer's to decide. XIncludes are not read.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A document. It is immutable, and any number of threads may read it.
typedef struct PrMaterialxDocument PrMaterialxDocument;

// An element's index in its document, or kPrMaterialxNone.
typedef uint32_t PrMaterialxElementIndex;
enum { kPrMaterialxNone = 0xffffffffu };

// An attribute of an element.
typedef struct PrMaterialxAttribute {
    PrStringView name;
    PrStringView value;
} PrMaterialxAttribute;

// An element: its tag, which the specification calls its category, as `nodegraph`, `input` or a node's `image`, and the
// attributes that every element of a node network may have. The views and the attributes live as long as the document.
typedef struct PrMaterialxElement {
    PrStringView category;
    PrStringView name;                       // the empty view for an element without a name
    PrStringView type;                       // `type`, or the empty view
    PrMaterialxElementIndex parent;          // kPrMaterialxNone for the root
    PrMaterialxElementIndex firstChild;      // kPrMaterialxNone for none
    PrMaterialxElementIndex nextSibling;     // kPrMaterialxNone for the last
    const PrMaterialxAttribute* attributes;
    uint32_t attributeCount;
} PrMaterialxElement;

// Reads a document from its source, an XML document whose root is a `materialx` element. On success, sets `*out` to the
// document, which the caller destroys. Returns InvalidData, with a sentence in `diagnostics` if one is given, for a
// source that is not well-formed XML or does not hold the guarantees above.
PrStatus prMaterialxDocumentCreate(PrStringView source, PrTextBuffer* PR_NULLABLE diagnostics,
    PrMaterialxDocument* PR_NULLABLE* PR_NONNULL out);

// Frees the document.
void prMaterialxDocumentDestroy(PrMaterialxDocument* PR_NULLABLE document);

// Returns the document's elements, `*count` of them, of which the first is the root.
const PrMaterialxElement* prMaterialxDocumentElements(const PrMaterialxDocument* document, uint32_t* count);

// Returns the value of the element's attribute `name`, or the null view if it has none.
PrStringView prMaterialxAttribute(const PrMaterialxDocument* document, PrMaterialxElementIndex element, PrStringView name);

// Returns the value of the attribute `name` of the element or of its nearest ancestor that has one, as the
// specification scopes `colorspace`, `fileprefix` and `geomprefix`: the null view if none has one.
PrStringView prMaterialxScopedAttribute(const PrMaterialxDocument* document, PrMaterialxElementIndex element,
    PrStringView name);

// Returns the element's child named `name`, or kPrMaterialxNone.
PrMaterialxElementIndex prMaterialxChild(const PrMaterialxDocument* document, PrMaterialxElementIndex element,
    PrStringView name);

// Reads a value of `count` floats, as the specification writes a float, a color, a vector or a matrix: numbers
// separated by commas and whitespace. Returns false, with `out` unchanged, if the value holds another number of numbers
// or anything that is not a number.
bool prMaterialxParseFloats(PrStringView value, float* out, uint32_t count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
