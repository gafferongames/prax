// MaterialX documents, read through lib/xml into a tree of elements.

#include "pr/materialx.h"

#include "pr/alloc.h"
#include "pr/atom_table.h"
#include "pr/float_convert.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/xml.h"

namespace pr_impl {

namespace {

struct Document {
    AtomTable strings;              // every string of the document
    RawArray<PrMaterialxElement> elements;
    RawArray<PrMaterialxAttribute> attributes;
    RawArray<uint32_t> firstAttribute;      // by element, while the document is read
};

// What a parse keeps while it reads.
struct Reader {
    Document* document;
    RawArray<PrMaterialxElementIndex> open;      // the elements whose end has not been read, the innermost last
    RawArray<PrMaterialxElementIndex> lastChild; // by element
    HashIndex siblings;                          // the named elements, by their parent and their name
    PrTextBuffer* PR_NULLABLE diagnostics;
};

uint32_t siblingHash(PrMaterialxElementIndex parent, PrStringView name) {
    return uint32_t(prHashBytes(name.data, name.size, parent));
}

// Returns `text` as a string of the document.
PrStringView intern(Document& d, PrStringView text) {
    return atomTableText(&d.strings, atomTableAdd(&d.strings, text));
}

PrStatus fail(Reader& r, PrXmlParser* parser, PrStringView sentence) {
    if (r.diagnostics != nullptr)
        prTextBufferAppend(r.diagnostics, sentence);
    return prXmlParserFail(parser, sentence);
}

PrStatus startElement(void* PR_NULLABLE context, PrXmlParser* parser, const PrXmlName* name,
        const PrXmlAttribute* PR_NULLABLE attributes, uint32_t attributeCount) {
    Reader& r = *static_cast<Reader*>(context);
    Document& d = *r.document;
    PrMaterialxElementIndex parent = r.open.count != 0 ? r.open.data[r.open.count - 1] : kPrMaterialxNone;
    if (parent == kPrMaterialxNone && (d.elements.count != 0 || name->local != "materialx"))
        return fail(r, parser, "the document's root is not a materialx element");
    PrMaterialxElementIndex index = d.elements.count;
    PrMaterialxElement element{
        .category = intern(d, name->local),
        .parent = parent,
        .firstChild = PrMaterialxElementIndex(kPrMaterialxNone),
        .nextSibling = PrMaterialxElementIndex(kPrMaterialxNone),
        .attributes = nullptr,
    };
    d.firstAttribute.push(d.attributes.count);
    for (uint32_t i = 0; i < attributeCount; ++i) {
        // Namespace declarations, as XInclude's, are no attributes of the element.
        if (attributes[i].name.space.size != 0)
            continue;
        PrStringView key = intern(d, attributes[i].name.local);
        PrStringView value = intern(d, attributes[i].value);
        if (key == "name")
            element.name = value;
        else if (key == "type")
            element.type = value;
        d.attributes.push(PrMaterialxAttribute{key, value});
        ++element.attributeCount;
    }
    if (element.name.size != 0) {
        uint32_t hash = siblingHash(parent, element.name);
        uint32_t found = r.siblings.find(hash, [&](uint32_t at) {
            return d.elements.data[at].parent == parent && d.elements.data[at].name == element.name;
        });
        if (found != kHashMissing)
            return fail(r, parser, "two elements of one parent have the same name");
        r.siblings.insert(hash, index);
    } else if (parent != kPrMaterialxNone) {
        return fail(r, parser, "an element below the root has no name");
    }
    d.elements.push(element);
    r.lastChild.push(kPrMaterialxNone);
    if (parent != kPrMaterialxNone) {
        if (r.lastChild.data[parent] == kPrMaterialxNone)
            d.elements.data[parent].firstChild = index;
        else
            d.elements.data[r.lastChild.data[parent]].nextSibling = index;
        r.lastChild.data[parent] = index;
    }
    r.open.push(index);
    return PrStatus_Ok;
}

PrStatus endElement(void* PR_NULLABLE context, PrXmlParser*, const PrXmlName*) {
    Reader& r = *static_cast<Reader*>(context);
    r.open.pop();
    return PrStatus_Ok;
}

bool isSeparator(char c) {
    return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMaterialxDocument : Document {};

PrStatus prMaterialxDocumentCreate(PrStringView source, PrTextBuffer* PR_NULLABLE diagnostics,
        PrMaterialxDocument* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrMaterialxDocument* document = allocZeroed<PrMaterialxDocument>();
    Reader r{.document = document, .diagnostics = diagnostics};
    PrXmlHandler handler{.context = &r, .startElement = startElement, .endElement = endElement};
    PrXmlError error{};
    PrStatus status = prXmlParse(source, &handler, &error);
    if (status == PrStatus_Ok && document->elements.count == 0) {
        status = PrStatus_InvalidData;
        if (diagnostics != nullptr)
            prTextBufferAppend(diagnostics, "the document has no root");
    } else if (status != PrStatus_Ok) {
        // A failure of the handler's has said why. One of the parser's says it here.
        if (diagnostics != nullptr && prTextBufferView(diagnostics).size == 0)
            prTextBufferAppend(diagnostics, error.message);
        status = PrStatus_InvalidData;
    }
    r.open.reset();
    r.lastChild.reset();
    r.siblings.reset();
    if (status != PrStatus_Ok) {
        prMaterialxDocumentDestroy(document);
        return status;
    }
    // The attributes are in their final place once every element has been read.
    for (uint32_t i = 0; i < document->elements.count; ++i)
        document->elements.data[i].attributes = document->attributes.data + document->firstAttribute.data[i];
    document->firstAttribute.reset();
    *out = document;
    return PrStatus_Ok;
}

void prMaterialxDocumentDestroy(PrMaterialxDocument* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    atomTableReset(&document->strings);
    document->elements.reset();
    document->attributes.reset();
    document->firstAttribute.reset();
    prMemoryFree(document);
}

const PrMaterialxElement* prMaterialxDocumentElements(const PrMaterialxDocument* document, uint32_t* count) {
    *count = document->elements.count;
    return document->elements.data;
}

PrStringView prMaterialxAttribute(const PrMaterialxDocument* document, PrMaterialxElementIndex element, PrStringView name) {
    const PrMaterialxElement& e = document->elements.data[element];
    for (uint32_t i = 0; i < e.attributeCount; ++i) {
        if (e.attributes[i].name == name)
            return e.attributes[i].value;
    }
    return PrStringView{};
}

PrStringView prMaterialxScopedAttribute(const PrMaterialxDocument* document, PrMaterialxElementIndex element,
        PrStringView name) {
    for (PrMaterialxElementIndex at = element; at != kPrMaterialxNone; at = document->elements.data[at].parent) {
        PrStringView value = prMaterialxAttribute(document, at, name);
        if (value.data != nullptr)
            return value;
    }
    return PrStringView{};
}

PrMaterialxElementIndex prMaterialxChild(const PrMaterialxDocument* document, PrMaterialxElementIndex element,
        PrStringView name) {
    for (PrMaterialxElementIndex at = document->elements.data[element].firstChild; at != kPrMaterialxNone;
            at = document->elements.data[at].nextSibling) {
        if (document->elements.data[at].name == name)
            return at;
    }
    return kPrMaterialxNone;
}

bool prMaterialxParseFloats(PrStringView value, float* out, uint32_t count) {
    float parsed[16];
    if (count > 16)
        return false;
    size_t at = 0;
    uint32_t n = 0;
    while (true) {
        while (at < value.size && isSeparator(value.data[at]))
            ++at;
        if (at == value.size)
            break;
        if (n == count)
            return false;
        double number = 0;
        size_t used = prF64ParsePrefix(PrStringView{value.data + at, value.size - at}, &number);
        if (used == 0)
            return false;
        parsed[n++] = float(number);
        at += used;
        if (at < value.size && !isSeparator(value.data[at]))
            return false;
    }
    if (n != count)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = parsed[i];
    return true;
}
