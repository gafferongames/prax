// The links between a tab's text and the structure that the text describes: the item of a glTF asset or a USD layer
// that a place in the text belongs to, and the text of an item.

#pragma once

#include "pr/json.h"
#include "pr/map.h"
#include "pr/mime_sniff.h"
#include "pr/string.h"
#include "pr/text_edit.h"
#include "pr/usd.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

using namespace pr;

// The structure of a text as its last version reads, with where each item's text is. Only a .gltf asset's JSON and a
// .usda layer have one.
struct SourceLinks {
    PrMimeFormat format = PrMimeFormat_None;
    uint64_t version = ~uint64_t(0);        // the buffer's version that was read

    // What the text reads as: the JSON of a glTF asset, with spans, or a USD layer. Null if the text does not read.
    PrJson* PR_NULLABLE json = nullptr;
    PrUsdLayer* PR_NULLABLE layer = nullptr;
    Map<String, PrUsdSpecIndex> varied;     // the layer's prims and properties in variants, by their paths on the stage

    // What a tab's view has synchronized: the selection whose text it showed last, and the caret that it has seen.
    String revealed;
    uint32_t caret = 0xffffffffu;
};

// Returns true if the text of a resource of `format` links to its structure.
bool sourceLinksFormat(PrMimeFormat format);

// Reads the text of `buffer` as `format` if the buffer changed since the links read it. Returns false if the text does
// not read, in which case the links name no item until it does.
bool sourceLinksUpdate(SourceLinks& links, PrMimeFormat format, PrTextEditBuffer* buffer);

// Frees what the links read.
void sourceLinksReset(SourceLinks& links);

// Returns the item that the byte at `offset` belongs to, as a step of the resource's address, or empty if the byte is
// in no item. A glTF item is an element of the asset's top-level arrays, as `/nodes/3`. A USD item is a prim or a
// property, as `/World/Car.size`, with a prim in a variant named by its path on the stage.
String sourceItemAt(const SourceLinks& links, uint32_t offset);

// Writes the bytes of the text of the item that `step` names, which sourceItemAt gives, to `*begin` and `*end`. Returns
// false if the text holds no such item.
bool sourceItemSpan(const SourceLinks& links, PrStringView step, uint32_t* begin, uint32_t* end);

} // namespace scribe

PR_ASSUME_NONNULL_END
