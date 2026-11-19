// The USD corpus harness, which tools/suite/usd.py drives.
//
// `usd_corpus <location> <dump>...` reads the layer at each location, which is a file or a file in a package as
// "a.usdz[b.usdc]", and writes what the layer holds to two files, `<dump>.strict` and `<dump>.loose`. It writes a line
// of JSON for each location: {"status": the name of the status of reading, "diagnostics": its sentence, "format": the
// name of its format (formatName), "version": the version that the file states, "specs": the number of specs}.
// `usd_corpus --package <file>` writes one line of JSON for a package: {"status", "diagnostics", "conforming": whether
// the package keeps the specification's rules of layout, "files": the paths of its files}.
//
// A dump is the layer in a canonical text that tools/suite/usd_reference.py writes of the same layer as the reference
// reads it, so that two dumps of one layer are equal as text:
//
//   S <form> <path>               a spec, followed by its lists of children, its fields and its time samples
//   C <list> "<name>"...          a list of children that is not empty, in order
//   F <field> <value>             a field, in order of name
//   T <time> <value>              a time sample of the field `timeSamples`, in order of time
//
// A number of a float type is written as the hexadecimal of its bytes, least significant first, so that equal text
// means equal bits. An array of more than eight elements is written as its length and the SHA-1 of its elements' bytes,
// in the order and layout that the file format gives them: a matrix by rows, a quaternion with its real part last, and
// a string with a zero byte after it. The loose dump differs from the strict one inside dictionaries alone: the
// reference gives the scalars of a dictionary to Python without their types, so there an integer of any width is `int`,
// a float of any width is `num` with the bytes of its value as a double, and a string or a token is `str`.

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/sha1.h"
#include "pr/sort.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/time.h"
#include "pr/usd.h"
#include "pr/usd_geom.h"
#include "pr/usd_model.h"
#include "pr/usd_stage.h"
#include "pr/vec.h"

#include "tools/harness/harness.h"
#include "usd/src/usd_compose.h"

using namespace harness;
using namespace pr;
using namespace pr_impl;

namespace {

// The set of every compression codec, which the corpus's reads share for the whole run.
const PrCompressCodecSet* corpusCompress() {
    static PrCompressCodecSet* set = prCompressCodecSetCreateDefault();
    return set;
}

// The longest array that a dump writes element by element.
constexpr uint32_t kShortArray = 8;

void appendHex(PrTextBuffer* out, const void* bytes, size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    const uint8_t* b = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) {
        prTextBufferAppendChar(out, kDigits[b[i] >> 4]);
        prTextBufferAppendChar(out, kDigits[b[i] & 15]);
    }
}

void appendQuoted(PrTextBuffer* out, PrStringView text) {
    static const char kDigits[] = "0123456789abcdef";
    prTextBufferAppendChar(out, '"');
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t c = uint8_t(text.data[i]);
        if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
            prTextBufferAppend(out, "\\x");
            prTextBufferAppendChar(out, kDigits[c >> 4]);
            prTextBufferAppendChar(out, kDigits[c & 15]);
        } else {
            prTextBufferAppendChar(out, char(c));
        }
    }
    prTextBufferAppendChar(out, '"');
}

void appendInteger(PrTextBuffer* out, int64_t value) {
    if (value < 0) {
        prTextBufferAppendChar(out, '-');
        prTextBufferAppendU64(out, 0 - uint64_t(value), 10);
    } else {
        prTextBufferAppendU64(out, uint64_t(value), 10);
    }
}

// The layout of a type whose elements are numbers: the size of a scalar, and how many an element has.
struct Layout {
    uint32_t scalar;
    uint32_t count;
    bool isQuaternion;
    uint32_t rows;              // of a matrix, or zero
};

bool layoutOf(PrUsdType type, Layout* out) {
    switch (type) {
        case PrUsdType_Half:
            *out = {2, 1, false, 0};
            return true;
        case PrUsdType_Float:
            *out = {4, 1, false, 0};
            return true;
        case PrUsdType_Double:
        case PrUsdType_TimeCode:
            *out = {8, 1, false, 0};
            return true;
        case PrUsdType_Half2:
            *out = {2, 2, false, 0};
            return true;
        case PrUsdType_Half3:
            *out = {2, 3, false, 0};
            return true;
        case PrUsdType_Half4:
            *out = {2, 4, false, 0};
            return true;
        case PrUsdType_Float2:
            *out = {4, 2, false, 0};
            return true;
        case PrUsdType_Float3:
            *out = {4, 3, false, 0};
            return true;
        case PrUsdType_Float4:
            *out = {4, 4, false, 0};
            return true;
        case PrUsdType_Double2:
            *out = {8, 2, false, 0};
            return true;
        case PrUsdType_Double3:
            *out = {8, 3, false, 0};
            return true;
        case PrUsdType_Double4:
            *out = {8, 4, false, 0};
            return true;
        case PrUsdType_Quath:
            *out = {2, 4, true, 0};
            return true;
        case PrUsdType_Quatf:
            *out = {4, 4, true, 0};
            return true;
        case PrUsdType_Quatd:
            *out = {8, 4, true, 0};
            return true;
        case PrUsdType_Matrix2d:
            *out = {8, 4, false, 2};
            return true;
        case PrUsdType_Matrix3d:
            *out = {8, 9, false, 3};
            return true;
        case PrUsdType_Matrix4d:
            *out = {8, 16, false, 4};
            return true;
        default:
            return false;
    }
}

bool isStringType(PrUsdType type) {
    return type == PrUsdType_String || type == PrUsdType_Token || type == PrUsdType_Asset || type == PrUsdType_Path ||
        type == PrUsdType_PathExpression || type == PrUsdType_Unregistered;
}

bool isIntegerType(PrUsdType type) {
    return type == PrUsdType_UChar || type == PrUsdType_Int || type == PrUsdType_UInt || type == PrUsdType_Int64 ||
        type == PrUsdType_UInt64;
}

int64_t integerAt(PrUsdType type, const void* data, uint32_t index) {
    switch (type) {
        case PrUsdType_UChar:
            return static_cast<const uint8_t*>(data)[index];
        case PrUsdType_Int:
            return static_cast<const int32_t*>(data)[index];
        case PrUsdType_UInt:
            return static_cast<const uint32_t*>(data)[index];
        default:
            return static_cast<const int64_t*>(data)[index];
    }
}

// Writes the bytes of one element of a type of numbers in the file format's layout: a matrix by rows.
void elementBytes(PrUsdType type, const Layout& layout, const void* data, uint32_t index, uint8_t* out) {
    size_t size = size_t(layout.scalar) * layout.count;
    const uint8_t* element = static_cast<const uint8_t*>(data) + size * index;
    if (layout.rows == 0) {
        prMemCopy(out, element, size);
        return;
    }
    (void)type;
    const double* m = reinterpret_cast<const double*>(element);
    double rows[16];
    for (uint32_t r = 0; r < layout.rows; ++r) {
        for (uint32_t c = 0; c < layout.rows; ++c)
            rows[r * layout.rows + c] = m[c * layout.rows + r];
    }
    prMemCopy(out, rows, size);
}

void dumpValue(PrTextBuffer* out, const PrUsdValue& value, bool loose, bool inDictionary);

// Writes one element, as a scalar is written and as each element of a short array is.
void dumpElement(PrTextBuffer* out, PrUsdType type, const void* data, uint32_t index, bool loose) {
    Layout layout;
    if (type == PrUsdType_Bool) {
        prTextBufferAppendChar(out, static_cast<const bool*>(data)[index] ? '1' : '0');
    } else if (isIntegerType(type)) {
        if (type == PrUsdType_UInt64)
            prTextBufferAppendU64(out, static_cast<const uint64_t*>(data)[index], 10);
        else
            appendInteger(out, integerAt(type, data, index));
    } else if (isStringType(type)) {
        appendQuoted(out, static_cast<const PrStringView*>(data)[index]);
    } else if (layoutOf(type, &layout)) {
        uint8_t bytes[128];
        elementBytes(type, layout, data, index, bytes);
        for (uint32_t i = 0; i < layout.count; ++i) {
            // A quaternion is written with its real part first, as the text format writes it.
            uint32_t from = layout.isQuaternion ? (i + 3) % 4 : i;
            if (i != 0)
                prTextBufferAppendChar(out, ',');
            appendHex(out, bytes + from * layout.scalar, layout.scalar);
        }
    } else if (type == PrUsdType_Int2 || type == PrUsdType_Int3 || type == PrUsdType_Int4) {
        uint32_t count = type == PrUsdType_Int2 ? 2 : type == PrUsdType_Int3 ? 3 : 4;
        const int32_t* components = static_cast<const int32_t*>(data) + size_t(index) * count;
        for (uint32_t i = 0; i < count; ++i) {
            if (i != 0)
                prTextBufferAppendChar(out, ',');
            appendInteger(out, components[i]);
        }
    } else if (type == PrUsdType_LayerOffset) {
        const PrUsdLayerOffset& offset = static_cast<const PrUsdLayerOffset*>(data)[index];
        appendHex(out, &offset.offset, 8);
        prTextBufferAppendChar(out, ',');
        appendHex(out, &offset.scale, 8);
    } else if (type == PrUsdType_Reference || type == PrUsdType_Payload) {
        const PrUsdReference& reference = static_cast<const PrUsdReference*>(data)[index];
        prTextBufferAppendChar(out, '{');
        appendQuoted(out, reference.assetPath);
        prTextBufferAppendChar(out, ' ');
        appendQuoted(out, reference.primPath);
        prTextBufferAppendChar(out, ' ');
        appendHex(out, &reference.layerOffset.offset, 8);
        prTextBufferAppendChar(out, ',');
        appendHex(out, &reference.layerOffset.scale, 8);
        if (type == PrUsdType_Reference) {
            PrUsdDictionary none{};
            const PrUsdDictionary* custom = reference.customData != nullptr ? reference.customData : &none;
            prTextBufferAppendChar(out, ' ');
            dumpValue(out, PrUsdValue{PrUsdType_Dictionary, false, 1, custom}, loose, false);
        }
        prTextBufferAppendChar(out, '}');
    } else if (type == PrUsdType_Relocate) {
        const PrUsdRelocate& relocate = static_cast<const PrUsdRelocate*>(data)[index];
        appendQuoted(out, relocate.source);
        prTextBufferAppendChar(out, '>');
        appendQuoted(out, relocate.target);
    } else if (type == PrUsdType_VariantSelection) {
        const PrUsdVariantSelection& selection = static_cast<const PrUsdVariantSelection*>(data)[index];
        appendQuoted(out, selection.variantSet);
        prTextBufferAppendChar(out, ':');
        appendQuoted(out, selection.variant);
    } else {
        prTextBufferAppendChar(out, '?');
    }
}

// Writes the SHA-1 of an array's elements.
void dumpDigest(PrTextBuffer* out, const PrUsdValue& value) {
    Array<uint8_t> bytes;
    Layout layout;
    if (isStringType(value.type)) {
        const PrStringView* strings = static_cast<const PrStringView*>(value.data);
        for (uint32_t i = 0; i < value.count; ++i) {
            bytes.append(reinterpret_cast<const uint8_t*>(strings[i].data), uint32_t(strings[i].size));
            bytes.push(0);
        }
    } else if (layoutOf(value.type, &layout) && layout.rows != 0) {
        size_t size = size_t(layout.scalar) * layout.count;
        bytes.resize(uint32_t(size * value.count));
        for (uint32_t i = 0; i < value.count; ++i)
            elementBytes(value.type, layout, value.data, i, bytes.data() + size * i);
    } else {
        size_t size = size_t(prUsdTypeSize(value.type)) * value.count;
        bytes.resize(uint32_t(size));
        if (size != 0)
            prMemCopy(bytes.data(), value.data, size);
    }
    uint8_t digest[kPrSha1Bytes];
    prSha1Digest(PrByteView{bytes.data(), bytes.count()}, digest);
    prTextBufferAppend(out, "sha1 ");
    appendHex(out, digest, kPrSha1Bytes);
}

void dumpList(PrTextBuffer* out, PrStringView name, const PrUsdValue& items, bool loose) {
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppend(out, name);
    prTextBufferAppendChar(out, '(');
    prTextBufferAppendU64(out, items.count, 10);
    for (uint32_t i = 0; i < items.count; ++i) {
        prTextBufferAppendChar(out, ' ');
        dumpElement(out, items.type, items.data, i, loose);
    }
    prTextBufferAppendChar(out, ')');
}

void dumpValue(PrTextBuffer* out, const PrUsdValue& value, bool loose, bool inDictionary) {
    switch (value.type) {
        case PrUsdType_Block:
            prTextBufferAppend(out, "none");
            return;
        case PrUsdType_Dictionary: {
            const PrUsdDictionary* dictionary = static_cast<const PrUsdDictionary*>(value.data);
            prTextBufferAppend(out, "dict {");
            for (uint32_t i = 0; i < dictionary->count; ++i) {
                if (i != 0)
                    prTextBufferAppendChar(out, ',');
                appendQuoted(out, dictionary->entries[i].key);
                prTextBufferAppendChar(out, ':');
                dumpValue(out, dictionary->entries[i].value, loose, true);
            }
            prTextBufferAppendChar(out, '}');
            return;
        }
        case PrUsdType_ListOp: {
            const PrUsdListOp* listOp = static_cast<const PrUsdListOp*>(value.data);
            prTextBufferAppend(out, "listop ");
            prTextBufferAppend(out, prUsdTypeName(listOp->itemType));
            prTextBufferAppend(out, listOp->isExplicit ? " explicit=1" : " explicit=0");
            static const PrStringView kNames[] = {"explicit", "added", "prepended", "appended", "deleted", "ordered"};
            for (uint32_t i = 0; i < PrUsdListOpList_Count; ++i)
                dumpList(out, kNames[i], listOp->items[i], loose);
            return;
        }
        case PrUsdType_TimeSamples:
            prTextBufferAppend(out, "samples ");
            prTextBufferAppendU64(out, static_cast<const PrUsdTimeSamples*>(value.data)->count, 10);
            return;
        case PrUsdType_Specifier: {
            static const PrStringView kNames[] = {"def", "over", "class"};
            prTextBufferAppend(out, "specifier ");
            prTextBufferAppend(out, kNames[*static_cast<const PrUsdSpecifier*>(value.data)]);
            return;
        }
        case PrUsdType_Variability: {
            bool uniform = *static_cast<const PrUsdVariability*>(value.data) == PrUsdVariability_Uniform;
            prTextBufferAppend(out, uniform ? "variability uniform" : "variability varying");
            return;
        }
        case PrUsdType_Permission: {
            bool isPrivate = *static_cast<const PrUsdPermission*>(value.data) == PrUsdPermission_Private;
            prTextBufferAppend(out, isPrivate ? "permission private" : "permission public");
            return;
        }
        default:
            break;
    }
    if (!value.isArray && loose && inDictionary) {
        // A scalar of a dictionary, as the reference gives it to Python.
        Layout layout;
        if (isIntegerType(value.type)) {
            prTextBufferAppend(out, "int ");
            dumpElement(out, value.type, value.data, 0, loose);
            return;
        }
        if (value.type == PrUsdType_String || value.type == PrUsdType_Token) {
            prTextBufferAppend(out, "str ");
            dumpElement(out, value.type, value.data, 0, loose);
            return;
        }
        if (layoutOf(value.type, &layout) && layout.count == 1 && value.type != PrUsdType_TimeCode) {
            double wide = 0;
            if (value.type == PrUsdType_Half)
                wide = double(prFloat16ToFloat(*static_cast<const PrFloat16*>(value.data)));
            else if (value.type == PrUsdType_Float)
                wide = double(*static_cast<const float*>(value.data));
            else
                wide = *static_cast<const double*>(value.data);
            prTextBufferAppend(out, "num ");
            appendHex(out, &wide, 8);
            return;
        }
    }
    prTextBufferAppend(out, prUsdTypeName(value.type));
    if (!value.isArray) {
        prTextBufferAppendChar(out, ' ');
        dumpElement(out, value.type, value.data, 0, loose);
        return;
    }
    prTextBufferAppend(out, "[] ");
    prTextBufferAppendU64(out, value.count, 10);
    // The records that hold strings and dictionaries have no bytes to digest, and are few.
    bool isRecord = value.type == PrUsdType_LayerOffset || value.type == PrUsdType_Reference ||
        value.type == PrUsdType_Payload || value.type == PrUsdType_Relocate || value.type == PrUsdType_VariantSelection;
    if (value.count > kShortArray && !isRecord) {
        prTextBufferAppendChar(out, ' ');
        dumpDigest(out, value);
        return;
    }
    for (uint32_t i = 0; i < value.count; ++i) {
        prTextBufferAppendChar(out, ' ');
        dumpElement(out, value.type, value.data, i, loose);
    }
}

void dumpChildren(PrTextBuffer* out, const PrUsdLayer* layer, PrStringView name, PrUsdList list) {
    if (list.count == 0)
        return;
    const PrUsdSpecIndex* children = prUsdLayerList(layer, list);
    const PrUsdSpec* specs = prUsdLayerSpecs(layer);
    prTextBufferAppend(out, "C ");
    prTextBufferAppend(out, name);
    for (uint32_t i = 0; i < list.count; ++i) {
        prTextBufferAppendChar(out, ' ');
        appendQuoted(out, specs[children[i]].name);
    }
    prTextBufferAppendChar(out, '\n');
}

void dumpLayer(PrTextBuffer* out, const PrUsdLayer* layer, bool loose) {
    static const PrStringView kForms[] = {"layer", "prim", "attribute", "relationship", "variantSet", "variant"};
    const PrUsdSpec* specs = prUsdLayerSpecs(layer);
    for (uint32_t s = 0; s < prUsdLayerSpecCount(layer); ++s) {
        const PrUsdSpec& spec = specs[s];
        prTextBufferAppend(out, "S ");
        prTextBufferAppend(out, kForms[spec.form]);
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, spec.path);
        prTextBufferAppendChar(out, '\n');
        dumpChildren(out, layer, "prims", spec.prims);
        dumpChildren(out, layer, "properties", spec.properties);
        dumpChildren(out, layer, "variantSets", spec.variantSets);
        dumpChildren(out, layer, "variants", spec.variants);
        for (uint32_t f = 0; f < spec.fieldCount; ++f) {
            const PrUsdField& field = spec.fields[f];
            prTextBufferAppend(out, "F ");
            prTextBufferAppend(out, field.name);
            prTextBufferAppendChar(out, ' ');
            dumpValue(out, field.value, loose, false);
            prTextBufferAppendChar(out, '\n');
            if (field.value.type != PrUsdType_TimeSamples)
                continue;
            const PrUsdTimeSamples* samples = static_cast<const PrUsdTimeSamples*>(field.value.data);
            for (uint32_t i = 0; i < samples->count; ++i) {
                prTextBufferAppend(out, "T ");
                appendHex(out, &samples->times[i], 8);
                prTextBufferAppendChar(out, ' ');
                dumpValue(out, samples->values[i], loose, false);
                prTextBufferAppendChar(out, '\n');
            }
        }
    }
}

void writeDump(const PrUsdLayer* layer, PrStringView path, PrStringView suffix, bool loose) {
    PrTextBuffer out = {};
    dumpLayer(&out, layer, loose);
    String name(path);
    name.append(suffix);
    PrStringView text = prTextBufferView(&out);
    prFileWrite(name, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    prTextBufferReset(&out);
}

// The name that a line of JSON gives a layer's format.
PrStringView formatName(PrUsdFormat format) {
    static const PrStringView kNames[] = {"text", "crate", "materialx"};
    return kNames[format];
}

// Appends `,"formats":[...]` to a line of JSON: the names of the formats of the layers that composing `stage` read,
// each once, in the order of the layers.
void writeStageFormats(PrTextBuffer* line, const PrUsdStage* PR_NULLABLE stage) {
    Array<PrStringView> names;
    for (uint32_t i = 0; stage != nullptr && i < prUsdStageLayerCount(stage); ++i) {
        const PrUsdLayer* layer = prUsdStageLayer(stage, i);
        if (layer != nullptr && !names.contains(formatName(prUsdLayerFormat(layer))))
            names.push(formatName(prUsdLayerFormat(layer)));
    }
    prTextBufferAppend(line, ",\"formats\":[");
    for (uint32_t i = 0; i < names.count(); ++i) {
        if (i != 0)
            prTextBufferAppendChar(line, ',');
        harnessWriteString(line, names[i]);
    }
    prTextBufferAppendChar(line, ']');
}

void run(const char* location, const char* dump) {
    PrTextBuffer out = {};
    PrTextBuffer diagnostics = {};
    PrUsdOptions options = prUsdOptionsDefault();
    options.codec = corpusCompress();
    options.diagnostics = &diagnostics;
    PrUsdLayer* layer = nullptr;
    PrStatus status = prUsdLayerRead(location, nullptr, nullptr, &options, &layer);
    prTextBufferAppend(&out, "{\"status\":");
    harnessWriteString(&out, prStatusName(status));
    prTextBufferAppend(&out, ",\"diagnostics\":");
    harnessWriteString(&out, prTextBufferView(&diagnostics));
    prTextBufferAppend(&out, ",\"format\":");
    harnessWriteString(&out, layer == nullptr ? PrStringView("") : formatName(prUsdLayerFormat(layer)));
    prTextBufferAppend(&out, ",\"version\":");
    harnessWriteString(&out, layer != nullptr ? prUsdLayerVersion(layer) : PrStringView(""));
    prTextBufferAppend(&out, ",\"specs\":");
    harnessWriteNumber(&out, layer != nullptr ? prUsdLayerSpecCount(layer) : 0);
    prTextBufferAppendChar(&out, '}');
    if (layer != nullptr) {
        writeDump(layer, dump, ".strict", false);
        writeDump(layer, dump, ".loose", true);
    }
    prUsdLayerDestroy(layer);
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
    prTextBufferReset(&diagnostics);
}

void package(const char* path) {
    PrTextBuffer out = {};
    PrTextBuffer diagnostics = {};
    PrUsdOptions options = prUsdOptionsDefault();
    options.diagnostics = &diagnostics;
    PrUsdPackage* opened = nullptr;
    PrStatus status = PrStatus_Io;
    PrBlob* file = prFileOpen(path);
    if (file != nullptr) {
        status = prUsdPackageCreate(file, &options, &opened);
        prBlobRelease(file);
    }
    prTextBufferAppend(&out, "{\"status\":");
    harnessWriteString(&out, prStatusName(status));
    prTextBufferAppend(&out, ",\"diagnostics\":");
    harnessWriteString(&out, prTextBufferView(&diagnostics));
    prTextBufferAppend(&out, opened != nullptr && prUsdPackageIsConforming(opened) ? ",\"conforming\":true" :
        ",\"conforming\":false");
    prTextBufferAppend(&out, ",\"files\":[");
    for (uint32_t i = 0; opened != nullptr && i < prUsdPackageFileCount(opened); ++i) {
        if (i != 0)
            prTextBufferAppendChar(&out, ',');
        harnessWriteString(&out, prUsdPackageFile(opened, i).path);
    }
    prTextBufferAppend(&out, "]}");
    prUsdPackageDestroy(opened);
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
    prTextBufferReset(&diagnostics);
}

// The names that the dumps give the kinds of composition error, by PrUsdErrorKind.
const PrStringView kErrorKinds[PrUsdErrorKind_Count] = {
    "SublayerUnreadable",
    "SublayerCycle",
    "InvalidOffset",
    "InvalidRelocate",
    "OpinionAtRelocateSource",
    "ArcCycle",
    "ArcUnreadable",
    "ArcUnresolved",
    "ArcInvalidPath",
    "InconsistentProperty",
};

// What `--prims` writes, and the stage that it writes of.
struct PrimsDump {
    PrUsdStage* stage;
    PrUsdScratch* scratch;
    PrStringView folder;        // the root layer's folder, which a layer is named under
    PrTextBuffer out;
};

// Returns the name that a dump gives the layer at `location`: its path under the root layer's folder.
PrStringView layerName(PrStringView folder, PrStringView location) {
    if (location.size > folder.size + 1 && prMemEquals(location.data, folder.data, folder.size))
        return PrStringView{location.data + folder.size + 1, location.size - folder.size - 1};
    return prFilePathName(location);
}

PrStringView layerName(const PrimsDump& dump, uint32_t layer) {
    return layerName(dump.folder, prUsdStageLayerLocation(dump.stage, layer));
}

void appendOffset(PrTextBuffer* out, PrUsdLayerOffset offset) {
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppendF64Fixed(out, offset.offset, 2);
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppendF64Fixed(out, offset.scale, 2);
}

// Writes what the graph of a prim holds that a stage does not give, for the prim at `path` and the prims under it: the
// retiming of each node and of the layers of its stack, and the names that relocates forbid the prim's children.
void dumpGraphFacts(UsdComposer* c, const UsdGraph* graph, const String& path, PrStringView folder, PrTextBuffer* out) {
    static const char* const kArcs[] = {"root", "inherit", "variant", "relocate", "reference", "payload", "specialize"};
    RawArray<Atom> names{};
    RawArray<Atom> prohibited{};
    usdGraphChildNames(c, graph, &names, &prohibited);
    for (uint32_t i = 0; path.count() != 0 && i < graph->nodes.count; ++i) {
        const UsdNode& n = graph->nodes.data[i];
        const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
        prTextBufferAppend(out, "O ");
        prTextBufferAppend(out, path);
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, layerName(folder, c->layers.data[stack.root].location));
        prTextBufferAppendChar(out, ' ');
        usdPathText(&c->paths, n.path, out);
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, kArcs[n.arc]);
        appendOffset(out, c->maps.maps.data[n.mapToRoot].offset);
        prTextBufferAppendChar(out, '\n');
        for (uint32_t j = 0; j < stack.layerCount; ++j) {
            const UsdStackLayer& layer = c->stackLayers.data[stack.firstLayer + j];
            if (usdOffsetIsIdentity(layer.offset))
                continue;
            prTextBufferAppend(out, "o ");
            prTextBufferAppend(out, path);
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, layerName(folder, c->layers.data[layer.layer].location));
            appendOffset(out, layer.offset);
            prTextBufferAppendChar(out, '\n');
        }
    }
    if (prohibited.count != 0) {
        prTextBufferAppend(out, "X ");
        prTextBufferAppend(out, path);
        for (uint32_t i = 0; i < prohibited.count; ++i) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, atomTableText(&c->paths.names, prohibited.data[i]));
        }
        prTextBufferAppendChar(out, '\n');
    }
    for (uint32_t i = 0; i < names.count; ++i) {
        UsdGraph child = {};
        usdComposeChild(c, graph, names.data[i], &child);
        String below(path);
        below.append("/");
        below.append(atomTableText(&c->paths.names, names.data[i]));
        dumpGraphFacts(c, &child, below, folder, out);
        child.nodes.reset();
    }
    names.reset();
    prohibited.reset();
}

// Writes the facts of a prim and of the prims under it. An instance's children are its prototype's, at paths under the
// instance, which is how a composition of each prim alone has them.
void dumpPrim(PrimsDump* dump, PrUsdPrimIndex index, PrStringView path) {
    PrUsdStage* stage = dump->stage;
    PrTextBuffer* out = &dump->out;
    const PrUsdPrim& prim = prUsdStagePrims(stage)[index];
    prTextBufferAppend(out, "P ");
    prTextBufferAppend(out, path);
    prTextBufferAppendChar(out, '\n');
    Array<PrStringView> sets;
    for (uint32_t j = 0; j < prim.opinionCount; ++j) {
        PrUsdOpinion opinion = prUsdStageOpinion(stage, prim.firstOpinion + j);
        const PrUsdLayer* layer = prUsdStageLayer(stage, opinion.layer);
        prTextBufferAppend(out, "S ");
        prTextBufferAppend(out, layerName(*dump, opinion.layer));
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, prUsdLayerSpecs(layer)[opinion.spec].path);
        prTextBufferAppendChar(out, '\n');
        // The variant sets that the prim's specs hold, which are the ones that it may have selected of.
        const PrUsdSpec& spec = prUsdLayerSpecs(layer)[opinion.spec];
        const PrUsdSpecIndex* held = prUsdLayerList(layer, spec.variantSets);
        for (uint32_t k = 0; k < spec.variantSets.count; ++k) {
            PrStringView name = prUsdLayerSpecs(layer)[held[k]].name;
            if (!sets.contains(name))
                sets.push(name);
        }
    }
    sortItems(sets.span(), [](PrStringView a, PrStringView b) { return prStringCompare(a, b) < 0; });
    for (PrStringView set : sets) {
        PrStringView variant = prUsdStagePrimVariantSelection(stage, index, set);
        if (variant.size != 0) {
            prTextBufferAppend(out, "V ");
            prTextBufferAppend(out, set);
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, variant);
            prTextBufferAppendChar(out, '\n');
        }
    }
    PrUsdPrimIndex first =
        prim.prototype != kPrUsdNone ? prUsdStagePrims(stage)[prim.prototype].firstChild : prim.firstChild;
    if (first != kPrUsdNone) {
        prTextBufferAppendChar(out, 'C');
        for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = prUsdStagePrims(stage)[child].nextSibling) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, prUsdStagePrims(stage)[child].name);
        }
        prTextBufferAppendChar(out, '\n');
    }
    Array<uint32_t> properties;
    for (uint32_t j = 0; j < prim.propertyCount; ++j)
        properties.push(prim.firstProperty + j);
    sortItems(properties.span(), [&](uint32_t a, uint32_t b) {
        return prStringCompare(prUsdStageProperties(stage)[a].name, prUsdStageProperties(stage)[b].name) < 0;
    });
    if (properties.count() != 0) {
        prTextBufferAppendChar(out, 'N');
        for (uint32_t property : properties) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, prUsdStageProperties(stage)[property].name);
        }
        prTextBufferAppendChar(out, '\n');
    }
    for (uint32_t property : properties) {
        Array<PrUsdOpinion> opinions(prUsdStagePropertyOpinions(stage, index, property, nullptr, 0));
        prUsdStagePropertyOpinions(stage, index, property, opinions.data(), opinions.count());
        for (const PrUsdOpinion& opinion : opinions) {
            prTextBufferAppend(out, "Q ");
            prTextBufferAppend(out, prUsdStageProperties(stage)[property].name);
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, layerName(*dump, opinion.layer));
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, prUsdLayerSpecs(prUsdStageLayer(stage, opinion.layer))[opinion.spec].path);
            prTextBufferAppendChar(out, '\n');
        }
    }
    uint32_t invalid = 0;
    for (uint32_t property : properties) {
        PrUsdValue targets = {};
        uint32_t unmapped = 0;
        if (!prUsdStageTargets(stage, index, property, false, dump->scratch, &targets, &unmapped))
            continue;
        invalid += unmapped;
        if (targets.count == 0)
            continue;
        prTextBufferAppend(out, "G ");
        prTextBufferAppend(out, prUsdStageProperties(stage)[property].name);
        for (uint32_t j = 0; j < targets.count; ++j) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, static_cast<const PrStringView*>(targets.data)[j]);
        }
        prTextBufferAppendChar(out, '\n');
    }
    if (invalid != 0) {
        prTextBufferAppend(out, "U ");
        prTextBufferAppendU64(out, invalid, 10);
        prTextBufferAppendChar(out, '\n');
    }
    for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = prUsdStagePrims(stage)[child].nextSibling) {
        String below(path);
        below.append("/");
        below.append(prUsdStagePrims(stage)[child].name);
        dumpPrim(dump, child, below);
    }
}

// Writes the results of composing every prim of a stage: `usd_corpus --prims <location> <dump>`, for the
// specification's composition tests. The dump has a line per fact:
//
//   P <path>                      a prim, followed by its facts
//   S <layer> <spec path>         an opinion, strongest first
//   V <variant set> <variant>     a variant that composing the prim selected
//   C <name>...                   the prim's children, in order
//   N <name>...                   the prim's properties, in the order of their bytes
//   Q <property> <layer> <path>   an opinion of a property, strongest first
//   G <property> <path>...        the targets of a relationship, or the connections of an attribute, that has any
//   U <count>                     the number of the prim's targets and connections that map to nothing
//   E <kind> <path> <message>     a composition error, after the prims
//
// What a stage does not give follows, from the graph of each prim, composed again. An instance's children are composed
// at their paths under the instance:
//
//   O <prim> <layer> <path> <arc> <offset> <scale>   a node of the prim's graph, strongest first: the root layer of
//                                 its layer stack, its path, its arc, and the retiming from its times to the stage's
//   o <prim> <layer> <offset> <scale>   a layer of the node's stack whose times are not the stack's, after the node
//   X <prim> <name>...            the names that relocates forbid the prim's children
void prims(const char* location, const char* file) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    // The fallback that the expected results were made with.
    const PrUsdVariantSelection fallback = {"standin", "render"};
    options.variantFallbackCount = 1;
    options.variantFallbacks = &fallback;
    PrimsDump dump = {};
    PrStatus status = prUsdStageCreate(location, &options, &dump.stage);
    PrUsdStage* stage = dump.stage;
    dump.scratch = prUsdScratchCreate();
    dump.folder = prFilePathParent(location);
    PrTextBuffer& out = dump.out;
    PrUsdPrimIndex first = stage != nullptr ? prUsdStagePrims(stage)[kPrUsdPseudoRoot].firstChild : kPrUsdNone;
    for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = prUsdStagePrims(stage)[child].nextSibling) {
        String path("/");
        path.append(prUsdStagePrims(stage)[child].name);
        dumpPrim(&dump, child, path);
    }
    for (uint32_t i = 0; stage != nullptr && i < prUsdStageErrorCount(stage); ++i) {
        const PrUsdError& error = prUsdStageErrors(stage)[i];
        prTextBufferAppend(&out, "E ");
        prTextBufferAppend(&out, kErrorKinds[error.kind]);
        prTextBufferAppendChar(&out, ' ');
        prTextBufferAppend(&out, error.prim.size != 0 ? error.prim : PrStringView("-"));
        prTextBufferAppendChar(&out, ' ');
        prTextBufferAppend(&out, error.message);
        prTextBufferAppendChar(&out, '\n');
    }
    if (stage != nullptr) {
        UsdComposer composer = {};
        usdComposerInit(&composer, &options);
        PrStatus opened = PrStatus_Ok;
        uint32_t stack = usdComposerOpen(&composer, location, &opened);
        if (stack != kPrUsdNone) {
            UsdGraph root = {};
            usdComposeRoot(&composer, stack, &root);
            dumpGraphFacts(&composer, &root, String(), dump.folder, &out);
            root.nodes.reset();
        }
        usdComposerReset(&composer);
    }
    PrStringView text = prTextBufferView(&out);
    prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    prTextBufferReset(&out);
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\":");
    harnessWriteString(&line, prStatusName(status));
    prTextBufferAppend(&line, ",\"prims\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStagePrimCount(stage) : 0);
    prTextBufferAppendChar(&line, '}');
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prUsdScratchDestroy(dump.scratch);
    prUsdStageDestroy(stage);
}

// What `--stage` writes, and the stage that it writes of.
struct StageDump {
    PrUsdStage* stage;
    PrUsdScratch* scratch;
    PrTextBuffer out;
    Array<uint32_t> names;      // the number that the dump names each prototype by, or zero until an instance is met
    Array<uint32_t> queue;      // the prototypes that are named and not yet written
};

// The fields of a prim that the dump leaves out: those that composition consumes, and those that it writes apart.
bool isComposedField(PrStringView name) {
    static const PrStringView kNames[] = {
        "specifier", "typeName", "references", "payload", "inheritPaths",
        "specializes", "variantSelection", "variantSetNames", "primOrder", "propertyOrder", "clips", "clipSets",
    };
    for (PrStringView each : kNames) {
        if (each == name)
            return true;
    }
    return false;
}

// Writes a resolved value: a list op as the list that it makes, and any other value as a layer's dump writes it.
void dumpResolved(PrTextBuffer* out, const PrUsdValue& value) {
    if (value.type == PrUsdType_ListOp)
        dumpValue(out, static_cast<const PrUsdListOp*>(value.data)->items[PrUsdListOpList_Explicit], true, false);
    else
        dumpValue(out, value, true, false);
}

// Writes a path of the stage, with the number of a prototype left out of its name: the two readers number their
// prototypes apart.
void dumpTarget(PrTextBuffer* out, PrStringView path) {
    static const PrStringView kPrefix = "/__Prototype_";
    if (path.size > kPrefix.size && prMemEquals(path.data, kPrefix.data, kPrefix.size)) {
        size_t end = kPrefix.size;
        while (end < path.size && prAsciiIsDigit(path.data[end]))
            ++end;
        prTextBufferAppend(out, "/__P");
        path = PrStringView{path.data + end, path.size - end};
    }
    prTextBufferAppend(out, path);
}

void dumpTimeValue(StageDump* dump, PrUsdPrimIndex prim, uint32_t property, char kind, double time) {
    PrTextBuffer* out = &dump->out;
    prTextBufferAppendChar(out, kind);
    prTextBufferAppendChar(out, ' ');
    if (kind != 'D') {
        appendHex(out, &time, 8);
        prTextBufferAppendChar(out, ' ');
    }
    PrUsdValue value{};
    if (prUsdStageAttributeValue(dump->stage, prim, property, time, dump->scratch, &value))
        dumpValue(out, value, true, false);
    else
        prTextBufferAppend(out, "none");
    prTextBufferAppendChar(out, '\n');
}

// Writes a prim of the stage and the prims under it. `path` is the prim's path, under the dump's name of its prototype
// if it is in one.
void dumpStagePrim(StageDump* dump, PrUsdPrimIndex index, PrStringView path) {
    PrUsdStage* stage = dump->stage;
    PrTextBuffer* out = &dump->out;
    const PrUsdPrim& prim = prUsdStagePrims(stage)[index];
    static const PrStringView kSpecifiers[] = {"def", "over", "class"};
    prTextBufferAppend(out, "P ");
    prTextBufferAppend(out, path);
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppend(out, kSpecifiers[prim.specifier]);
    prTextBufferAppendChar(out, ' ');
    appendQuoted(out, prim.typeName);
    prTextBufferAppendChar(out, ' ');
    static const char kFlags[] = "aldbi-p";
    for (uint32_t bit = 0; bit < 7; ++bit) {
        if (kFlags[bit] != '-' && (prim.flags & (1u << bit)) != 0)
            prTextBufferAppendChar(out, kFlags[bit]);
    }
    if (prim.prototype != kPrUsdNone) {
        // A prototype is named by the order in which the dump meets its instances.
        uint32_t prototype = 0;
        while (prUsdStagePrototype(stage, prototype) != prim.prototype)
            ++prototype;
        if (dump->names[prototype] == 0) {
            dump->queue.push(prototype);
            dump->names[prototype] = dump->queue.count();
        }
        prTextBufferAppend(out, " /__P");
        prTextBufferAppendU64(out, dump->names[prototype], 10);
    }
    prTextBufferAppendChar(out, '\n');

    Array<PrStringView> fields;
    for (uint32_t i = 0; i < prim.opinionCount; ++i) {
        PrUsdOpinion opinion = prUsdStageOpinion(stage, prim.firstOpinion + i);
        const PrUsdLayer* layer = prUsdStageLayer(stage, opinion.layer);
        const PrUsdSpec& spec = prUsdLayerSpecs(layer)[opinion.spec];
        prTextBufferAppend(out, "S ");
        prTextBufferAppend(out, prFilePathName(prUsdStageLayerLocation(stage, opinion.layer)));
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, spec.path);
        prTextBufferAppendChar(out, '\n');
        for (uint32_t j = 0; j < spec.fieldCount; ++j) {
            if (!isComposedField(spec.fields[j].name) && !fields.contains(spec.fields[j].name))
                fields.push(spec.fields[j].name);
        }
    }
    sortItems(fields.span(), [](PrStringView a, PrStringView b) { return prStringCompare(a, b) < 0; });
    for (PrStringView field : fields) {
        PrUsdValue value{};
        if (!prUsdStagePrimField(stage, index, field, dump->scratch, &value))
            continue;
        prTextBufferAppend(out, "M ");
        prTextBufferAppend(out, field);
        prTextBufferAppendChar(out, ' ');
        dumpResolved(out, value);
        prTextBufferAppendChar(out, '\n');
    }
    PrUsdValue sets{};
    if (prUsdStagePrimVariantSets(stage, index, dump->scratch, &sets)) {
        for (uint32_t i = 0; i < sets.count; ++i) {
            PrStringView set = static_cast<const PrStringView*>(sets.data)[i];
            prTextBufferAppend(out, "V ");
            appendQuoted(out, set);
            prTextBufferAppendChar(out, ' ');
            appendQuoted(out, prUsdStagePrimVariantSelection(stage, index, set));
            prTextBufferAppendChar(out, '\n');
        }
    }

    for (uint32_t i = 0; i < prim.propertyCount; ++i) {
        uint32_t property = prim.firstProperty + i;
        const PrUsdProperty& p = prUsdStageProperties(stage)[property];
        bool attribute = p.form == PrUsdSpecForm_Attribute;
        PrUsdValue value{};
        prTextBufferAppend(out, attribute ? "A " : "R ");
        prTextBufferAppend(out, p.name);
        bool custom = prUsdStagePropertyField(stage, index, property, "custom", dump->scratch, &value) &&
            *static_cast<const bool*>(value.data);
        prTextBufferAppend(out, custom ? " custom" : " -");
        if (attribute) {
            prTextBufferAppendChar(out, ' ');
            if (prUsdStagePropertyField(stage, index, property, "typeName", dump->scratch, &value))
                prTextBufferAppend(out, *static_cast<const PrStringView*>(value.data));
            bool uniform = prUsdStagePropertyField(stage, index, property, "variability", dump->scratch, &value) &&
                *static_cast<const PrUsdVariability*>(value.data) == PrUsdVariability_Uniform;
            prTextBufferAppend(out, uniform ? " uniform" : " varying");
        }
        prTextBufferAppendChar(out, '\n');
        if (attribute) {
            dumpTimeValue(dump, index, property, 'D', kPrF64Nan);
            // The values at the first and the last sample, outside them, and between the first two and the last two.
            const double* times = nullptr;
            uint32_t count = prUsdStageAttributeTimes(stage, index, property, -kPrF64Infinity, kPrF64Infinity,
                dump->scratch, &times);
            if (count != 0) {
                prTextBufferAppend(out, "T ");
                prTextBufferAppendU64(out, count, 10);
                prTextBufferAppendChar(out, ' ');
                uint8_t sha[20];
                prSha1Digest(PrByteView{reinterpret_cast<const uint8_t*>(times), size_t(count) * 8}, sha);
                appendHex(out, sha, 20);
                prTextBufferAppendChar(out, '\n');
                dumpTimeValue(dump, index, property, 'X', times[0]);
                dumpTimeValue(dump, index, property, 'X', times[count - 1]);
                dumpTimeValue(dump, index, property, 'X', times[0] - 1.0);
                dumpTimeValue(dump, index, property, 'X', times[count - 1] + 1.0);
                if (count > 1) {
                    dumpTimeValue(dump, index, property, 'I', (times[0] + times[1]) / 2);
                    dumpTimeValue(dump, index, property, 'I', (times[count - 2] + times[count - 1]) / 2);
                }
            }
        }
        for (uint32_t forward = 0; forward < (attribute ? 1u : 2u); ++forward) {
            if (!prUsdStageTargets(stage, index, property, forward != 0, dump->scratch, &value, nullptr) && forward == 0)
                continue;
            prTextBufferAppendChar(out, forward != 0 ? 'F' : 'G');
            for (uint32_t j = 0; j < value.count; ++j) {
                prTextBufferAppendChar(out, ' ');
                dumpTarget(out, static_cast<const PrStringView*>(value.data)[j]);
            }
            prTextBufferAppendChar(out, '\n');
        }
        prUsdScratchClear(dump->scratch);
    }
    for (PrUsdPrimIndex child = prim.firstChild; child != kPrUsdNone; child = prUsdStagePrims(stage)[child].nextSibling) {
        String below(path);
        below.append("/");
        below.append(prUsdStagePrims(stage)[child].name);
        dumpStagePrim(dump, child, below);
    }
}

// Writes a composed stage, for comparing with the stage that OpenUSD composes of the same root layer: `usd_corpus
// --stage <location> <dump>`. tools/suite/usd_reference.py writes the same lines of OpenUSD's stage:
//
//   P <path> <specifier> <type> <flags> [<prototype>]   a prim, in depth-first order, followed by its facts. The flags
//                                 are letters: a active, l loaded, d defined, b abstract, i an instance, p a payload
//   S <layer> <spec path>         an opinion, strongest first
//   M <field> <value>             a resolved metadata field, in the order of the names
//   V <variant set> <variant>     a variant set and the variant that composing the prim selected
//   A <name> <custom> <type> <variability>   an attribute, in the prim's order of properties
//   R <name> <custom>             a relationship
//   D <value>                     the attribute's default value
//   T <count> <digest>            the times of the samples that decide the attribute's value
//   X <time> <value>              the value at a sample's time, or outside the samples
//   I <time> <value>              the value between two samples
//   G <path>...                   the targets of the relationship, or the connections of the attribute
//   F <path>...                   the targets with relationships followed
//   E <kind>                      a kind of composition error that the stage has
//
// A prototype's prims follow the others, under `/__P<n>`, numbered in the order that the dump meets their instances.
void stage(const char* location, const char* file) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    StageDump dump = {};
    PrStatus status = prUsdStageCreate(location, &options, &dump.stage);
    PrUsdStage* stage = dump.stage;
    dump.scratch = prUsdScratchCreate();
    PrTextBuffer& out = dump.out;
    if (stage != nullptr) {
        dump.names.resize(prUsdStagePrototypeCount(stage));
        const PrUsdPrim* prims = prUsdStagePrims(stage);
        for (PrUsdPrimIndex child = prims[kPrUsdPseudoRoot].firstChild; child != kPrUsdNone; child =
                prims[child].nextSibling) {
            String path("/");
            path.append(prims[child].name);
            dumpStagePrim(&dump, child, path);
        }
        for (uint32_t i = 0; i < dump.queue.count(); ++i) {
            String root("/__P");
            PrTextBuffer number = {};
            prTextBufferAppendU64(&number, i + 1, 10);
            root.append(prTextBufferView(&number));
            prTextBufferReset(&number);
            PrUsdPrimIndex first = prims[prUsdStagePrototype(stage, dump.queue[i])].firstChild;
            for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = prims[child].nextSibling) {
                String path(root);
                path.append("/");
                path.append(prims[child].name);
                dumpStagePrim(&dump, child, path);
            }
        }
        Array<PrStringView> kinds;
        for (uint32_t i = 0; i < prUsdStageErrorCount(stage); ++i) {
            PrStringView kind = kErrorKinds[prUsdStageErrors(stage)[i].kind];
            if (!kinds.contains(kind))
                kinds.push(kind);
        }
        sortItems(kinds.span(), [](PrStringView a, PrStringView b) { return prStringCompare(a, b) < 0; });
        for (PrStringView kind : kinds) {
            prTextBufferAppend(&out, "E ");
            prTextBufferAppend(&out, kind);
            prTextBufferAppendChar(&out, '\n');
        }
    }
    PrStringView text = prTextBufferView(&out);
    prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    prTextBufferReset(&out);
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\":");
    harnessWriteString(&line, prStatusName(status));
    prTextBufferAppend(&line, ",\"prims\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStagePrimCount(stage) : 0);
    prTextBufferAppend(&line, ",\"layers\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStageLayerCount(stage) : 0);
    writeStageFormats(&line, stage);
    prTextBufferAppendChar(&line, '}');
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prUsdScratchDestroy(dump.scratch);
    prUsdStageDestroy(stage);
}

// Writes what the schemas of geometry read of each prim of a stage that is not in a prototype, depth first, as
// tools/suite/usd_reference.py writes OpenUSD's reading: for a prim of a transformable type, `X <path> <resets> <m>`,
// its local transform at the stage's start time code in USD's rows, sixteen numbers; for a mesh, `M <path> <points>
// <faces> <corners> <holes> <orientation> <normals> <interpolation>`, or `M <path> invalid` where its topology does not
// hold together, and `B <path> <material>`, the path of the material bound for the preview purpose, or `-`.
void dumpGeometryPrim(const PrUsdStage* stage, PrUsdScratch* scratch, double time, PrUsdPrimIndex index,
        PrStringView path, PrTextBuffer* out) {
    const PrUsdPrim* prims = prUsdStagePrims(stage);
    PrUsdGeomKind kind = prUsdGeomPrimKind(stage, index);
    if (prUsdGeomKindIsXformable(kind)) {
        PrDMatrix4 m{};
        bool resets = false;
        prUsdGeomLocalTransform(stage, index, time, scratch, &m, &resets);
        prTextBufferAppend(out, "X ");
        prTextBufferAppend(out, path);
        prTextBufferAppend(out, resets ? " 1" : " 0");
        // lib/geom's matrix maps column vectors, and USD's rows are its columns.
        for (uint32_t row = 0; row < 4; ++row) {
            for (uint32_t column = 0; column < 4; ++column) {
                prTextBufferAppendChar(out, ' ');
                prTextBufferAppendF64Fixed(out, m.m[column][row], 9);
            }
        }
        prTextBufferAppendChar(out, '\n');
    }
    if (kind == PrUsdGeomKind_Mesh) {
        PrUsdGeomMesh mesh{};
        PrStatus status = prUsdGeomMeshRead(stage, index, time, scratch, &mesh);
        if (status != PrStatus_NotFound) {
            prTextBufferAppend(out, "M ");
            prTextBufferAppend(out, path);
        }
        if (status == PrStatus_InvalidData) {
            prTextBufferAppend(out, " invalid\n");
        } else if (status == PrStatus_Ok) {
            const uint32_t counts[] = {mesh.pointCount, mesh.faceCount, mesh.cornerCount, mesh.holeCount};
            for (uint32_t count : counts) {
                prTextBufferAppendChar(out, ' ');
                prTextBufferAppendU64(out, count, 10);
            }
            prTextBufferAppend(out, mesh.leftHanded ? " leftHanded " : " rightHanded ");
            prTextBufferAppendU64(out,
                mesh.normals.indices != nullptr ? mesh.normals.indexCount : mesh.normals.value.count, 10);
            const PrStringView interpolations[] = {"constant", "uniform", "varying", "vertex", "faceVarying"};
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, interpolations[mesh.normals.interpolation]);
            prTextBufferAppendChar(out, '\n');
        }
        PrUsdPrimIndex material = prUsdShadeBoundMaterial(stage, index, scratch);
        prTextBufferAppend(out, "B ");
        prTextBufferAppend(out, path);
        prTextBufferAppendChar(out, ' ');
        if (material == kPrUsdNone)
            prTextBufferAppendChar(out, '-');
        else
            prUsdStagePrimPath(stage, material, out);
        prTextBufferAppendChar(out, '\n');
    }
    prUsdScratchClear(scratch);
    if ((prims[index].flags & PrUsdPrimFlag_Instance) != 0)
        return;
    for (PrUsdPrimIndex child = prims[index].firstChild; child != kPrUsdNone; child = prims[child].nextSibling) {
        String childPath(path);
        childPath.append("/");
        childPath.append(prims[child].name);
        dumpGeometryPrim(stage, scratch, time, child, childPath, out);
    }
}

// Composes a stage and writes what the schemas of geometry read of it to `file` (dumpGeometryPrim), and a line of JSON:
// {"status", "formats": the formats of the layers that composing it read (writeStageFormats)}.
void geometry(const char* location, const char* file) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    PrUsdStage* stage = nullptr;
    PrStatus status = prUsdStageCreate(location, &options, &stage);
    PrTextBuffer out = {};
    if (stage != nullptr) {
        PrUsdScratch* scratch = prUsdScratchCreate();
        double time = prUsdGeomStageInfo(stage).startTimeCode;
        const PrUsdPrim* prims = prUsdStagePrims(stage);
        for (PrUsdPrimIndex child = prims[kPrUsdPseudoRoot].firstChild; child != kPrUsdNone;
                child = prims[child].nextSibling) {
            String path("/");
            path.append(prims[child].name);
            dumpGeometryPrim(stage, scratch, time, child, path, &out);
        }
        prUsdScratchDestroy(scratch);
    }
    PrStringView text = prTextBufferView(&out);
    prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    prTextBufferReset(&out);
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\":");
    harnessWriteString(&line, prStatusName(status));
    writeStageFormats(&line, stage);
    prTextBufferAppendChar(&line, '}');
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prUsdStageDestroy(stage);
}

// Appends the canonical text of a source's description (prModelSourceInfoWrite) to `out`, and the data of each of its
// meshes with every stream that its parts have: positions, normals and streams by their floats' bits, and triangles.
void writeReified(const PrModelSource* source, PrTextBuffer* out) {
    const PrModelSourceInfo* info = prModelSourceInfo(source);
    prModelSourceInfoWrite(info, out);
    auto floats = [&](const float* PR_NULLABLE values, size_t count) {
        for (size_t k = 0; values != nullptr && k < count; ++k) {
            uint32_t bits = 0;
            prMemCopy(&bits, &values[k], sizeof(bits));
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppendU64(out, bits, 16);
        }
    };
    for (uint32_t i = 0; i < info->meshCount; ++i) {
        const PrModelSourceMesh& mesh = info->meshes[i];
        Array<PrStringView> names;
        for (uint32_t p = 0; p < mesh.partCount; ++p) {
            const PrModelSourcePart& part = info->parts[mesh.firstPart + p];
            for (uint32_t k = 0; k < part.streamCount; ++k) {
                PrStringView name = info->streams[part.firstStream + k].name;
                if (!names.contains(name))
                    names.push(name);
            }
        }
        PrModelMeshRequest request{.streams = names.data(), .streamCount = names.count()};
        PrModelMeshData data{};
        PrStatus status = prModelSourceMeshRead(source, i, &request, &data);
        prTextBufferAppend(out, "\nmeshData ");
        prTextBufferAppend(out, prStatusName(status));
        floats(data.positions, size_t(data.vertexCount) * 3);
        floats(data.normals, size_t(data.vertexCount) * 3);
        for (uint32_t k = 0; data.streams != nullptr && k < data.streamCount; ++k)
            floats(data.streams[k], size_t(data.vertexCount) * data.streamWidths[k]);
        for (uint32_t k = 0; k < data.triangleCount * 3; ++k) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppendU64(out, data.triangles[k], 10);
        }
        for (uint32_t k = 0; k < data.partCount; ++k) {
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppendU64(out, data.parts[k].triangleCount, 10);
        }
        prModelMeshDataReset(&data);
    }
    prTextBufferAppendChar(out, '\n');
}

// Composes a stage and reifies it three times in its own context (pr/usd_model.h): twice from the stage, and once from
// a stage composed again of the same layers. Writes the first reification to `file` (writeReified), and a line of JSON:
// {"status", "converges": whether the three are equal, "difference": the first line where one differs from the first,
// "tracks": the number of tracks, "diagnostics": the features that the description names}.
void reified(const char* location, const char* file) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    PrUsdStage* stage = nullptr;
    PrUsdStage* again = nullptr;
    PrStatus status = prUsdStageCreate(location, &options, &stage);
    if (status == PrStatus_Ok)
        status = prUsdStageCreate(location, &options, &again);
    PrTextBuffer texts[3] = {};
    uint32_t tracks = 0;
    Array<PrStringView> features;
    PrModelSource* first = nullptr;
    if (status == PrStatus_Ok) {
        PrUsdModelOptions model = prUsdModelOptionsDefault();
        model.codec = options.layer.codec;
        const PrUsdStage* stages[3] = {stage, stage, again};
        for (uint32_t i = 0; i < 3; ++i) {
            PrModelSource* source = prUsdModelSourceCreate(stages[i], &model);
            writeReified(source, &texts[i]);
            if (i == 0)
                first = source;
            else
                prModelSourceDestroy(source);
        }
        const PrModelSourceInfo* info = prModelSourceInfo(first);
        tracks = info->trackCount;
        for (uint32_t i = 0; i < info->diagnosticCount; ++i)
            features.push(info->diagnostics[i].feature);
    }
    PrStringView text = prTextBufferView(&texts[0]);
    prFileWrite(file, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    // The first line where a later reification differs from the first.
    PrStringView difference{};
    for (uint32_t i = 1; i < 3 && difference.size == 0; ++i) {
        PrStringView other = prTextBufferView(&texts[i]);
        if (other == text)
            continue;
        size_t at = 0;
        while (at < text.size && at < other.size && text.data[at] == other.data[at])
            ++at;
        size_t begin = at;
        while (begin > 0 && text.data[begin - 1] != '\n')
            --begin;
        size_t end = begin;
        while (end < text.size && text.data[end] != '\n' && end - begin < 200)
            ++end;
        difference = PrStringView{text.data + begin, end - begin};
        if (difference.size == 0)
            difference = "the descriptions differ in length";
    }
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\":");
    harnessWriteString(&line, prStatusName(status));
    prTextBufferAppend(&line, ",\"converges\":");
    prTextBufferAppend(&line, status == PrStatus_Ok && difference.size == 0 ? "true" : "false");
    prTextBufferAppend(&line, ",\"difference\":");
    harnessWriteString(&line, difference);
    prTextBufferAppend(&line, ",\"tracks\":");
    harnessWriteNumber(&line, tracks);
    prTextBufferAppend(&line, ",\"diagnostics\":[");
    for (uint32_t i = 0; i < features.count(); ++i) {
        if (i != 0)
            prTextBufferAppendChar(&line, ',');
        harnessWriteString(&line, features[i]);
    }
    prTextBufferAppend(&line, "]}");
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    for (PrTextBuffer& each : texts)
        prTextBufferReset(&each);
    prModelSourceDestroy(first);
    prUsdStageDestroy(stage);
    prUsdStageDestroy(again);
}

// Composes a stage and visits its prims, for measuring: `usd_corpus --compose <location>` writes a line of JSON with
// the microseconds that composing took and what the stage holds.
void compose(const char* location) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    PrTimePerf start = prTimePerfNow();
    PrUsdStage* stage = nullptr;
    PrStatus status = prUsdStageCreate(location, &options, &stage);
    uint64_t opinions = 0;
    uint64_t properties = 0;
    uint32_t prims = stage != nullptr ? prUsdStagePrimCount(stage) : 0;
    for (uint32_t i = 0; i < prims; ++i) {
        opinions += prUsdStagePrims(stage)[i].opinionCount;
        properties += prUsdStagePrims(stage)[i].propertyCount;
    }
    double milliseconds = prTimePerfMilliseconds(start, prTimePerfNow());
    PrTextBuffer line = {};
    prTextBufferAppend(&line, "{\"status\":");
    harnessWriteString(&line, prStatusName(status));
    prTextBufferAppend(&line, ",\"microseconds\":");
    harnessWriteNumber(&line, uint64_t(milliseconds * 1000.0));
    prTextBufferAppend(&line, ",\"prims\":");
    harnessWriteNumber(&line, prims);
    prTextBufferAppend(&line, ",\"opinions\":");
    harnessWriteNumber(&line, opinions);
    prTextBufferAppend(&line, ",\"properties\":");
    harnessWriteNumber(&line, properties);
    prTextBufferAppend(&line, ",\"layers\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStageLayerCount(stage) : 0);
    prTextBufferAppend(&line, ",\"prototypes\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStagePrototypeCount(stage) : 0);
    prTextBufferAppend(&line, ",\"errors\":");
    harnessWriteNumber(&line, stage != nullptr ? prUsdStageErrorCount(stage) : 0);
    prTextBufferAppendChar(&line, '}');
    harnessWriteLine(prTextBufferView(&line));
    prTextBufferReset(&line);
    prUsdStageDestroy(stage);
}

// Prints the graph of the prim at `path` of the stage whose root layer is at `location`: `usd_corpus --graph <location>
// <path>`, for looking at what composition did.
void graph(const char* location, const char* path) {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = corpusCompress();
    UsdComposer composer = {};
    usdComposerInit(&composer, &options);
    PrStatus status = PrStatus_Ok;
    uint32_t stack = usdComposerOpen(&composer, location, &status);
    if (stack != kPrUsdNone) {
        Array<UsdPath> chain;
        for (UsdPath at = usdPathParse(&composer.paths, path); at > kUsdPathRoot; at =
                usdPathNode(&composer.paths, at).parent)
            chain.push(at);
        UsdGraph current = {};
        usdComposeRoot(&composer, stack, &current);
        for (uint32_t i = chain.count(); i-- > 0;) {
            UsdGraph next = {};
            usdComposeChild(&composer, &current, usdPathNode(&composer.paths, chain[i]).name, &next);
            current.nodes.reset();
            current = next;
        }
        PrTextBuffer out = {};
        usdGraphText(&composer, &current, &out);
        for (uint32_t i = 0; i < composer.errors.count; ++i) {
            prTextBufferAppend(&out, "error: ");
            prTextBufferAppend(&out, composer.errors.data[i].message);
            prTextBufferAppendChar(&out, '\n');
        }
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        current.nodes.reset();
    }
    usdComposerReset(&composer);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2 && PrStringView(argv[1]) == "--package") {
        for (int i = 2; i < argc; ++i)
            package(argv[i]);
        return 0;
    }
    if (argc == 3 && PrStringView(argv[1]) == "--compose") {
        compose(argv[2]);
        return 0;
    }
    if (argc == 4 && PrStringView(argv[1]) == "--graph") {
        graph(argv[2], argv[3]);
        return 0;
    }
    if (argc > 2 && PrStringView(argv[1]) == "--stage") {
        for (int i = 2; i + 1 < argc; i += 2)
            stage(argv[i], argv[i + 1]);
        return 0;
    }
    if (argc > 2 && PrStringView(argv[1]) == "--geometry") {
        for (int i = 2; i + 1 < argc; i += 2)
            geometry(argv[i], argv[i + 1]);
        return 0;
    }
    if (argc > 2 && PrStringView(argv[1]) == "--reified") {
        for (int i = 2; i + 1 < argc; i += 2)
            reified(argv[i], argv[i + 1]);
        return 0;
    }
    if (argc > 2 && PrStringView(argv[1]) == "--prims") {
        for (int i = 2; i + 1 < argc; i += 2)
            prims(argv[i], argv[i + 1]);
        return 0;
    }
    for (int i = 1; i + 1 < argc; i += 2)
        run(argv[i], argv[i + 1]);
    return 0;
}
