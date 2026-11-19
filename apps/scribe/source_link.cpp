// The links between a tab's text and the structure that the text describes.

#include "apps/scribe/source_link.h"

#include "pr/blob.h"
#include "pr/string_ops.h"

namespace scribe {

using namespace pr;

namespace {

// Returns the path on the stage of the spec at `path` in its layer: the path without its variant selections, which
// compose the prims and properties under them at the prim's own path.
String stagePath(PrStringView path) {
    String out;
    for (size_t i = 0; i < path.size; ++i) {
        if (path.data[i] != '{') {
            out.append(path.data[i]);
            continue;
        }
        while (i < path.size && path.data[i] != '}')
            ++i;
        bool child = i + 1 < path.size && path.data[i + 1] != '.' && path.data[i + 1] != '{';
        if (child)
            out.append('/');
    }
    return out;
}

// Returns the spec of the layer that the step names: a prim or a property, by its path on the stage.
PrUsdSpecIndex specOf(const SourceLinks& links, PrStringView step) {
    PrUsdSpecIndex spec = prUsdLayerFindSpec(links.layer, step);
    if (spec != kPrUsdNone)
        return spec;
    const PrUsdSpecIndex* varied = links.varied.get(step);
    return varied != nullptr ? *varied : kPrUsdNone;
}

} // namespace

bool sourceLinksFormat(PrMimeFormat format) {
    return format == PrMimeFormat_Gltf || format == PrMimeFormat_Usda;
}

bool sourceLinksUpdate(SourceLinks& links, PrMimeFormat format, PrTextEditBuffer* buffer) {
    uint64_t version = prTextEditBufferVersion(buffer);
    if (links.format == format && links.version == version)
        return links.json != nullptr || links.layer != nullptr;
    sourceLinksReset(links);
    links.format = format;
    links.version = version;
    PrStringView text = prTextEditBufferText(buffer);
    if (format == PrMimeFormat_Gltf) {
        links.json = prJsonCreate();
        if (!prJsonParse(links.json, text, PrJsonParseFlag_Spans, nullptr)) {
            prJsonDestroy(links.json);
            links.json = nullptr;
        }
    } else if (format == PrMimeFormat_Usda) {
        PrBlob* blob = prBlobCopy(prStringBytes(text));
        if (prUsdLayerCreate(blob, nullptr, &links.layer) != PrStatus_Ok)
            links.layer = nullptr;
        prBlobRelease(blob);
        // The specs in variants, by their paths on the stage. The first spec of a path is the item's.
        const PrUsdSpec* specs = links.layer != nullptr ? prUsdLayerSpecs(links.layer) : nullptr;
        for (PrUsdSpecIndex i = 0; specs != nullptr && i < prUsdLayerSpecCount(links.layer); ++i) {
            bool item = specs[i].form == PrUsdSpecForm_Prim || specs[i].form == PrUsdSpecForm_Attribute ||
                specs[i].form == PrUsdSpecForm_Relationship;
            String path = stagePath(specs[i].path);
            if (item &&prStringFindByte(specs[i].path, '{') != specs[i].path.size && !links.varied.contains(path))
                links.varied.set(path, i);
        }
    }
    return links.json != nullptr || links.layer != nullptr;
}

void sourceLinksReset(SourceLinks& links) {
    prJsonDestroy(links.json);
    links.json = nullptr;
    prUsdLayerDestroy(links.layer);
    links.layer = nullptr;
    links.varied = Map<String, PrUsdSpecIndex>();
    links.version = ~uint64_t(0);
}

String sourceItemAt(const SourceLinks& links, uint32_t offset) {
    if (links.json != nullptr) {
        PrJsonValueIndex value = prJsonValueFromOffset(links.json, offset);
        if (value == kPrJsonMissing)
            return String();
        PrRawString raw = prJsonPointer(links.json, value);
        PrStringView pointer = prRawStringView(&raw);
        // The item is the element of a top-level array that holds the value: the pointer's first two tokens.
        size_t second = pointer.size != 0 ? prStringFindByte(prStringFrom(pointer, 1), '/') + 1 : pointer.size;
        String item;
        if (second < pointer.size) {
            size_t third = prStringFindByte(prStringFrom(pointer, second + 1), '/') + second + 1;
            PrJsonValueIndex container = kPrJsonMissing;
            PrStringView head = prStringHead(pointer, second);
            if (prJsonValueFromPointer(links.json, head, &container) == PrStatus_Ok &&
                    prJsonKind(links.json, container) == PrJsonKind_Array)
                item = prStringHead(pointer, third);
        }
        prRawStringReset(&raw);
        return item;
    }
    if (links.layer != nullptr) {
        PrUsdSpecIndex spec = prUsdLayerSpecFromOffset(links.layer, offset);
        if (spec == kPrUsdNone || spec == kPrUsdLayerSpec)
            return String();
        return stagePath(prUsdLayerSpecs(links.layer)[spec].path);
    }
    return String();
}

bool sourceItemSpan(const SourceLinks& links, PrStringView step, uint32_t* begin, uint32_t* end) {
    if (links.json != nullptr) {
        PrJsonValueIndex value = kPrJsonMissing;
        if (prJsonValueFromPointer(links.json, step, &value) != PrStatus_Ok || step.size == 0)
            return false;
        PrJsonSpan span = prJsonSpan(links.json, value);
        *begin = span.begin;
        *end = span.end;
        return true;
    }
    if (links.layer != nullptr) {
        PrUsdSpecIndex spec = specOf(links, step);
        if (spec == kPrUsdNone)
            return false;
        PrUsdTextSpan span = prUsdLayerSpecSpan(links.layer, spec);
        *begin = span.begin;
        *end = span.end;
        return span.end > span.begin;
    }
    return false;
}

} // namespace scribe
