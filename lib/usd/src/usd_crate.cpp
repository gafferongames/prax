// The binary format of layers, crate, .usdc (usd-core/#binary).
//
// A crate file is a header, values, and six sections that a table of contents at the end of the file locates: the
// tokens, the strings, the fields, the field sets, the paths and the specs. A spec is a path, a form and a field set, a
// field set is a run of fields, and a field is a token and a value representation: eight bytes that hold a small value
// themselves, or say where in the file the value is. Everything a file says about where something is and how much of it
// there is comes from outside the program, so every offset and count is checked against the file before it is used.

#include "usd/src/usd_crate.h"

#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// The types of a value representation (usd-core/#type-table).
enum {
    CrateType_Bool = 1,
    CrateType_UChar,
    CrateType_Int,
    CrateType_UInt,
    CrateType_Int64,
    CrateType_UInt64,
    CrateType_Half,
    CrateType_Float,
    CrateType_Double,
    CrateType_String,
    CrateType_Token,
    CrateType_Asset,
    CrateType_Matrix2d,
    CrateType_Matrix3d,
    CrateType_Matrix4d,
    CrateType_Quatd,
    CrateType_Quatf,
    CrateType_Quath,
    CrateType_Vec2d,
    CrateType_Vec2f,
    CrateType_Vec2h,
    CrateType_Vec2i,
    CrateType_Vec3d,
    CrateType_Vec3f,
    CrateType_Vec3h,
    CrateType_Vec3i,
    CrateType_Vec4d,
    CrateType_Vec4f,
    CrateType_Vec4h,
    CrateType_Vec4i,
    CrateType_Dictionary,
    CrateType_TokenListOp,
    CrateType_StringListOp,
    CrateType_PathListOp,
    CrateType_ReferenceListOp,
    CrateType_IntListOp,
    CrateType_Int64ListOp,
    CrateType_UIntListOp,
    CrateType_UInt64ListOp,
    CrateType_PathVector,
    CrateType_TokenVector,
    CrateType_Specifier,
    CrateType_Permission,
    CrateType_Variability,
    CrateType_VariantSelectionMap,
    CrateType_TimeSamples,
    CrateType_Payload,
    CrateType_DoubleVector,
    CrateType_LayerOffsetVector,
    CrateType_StringVector,
    CrateType_ValueBlock,
    CrateType_Value,
    CrateType_UnregisteredValue,
    CrateType_UnregisteredValueListOp,
    CrateType_PayloadListOp,
    CrateType_TimeCode,
    CrateType_PathExpression,
    CrateType_Relocates,
    CrateType_Spline,
};
typedef uint32_t CrateType;

// The element type that a crate type of the foundational types reads as, by crate type from Bool through Vec4i.
constexpr PrUsdType kElementTypes[] = {
    PrUsdType_Block,    PrUsdType_Bool,     PrUsdType_UChar,    PrUsdType_Int,      PrUsdType_UInt,
    PrUsdType_Int64,    PrUsdType_UInt64,   PrUsdType_Half,     PrUsdType_Float,    PrUsdType_Double,
    PrUsdType_String,   PrUsdType_Token,    PrUsdType_Asset,    PrUsdType_Matrix2d, PrUsdType_Matrix3d,
    PrUsdType_Matrix4d, PrUsdType_Quatd,    PrUsdType_Quatf,    PrUsdType_Quath,    PrUsdType_Double2,
    PrUsdType_Float2,   PrUsdType_Half2,    PrUsdType_Int2,     PrUsdType_Double3,  PrUsdType_Float3,
    PrUsdType_Half3,    PrUsdType_Int3,     PrUsdType_Double4,  PrUsdType_Float4,   PrUsdType_Half4,
    PrUsdType_Int4,
};

// The flags of a value representation, in its highest byte.
constexpr uint64_t kRepArray = uint64_t(1) << 63;
constexpr uint64_t kRepInlined = uint64_t(1) << 62;
constexpr uint64_t kRepCompressed = uint64_t(1) << 61;
constexpr uint64_t kRepPayload = (uint64_t(1) << 48) - 1;

// The bits of a list op's header.
enum {
    ListOpBit_Explicit = 1 << 0,
    ListOpBit_ExplicitItems = 1 << 1,
    ListOpBit_AddedItems = 1 << 2,
    ListOpBit_DeletedItems = 1 << 3,
    ListOpBit_OrderedItems = 1 << 4,
    ListOpBit_PrependedItems = 1 << 5,
    ListOpBit_AppendedItems = 1 << 6,
};
typedef uint8_t ListOpBit;

// The forms of a spec as the file numbers them (usd-core/#specs-section). The forms that are not listed are those of
// legacy content, which hold nothing that a layer gives.
enum {
    CrateForm_Attribute = 1,
    CrateForm_Prim = 6,
    CrateForm_Layer = 7,
    CrateForm_Relationship = 8,
    CrateForm_Variant = 10,
    CrateForm_VariantSet = 11,
    CrateForm_Count = 12,
};
typedef uint32_t CrateForm;

// The most that an LZ4 block expands: each byte of a length's continuation stands for 255 bytes.
constexpr uint64_t kMaxExpansion = 255;

// The most bytes that one compressed array may decode to. A compressed array states its own length, which the size of
// the file does not bound as it bounds an array stored as it is.
constexpr uint64_t kMaxDecodedBytes = uint64_t(1) << 30;

// The deepest that values nest in values: dictionaries in dictionaries, and values that point to values.
constexpr uint32_t kMaxDepth = 64;

// A spec as the file lists it.
struct CrateSpec {
    uint32_t path;              // in the paths
    uint32_t fieldSet;          // the position in the field sets of its first field
    uint32_t form;
    PrUsdSpecIndex spec;        // in the layer, or kPrUsdNone until the spec is placed
};

// The state of a read.
struct Crate {
    UsdLayer* layer;
    const uint8_t* data;
    uint64_t size;
    const PrCompressCodecSet* codec;
    PrTextBuffer* PR_NULLABLE diagnostics;
    PrStatus status;
    uint8_t version[3];
    uint32_t depth;

    RawArray<PrStringView> tokens;
    RawArray<uint32_t> strings;         // by string: its token
    RawArray<uint32_t> fieldTokens;     // by field: the token of its name
    RawArray<uint64_t> fieldReps;       // by field: its value representation
    RawArray<uint32_t> fieldSets;
    RawArray<PrStringView> paths;       // by path: its text, or the empty view for no path
    RawArray<CrateSpec> specs;
    RawArray<uint32_t> specOfPath;      // by path: the spec there, or kPrUsdNone
};

bool fail(Crate& c, PrStatus status, PrStringView message) {
    if (c.status == PrStatus_Ok) {
        c.status = status;
        usdDiagnose(c.diagnostics, message);
    }
    return false;
}

bool invalid(Crate& c, PrStringView message) {
    return fail(c, PrStatus_InvalidData, message);
}

bool versionAtLeast(const Crate& c, uint8_t minor) {
    return c.version[0] > 0 || c.version[1] >= minor;
}

// Returns true if the `count` records of `size` bytes at `offset` lie in the file.
bool inside(const Crate& c, uint64_t offset, uint64_t count, uint64_t size) {
    return offset <= c.size && (size == 0 || count <= (c.size - offset) / size);
}

// A cursor over the file.
struct Reader {
    Crate& c;
    uint64_t at;

    bool read(void* out, uint64_t size) {
        if (!inside(c, at, size, 1))
            return invalid(c, "a value lies outside the file");
        prMemCopy(out, c.data + at, size_t(size));
        at += size;
        return true;
    }

    bool read8(uint8_t* out) {
        return read(out, 1);
    }
    bool read32(uint32_t* out) {
        return read(out, 4);
    }
    bool read64(uint64_t* out) {
        return read(out, 8);
    }

    // Follows an offset that is written relative to its own position.
    bool jump() {
        uint64_t start = at;
        int64_t offset = 0;
        if (!read(&offset, 8))
            return false;
        uint64_t target = start + uint64_t(offset);
        if (target > c.size)
            return invalid(c, "a value's offset lies outside the file");
        at = target;
        return true;
    }
};

// Decodes the compressed bytes at `input` into `out`, which is resized to what they decode to, at most `capacity`
// bytes. The bytes are one LZ4 block after a zero byte, or the number of chunks in one byte and then, for each chunk,
// its size in four bytes and its block.
bool decompress(Crate& c, PrByteView input, uint64_t capacity, RawArray<uint8_t>& out) {
    out.count = 0;
    if (input.size == 0)
        return invalid(c, "compressed data is empty");
    if (capacity > kArrayMaxGrowth)
        return fail(c, PrStatus_Limit, "compressed data decodes to more than the module holds");
    out.resize(uint32_t(capacity));
    uint32_t chunks = input.data[0];
    uint64_t at = 1;
    uint64_t written = 0;
    for (uint32_t i = 0; i < (chunks == 0 ? 1u : chunks); ++i) {
        uint64_t size = input.size - at;
        if (chunks != 0) {
            if (input.size - at < 4)
                return invalid(c, "a compressed chunk lies outside its data");
            int32_t stated = 0;
            prMemCopy(&stated, input.data + at, 4);
            at += 4;
            if (stated < 0 || uint64_t(stated) > input.size - at)
                return invalid(c, "a compressed chunk lies outside its data");
            size = uint64_t(stated);
        }
        PrCompressRead read{};
        PrStatus status =
            prCompressCodecSetDecode(c.codec, PrCompressFormat_Lz4, PrByteView{input.data + at, size_t(size)},
                out.data + written, size_t(capacity - written), &read);
        if (status == PrStatus_NotSupported)
            return fail(c, PrStatus_NotSupported, "the codec does not decode LZ4");
        if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done)
            return invalid(c, "compressed data does not decode");
        written += read.written;
        at += size;
    }
    out.resize(uint32_t(written));
    return true;
}

// Reads a run of compressed data that its size in eight bytes precedes, and decodes it.
bool readCompressed(Reader& r, uint64_t capacity, RawArray<uint8_t>& out) {
    uint64_t size = 0;
    if (!r.read64(&size))
        return false;
    if (!inside(r.c, r.at, size, 1))
        return invalid(r.c, "compressed data lies outside the file");
    PrByteView input{r.c.data + r.at, size_t(size)};
    r.at += size;
    return decompress(r.c, input, capacity < size * kMaxExpansion ? capacity : size * kMaxExpansion, out);
}

// Decodes `count` integers of `width` bytes, 4 or 8, from their coded form (usd-core/#compressed-integer-arrays) into
// `out`: the most common difference, two bits for each integer that say how wide its difference from the integer before
// it is stored, and the differences that are not the common one.
bool decodeIntegers(Crate& c, const RawArray<uint8_t>& coded, uint64_t count, uint32_t width, void* out) {
    uint64_t codes = (count * 2 + 7) / 8;
    if (coded.count < width || coded.count - width < codes)
        return invalid(c, "coded integers are shorter than their count");
    const uint8_t* bytes = coded.data;
    int64_t common = 0;
    if (width == 4) {
        int32_t value = 0;
        prMemCopy(&value, bytes, 4);
        common = value;
    } else {
        prMemCopy(&common, bytes, 8);
    }
    const uint8_t* code = bytes + width;
    uint64_t at = width + codes;
    uint64_t previous = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t kind = (code[i / 4] >> ((i % 4) * 2)) & 3;
        int64_t difference = common;
        if (kind != 0) {
            // The widths are a quarter, a half and the whole of the integer's.
            uint32_t size = width == 4 ? (1u << (kind - 1)) : (2u << (kind - 1));
            if (coded.count - at < size)
                return invalid(c, "coded integers are shorter than their count");
            if (size == 1) {
                difference = int8_t(bytes[at]);
            } else if (size == 2) {
                int16_t value = 0;
                prMemCopy(&value, bytes + at, 2);
                difference = value;
            } else if (size == 4) {
                int32_t value = 0;
                prMemCopy(&value, bytes + at, 4);
                difference = value;
            } else {
                prMemCopy(&difference, bytes + at, 8);
            }
            at += size;
        }
        previous += uint64_t(difference);
        if (width == 4) {
            uint32_t value = uint32_t(previous);
            prMemCopy(static_cast<uint8_t*>(out) + i * 4, &value, 4);
        } else {
            prMemCopy(static_cast<uint8_t*>(out) + i * 8, &previous, 8);
        }
    }
    return true;
}

// Reads `count` coded integers of `width` bytes at the cursor into `out`.
bool readIntegers(Reader& r, uint64_t count, uint32_t width, void* PR_NULLABLE out) {
    if (count == 0) {
        // No integers are coded as no bytes, which their size still precedes.
        uint64_t size = 0;
        if (!r.read64(&size))
            return false;
        if (!inside(r.c, r.at, size, 1))
            return invalid(r.c, "compressed data lies outside the file");
        r.at += size;
        return true;
    }
    RawArray<uint8_t> coded{};
    bool ok = readCompressed(r, width + (count * 2 + 7) / 8 + count * width, coded) &&
        decodeIntegers(r.c, coded, count, width, out);
    coded.reset();
    return ok;
}

// Reads `count` coded 32-bit integers into `out`, which is resized to hold them.
bool readIndices(Reader& r, uint64_t count, RawArray<uint32_t>& out) {
    // Each integer takes two bits at least of what the data decodes to, which the size of the file bounds.
    if (count > r.c.size * kMaxExpansion * 4)
        return invalid(r.c, "a section states more entries than the file can hold");
    if (count > kArrayMaxGrowth / 8)
        return fail(r.c, PrStatus_Limit, "a section has more entries than the module holds");
    out.resize(uint32_t(count));
    return readIntegers(r, count, 4, out.data);
}

// A section of the file.
struct Section {
    uint64_t start;
    uint64_t size;
    bool found;
};

bool readTokens(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    uint64_t size = 0;
    if (!r.read64(&count) || !r.read64(&size))
        return false;
    RawArray<uint8_t> text{};
    bool ok = readCompressed(r, size, text);
    if (ok && (text.count != size || (size != 0 && text.data[size - 1] != 0)))
        ok = invalid(c, "the tokens do not decode to their stated size, or do not end");
    uint64_t at = 0;
    for (uint64_t i = 0; ok && i < count; ++i) {
        if (at >= text.count) {
            ok = invalid(c, "the file has fewer tokens than it states");
            break;
        }
        uint64_t end = at;
        while (text.data[end] != 0)
            ++end;
        PrStringView token{reinterpret_cast<const char*>(text.data + at), size_t(end - at)};
        c.tokens.push(usdLayerIntern(c.layer, token));
        at = end + 1;
    }
    text.reset();
    return ok;
}

bool readStrings(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    if (!r.read64(&count))
        return false;
    if (!inside(c, r.at, count, 4))
        return invalid(c, "the strings lie outside the file");
    c.strings.resize(uint32_t(count));
    if (count != 0)
        prMemCopy(c.strings.data, c.data + r.at, size_t(count) * 4);
    for (uint32_t i = 0; i < c.strings.count; ++i) {
        if (c.strings.data[i] >= c.tokens.count)
            return invalid(c, "a string names a token that the file does not have");
    }
    return true;
}

bool readFields(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    if (!r.read64(&count) || !readIndices(r, count, c.fieldTokens))
        return false;
    for (uint32_t i = 0; i < c.fieldTokens.count; ++i) {
        if (c.fieldTokens.data[i] >= c.tokens.count)
            return invalid(c, "a field names a token that the file does not have");
    }
    RawArray<uint8_t> reps{};
    bool ok = readCompressed(r, count * 8, reps);
    if (ok && reps.count != count * 8)
        ok = invalid(c, "the fields' values do not decode to one for each field");
    if (ok) {
        c.fieldReps.resize(uint32_t(count));
        if (count != 0)
            prMemCopy(c.fieldReps.data, reps.data, size_t(count) * 8);
    }
    reps.reset();
    return ok;
}

bool readFieldSets(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    if (!r.read64(&count) || !readIndices(r, count, c.fieldSets))
        return false;
    for (uint32_t i = 0; i < c.fieldSets.count; ++i) {
        if (c.fieldSets.data[i] != kPrUsdNone && c.fieldSets.data[i] >= c.fieldTokens.count)
            return invalid(c, "a field set names a field that the file does not have");
    }
    if (c.fieldSets.count != 0 && c.fieldSets.data[c.fieldSets.count - 1] != kPrUsdNone)
        return invalid(c, "the last field set does not end");
    return true;
}

// A run of the paths' tree that waits to be read: the entry at which a sibling begins, and the path of its parent.
struct PendingPath {
    uint32_t entry;
    PrStringView parent;
};

// Reads the paths: a tree in preorder, in which each entry gives the path's index, the token of its last element, and a
// jump that says whether a child follows it and where its next sibling is (usd-core/#paths-section).
bool readPaths(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    uint64_t entries = 0;
    if (!r.read64(&count) || !r.read64(&entries))
        return false;
    if (count > kArrayMaxGrowth / 16)
        return fail(c, PrStatus_Limit, "the file has more paths than the module holds");
    RawArray<uint32_t> indices{};
    RawArray<uint32_t> elements{};
    RawArray<uint32_t> jumps{};
    RawArray<bool> visited{};
    RawArray<PendingPath> pending{};
    RawArray<char> text{};
    bool ok = readIndices(r, entries, indices) && readIndices(r, entries, elements) && readIndices(r, entries, jumps);
    if (ok) {
        c.paths.resize(uint32_t(count));
        for (uint32_t i = 0; i < c.paths.count; ++i)
            c.paths.data[i] = "";
        visited.resizeZeroed(uint32_t(entries));
        if (entries != 0)
            pending.push(PendingPath{0, PrStringView{}});
    }
    while (ok && pending.count != 0) {
        PendingPath run = pending.last();
        pending.pop();
        uint32_t entry = run.entry;
        PrStringView parent = run.parent;
        for (;;) {
            if (entry >= entries || visited.data[entry]) {
                ok = invalid(c, "the paths' tree leaves its entries, or reaches one twice");
                break;
            }
            visited.data[entry] = true;
            if (indices.data[entry] >= count) {
                ok = invalid(c, "a path's index is not one of the file's paths");
                break;
            }
            int32_t element = int32_t(elements.data[entry]);
            text.count = 0;
            if (parent.data == nullptr) {
                text.push('/');
            } else {
                uint32_t token = uint32_t(element < 0 ? -int64_t(element) : int64_t(element));
                if (token >= c.tokens.count) {
                    ok = invalid(c, "a path names a token that the file does not have");
                    break;
                }
                PrStringView name = c.tokens.data[token];
                text.append(parent.data, uint32_t(parent.size));
                if (element < 0)
                    text.push('.');
                else if (parent.size > 1 && parent.data[parent.size - 1] != '}' && !(name.size != 0 && name.data[0] == '{'))
                    text.push('/');
                text.append(name.data, uint32_t(name.size));
            }
            PrStringView path{text.data, text.count};
            if (!usdPathIsValid(path, UsdPathKind_Any)) {
                ok = invalid(c, "a path's elements do not make a path of the grammar");
                break;
            }
            path = usdLayerIntern(c.layer, path);
            c.paths.data[indices.data[entry]] = path;
            int32_t jump = int32_t(jumps.data[entry]);
            bool hasChild = jump > 0 || jump == -1;
            bool hasSibling = jump >= 0;
            if (hasChild && hasSibling) {
                if (uint64_t(entry) + uint64_t(jump) >= entries) {
                    ok = invalid(c, "a path's sibling lies outside the paths");
                    break;
                }
                pending.push(PendingPath{entry + uint32_t(jump), parent});
            }
            if (hasChild)
                parent = path;
            else if (!hasSibling)
                break;
            ++entry;
        }
    }
    indices.reset();
    elements.reset();
    jumps.reset();
    visited.reset();
    pending.reset();
    text.reset();
    return ok;
}

bool readSpecs(Crate& c, Section section) {
    Reader r{c, section.start};
    uint64_t count = 0;
    if (!r.read64(&count))
        return false;
    RawArray<uint32_t> paths{};
    RawArray<uint32_t> fieldSets{};
    RawArray<uint32_t> forms{};
    bool ok = readIndices(r, count, paths) && readIndices(r, count, fieldSets) && readIndices(r, count, forms);
    if (ok) {
        c.specOfPath.resize(c.paths.count);
        for (uint32_t i = 0; i < c.paths.count; ++i)
            c.specOfPath.data[i] = kPrUsdNone;
    }
    for (uint32_t i = 0; ok && i < count; ++i) {
        CrateSpec spec{paths.data[i], fieldSets.data[i], forms.data[i], kPrUsdNone};
        if (spec.path >= c.paths.count || c.paths.data[spec.path].size == 0)
            ok = invalid(c, "a spec's path is not one of the file's paths");
        else if (spec.fieldSet >= c.fieldSets.count)
            ok = invalid(c, "a spec's field set is not one of the file's field sets");
        else if (spec.form >= CrateForm_Count)
            ok = invalid(c, "a spec's form is not one of the specification's forms");
        else if (c.specOfPath.data[spec.path] != kPrUsdNone)
            ok = invalid(c, "two specs have one path");
        if (ok) {
            c.specOfPath.data[spec.path] = c.specs.count;
            c.specs.push(spec);
        }
    }
    paths.reset();
    fieldSets.reset();
    forms.reset();
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Values

bool tokenAt(Crate& c, uint64_t index, PrStringView* out) {
    if (index >= c.tokens.count)
        return invalid(c, "a value names a token that the file does not have");
    *out = c.tokens.data[index];
    return true;
}

bool stringAt(Crate& c, uint64_t index, PrStringView* out) {
    if (index >= c.strings.count)
        return invalid(c, "a value names a string that the file does not have");
    *out = c.tokens.data[c.strings.data[index]];
    return true;
}

bool pathAt(Crate& c, uint64_t index, PrStringView* out) {
    if (index >= c.paths.count)
        return invalid(c, "a value names a path that the file does not have");
    *out = c.paths.data[index];
    return true;
}

// Writes the transpose of a square matrix of `n` rows.
void transpose(const double* rows, uint32_t n, double* out) {
    for (uint32_t r = 0; r < n; ++r) {
        for (uint32_t col = 0; col < n; ++col)
            out[r * n + col] = rows[col * n + r];
    }
}

uint32_t matrixRows(PrUsdType type) {
    return type == PrUsdType_Matrix2d ? 2 : type == PrUsdType_Matrix3d ? 3 : type == PrUsdType_Matrix4d ? 4 : 0;
}

// The number of scalars in an element of a dimensioned type, and the size of each.
uint32_t componentCount(PrUsdType type) {
    switch (type) {
        case PrUsdType_Int2:
        case PrUsdType_Half2:
        case PrUsdType_Float2:
        case PrUsdType_Double2:
            return 2;
        case PrUsdType_Int3:
        case PrUsdType_Half3:
        case PrUsdType_Float3:
        case PrUsdType_Double3:
            return 3;
        case PrUsdType_Int4:
        case PrUsdType_Half4:
        case PrUsdType_Float4:
        case PrUsdType_Double4:
            return 4;
        default:
            return 1;
    }
}

bool unpack(Crate& c, uint64_t rep, PrUsdValue* out);

// Reads a value that is written as an offset to its representation, and leaves the cursor after the representation.
bool readNestedValue(Reader& r, PrUsdValue* out) {
    uint64_t rep = 0;
    return r.jump() && r.read64(&rep) && unpack(r.c, rep, out);
}

bool readDictionary(Reader& r, PrUsdValue* out) {
    Crate& c = r.c;
    uint64_t count = 0;
    if (!r.read64(&count))
        return false;
    // An entry takes a key, an offset and a representation at least.
    if (!inside(c, r.at, count, 20))
        return invalid(c, "a dictionary states more entries than the file can hold");
    RawArray<PrUsdDictionaryEntry> entries{};
    bool ok = true;
    for (uint64_t i = 0; ok && i < count; ++i) {
        PrUsdDictionaryEntry entry{};
        uint32_t key = 0;
        ok = r.read32(&key) && stringAt(c, key, &entry.key) && readNestedValue(r, &entry.value);
        if (ok)
            entries.push(entry);
    }
    if (ok)
        *out = usdLayerDictionary(c.layer, entries);
    entries.reset();
    return ok;
}

bool readLayerOffset(Reader& r, PrUsdLayerOffset* out) {
    return r.read(&out->offset, 8) && r.read(&out->scale, 8);
}

// Reads a reference, or a payload, whose retiming a file has from version 0.8.0.
bool readArc(Reader& r, bool isReference, PrUsdReference* out) {
    Crate& c = r.c;
    uint32_t asset = 0;
    uint32_t path = 0;
    *out = PrUsdReference{};
    out->layerOffset = PrUsdLayerOffset{0, 1};
    if (!r.read32(&asset) || !stringAt(c, asset, &out->assetPath) || !r.read32(&path) ||
            !pathAt(c, path, &out->primPath))
        return false;
    if (out->primPath.size != 0 && !usdPathIsValid(out->primPath, UsdPathKind_Prim))
        return invalid(c, "a reference or a payload names a path that is not an absolute path to a prim");
    if ((isReference || versionAtLeast(c, 8)) && !readLayerOffset(r, &out->layerOffset))
        return false;
    if (isReference) {
        PrUsdValue customData{};
        if (!readDictionary(r, &customData))
            return false;
        const PrUsdDictionary* dictionary = static_cast<const PrUsdDictionary*>(customData.data);
        out->customData = dictionary->count != 0 ? dictionary : nullptr;
    }
    return true;
}

// Reads a list of a count and that many items of `itemType`, as a list op, a vector and a map hold them.
bool readItems(Reader& r, PrUsdType itemType, PrUsdValue* out) {
    Crate& c = r.c;
    uint64_t count = 0;
    if (!r.read64(&count))
        return false;
    uint32_t stored = 4;
    if (itemType == PrUsdType_Int64 || itemType == PrUsdType_UInt64 || itemType == PrUsdType_Double)
        stored = 8;
    else if (itemType == PrUsdType_LayerOffset || itemType == PrUsdType_Reference || itemType == PrUsdType_Payload)
        stored = 16;
    else if (itemType == PrUsdType_Relocate || itemType == PrUsdType_VariantSelection ||
            itemType == PrUsdType_Unregistered)
        stored = 8;
    if (!inside(c, r.at, count, stored) || count > kArrayMaxGrowth / 64)
        return invalid(c, "a list states more items than the file can hold");
    void* items = usdLayerAllocElements(c.layer, itemType, uint32_t(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t first = 0;
        uint32_t second = 0;
        switch (itemType) {
            case PrUsdType_Token:
                if (!r.read32(&first) || !tokenAt(c, first, static_cast<PrStringView*>(items) + i))
                    return false;
                break;
            case PrUsdType_String:
                if (!r.read32(&first) || !stringAt(c, first, static_cast<PrStringView*>(items) + i))
                    return false;
                break;
            case PrUsdType_Path:
                if (!r.read32(&first) || !pathAt(c, first, static_cast<PrStringView*>(items) + i))
                    return false;
                break;
            case PrUsdType_Reference:
            case PrUsdType_Payload:
                if (!readArc(r, itemType == PrUsdType_Reference, static_cast<PrUsdReference*>(items) + i))
                    return false;
                break;
            case PrUsdType_LayerOffset:
                if (!readLayerOffset(r, static_cast<PrUsdLayerOffset*>(items) + i))
                    return false;
                break;
            case PrUsdType_Relocate: {
                PrUsdRelocate* relocate = static_cast<PrUsdRelocate*>(items) + i;
                if (!r.read32(&first) || !pathAt(c, first, &relocate->source) || !r.read32(&second) ||
                        !pathAt(c, second, &relocate->target))
                    return false;
                break;
            }
            case PrUsdType_VariantSelection: {
                PrUsdVariantSelection* selection = static_cast<PrUsdVariantSelection*>(items) + i;
                if (!r.read32(&first) || !stringAt(c, first, &selection->variantSet) || !r.read32(&second) ||
                        !stringAt(c, second, &selection->variant))
                    return false;
                break;
            }
            case PrUsdType_Unregistered: {
                // An item of a list whose type its writer did not know: the item's text, as a string.
                PrUsdValue inner{};
                if (!readNestedValue(r, &inner))
                    return false;
                if (inner.type != PrUsdType_String || inner.isArray)
                    return fail(c, PrStatus_NotSupported, "a list op has an unregistered item that is not text");
                static_cast<PrStringView*>(items)[i] = *static_cast<const PrStringView*>(inner.data);
                break;
            }
            default:
                if (!r.read(static_cast<uint8_t*>(items) + i * stored, stored))
                    return false;
                break;
        }
    }
    *out = PrUsdValue{itemType, true, uint32_t(count), items};
    return true;
}

bool readListOp(Reader& r, PrUsdType itemType, PrUsdValue* out) {
    Crate& c = r.c;
    uint8_t header = 0;
    if (!r.read8(&header))
        return false;
    PrUsdListOp* listOp = c.layer->arena.allocZeroed<PrUsdListOp>();
    listOp->itemType = itemType;
    listOp->isExplicit = (header & ListOpBit_Explicit) != 0;
    // The lists in the order that the file writes them.
    static const uint8_t kBits[] = {
        ListOpBit_ExplicitItems, ListOpBit_AddedItems, ListOpBit_PrependedItems,
        ListOpBit_AppendedItems, ListOpBit_DeletedItems, ListOpBit_OrderedItems,
    };
    static const PrUsdListOpList kLists[] = {
        PrUsdListOpList_Explicit, PrUsdListOpList_Added,
        PrUsdListOpList_Prepended, PrUsdListOpList_Appended, PrUsdListOpList_Deleted, PrUsdListOpList_Ordered,
    };
    for (PrUsdValue& each : listOp->items)
        each = PrUsdValue{itemType, true, 0, nullptr};
    for (uint32_t i = 0; i < 6; ++i) {
        if ((header & kBits[i]) == 0)
            continue;
        PrUsdValue items{};
        if (!readItems(r, itemType, &items))
            return false;
        uint32_t count = usdListRemoveRepeats(itemType, const_cast<void*>(items.data), items.count);
        if (count != items.count)
            return invalid(c, "a list of a list op names an item twice");
        listOp->items[kLists[i]] = items;
    }
    *out = PrUsdValue{PrUsdType_ListOp, false, 1, listOp};
    return true;
}

bool readTimeSamples(Reader& r, PrUsdValue* out) {
    Crate& c = r.c;
    PrUsdValue times{};
    uint64_t count = 0;
    if (!readNestedValue(r, &times))
        return false;
    if (!times.isArray || times.type != PrUsdType_Double)
        return invalid(c, "the times of time samples are not an array of doubles");
    if (!r.jump() || !r.read64(&count))
        return false;
    if (count != times.count || !inside(c, r.at, count, 8))
        return invalid(c, "time samples do not have one value for each time");
    RawArray<double> sampleTimes{};
    RawArray<PrUsdValue> values{};
    bool ok = true;
    for (uint64_t i = 0; ok && i < count; ++i) {
        uint64_t rep = 0;
        PrUsdValue value{};
        double time = static_cast<const double*>(times.data)[i];
        ok = r.read64(&rep) && unpack(c, rep, &value);
        if (ok && time != time)
            ok = invalid(c, "a sample's time is not a number");
        if (ok && (value.type >= PrUsdType_Dictionary && value.type != PrUsdType_Unregistered))
            ok = invalid(c, "a sample's value is not a value that an attribute holds");
        if (ok) {
            sampleTimes.push(time);
            values.push(value);
        }
    }
    if (ok)
        *out = usdLayerTimeSamples(c.layer, sampleTimes, values);
    sampleTimes.reset();
    values.reset();
    return ok;
}

// Reads an array of a foundational type: its count, and its elements as they are or coded.
bool readArray(Crate& c, PrUsdType type, uint64_t offset, bool compressed, PrUsdValue* out) {
    Reader r{c, offset};
    uint64_t count = 0;
    if (!versionAtLeast(c, 5)) {
        // The number of dimensions, which was always one.
        uint32_t rank = 0;
        if (!r.read32(&rank))
            return false;
    }
    if (versionAtLeast(c, 7)) {
        if (!r.read64(&count))
            return false;
    } else {
        uint32_t narrow = 0;
        if (!r.read32(&narrow))
            return false;
        count = narrow;
    }
    uint32_t size = usdTypeSize(type);
    bool isString = type == PrUsdType_String || type == PrUsdType_Token || type == PrUsdType_Asset ||
        type == PrUsdType_PathExpression;
    if (count > kArrayMaxGrowth / 2)
        return fail(c, PrStatus_Limit, "an array has more elements than the module holds");
    if (compressed) {
        bool isInteger = type == PrUsdType_Int || type == PrUsdType_UInt || type == PrUsdType_Int64 ||
            type == PrUsdType_UInt64;
        bool isFloat = type == PrUsdType_Half || type == PrUsdType_Float || type == PrUsdType_Double;
        if (!isInteger && !isFloat)
            return invalid(c, "an array of a type that the format does not compress is compressed");
        if (count * size > kMaxDecodedBytes)
            return fail(c, PrStatus_Limit, "a compressed array decodes to more than the module allows");
        void* elements = usdLayerAllocElements(c.layer, type, uint32_t(count));
        if (isInteger) {
            if (!readIntegers(r, count, size, elements))
                return false;
        } else {
            uint8_t code = 0;
            if (!r.read8(&code))
                return false;
            RawArray<uint32_t> integers{};
            integers.resize(uint32_t(count));
            bool ok = true;
            if (code == 'i') {
                // Every element is a whole number, coded as an integer.
                ok = readIntegers(r, count, 4, integers.data);
                for (uint64_t i = 0; ok && i < count; ++i) {
                    int32_t value = int32_t(integers.data[i]);
                    if (type == PrUsdType_Half)
                        static_cast<PrFloat16*>(elements)[i] = prFloat16FromFloat(float(value));
                    else if (type == PrUsdType_Float)
                        static_cast<float*>(elements)[i] = float(value);
                    else
                        static_cast<double*>(elements)[i] = double(value);
                }
            } else if (code == 't') {
                // The elements are positions in a table of the values that the array holds.
                uint32_t entries = 0;
                ok = r.read32(&entries);
                if (ok && !inside(c, r.at, entries, size))
                    ok = invalid(c, "a compressed array's table lies outside the file");
                const uint8_t* table = c.data + r.at;
                r.at += uint64_t(entries) * size;
                ok = ok && readIntegers(r, count, 4, integers.data);
                for (uint64_t i = 0; ok && i < count; ++i) {
                    if (integers.data[i] >= entries) {
                        ok = invalid(c, "a compressed array names an entry outside its table");
                        break;
                    }
                    prMemCopy(static_cast<uint8_t*>(elements) + i * size, table + uint64_t(integers.data[i]) * size, size);
                }
            } else {
                ok = invalid(c, "a compressed array of floats has a coding that the format does not define");
            }
            integers.reset();
            if (!ok)
                return false;
        }
        *out = PrUsdValue{type, true, uint32_t(count), elements};
        return true;
    }
    if (isString) {
        if (!inside(c, r.at, count, 4))
            return invalid(c, "an array lies outside the file");
        PrStringView* strings = static_cast<PrStringView*>(usdLayerAllocElements(c.layer, type, uint32_t(count)));
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t index = 0;
            r.read32(&index);
            // An array's asset paths are strings, though an asset path alone is a token.
            bool found = type == PrUsdType_Token ? tokenAt(c, index, strings + i) : stringAt(c, index, strings + i);
            if (!found)
                return false;
        }
        *out = PrUsdValue{type, true, uint32_t(count), strings};
        return true;
    }
    if (!inside(c, r.at, count, size))
        return invalid(c, "an array lies outside the file");
    const uint8_t* stored = c.data + r.at;
    uint32_t rows = matrixRows(type);
    if (rows != 0 || type == PrUsdType_Bool) {
        // A matrix is transposed, and a boolean is made one of the two values that the type has.
        uint8_t* elements = static_cast<uint8_t*>(usdLayerAllocElements(c.layer, type, uint32_t(count)));
        for (uint64_t i = 0; i < count; ++i) {
            if (rows == 0) {
                static_cast<bool*>(static_cast<void*>(elements))[i] = stored[i] != 0;
                continue;
            }
            double matrix[16];
            prMemCopy(matrix, stored + i * size, size);
            transpose(matrix, rows, reinterpret_cast<double*>(elements + i * size));
        }
        *out = PrUsdValue{type, true, uint32_t(count), elements};
        return true;
    }
    // The elements are read in place if the file aligns them, as files do, and are copied if it does not.
    uint32_t alignment = 4;
    switch (type) {
        case PrUsdType_UChar:
            alignment = 1;
            break;
        case PrUsdType_Half:
        case PrUsdType_Half2:
        case PrUsdType_Half3:
        case PrUsdType_Half4:
        case PrUsdType_Quath:
            alignment = 2;
            break;
        case PrUsdType_Int64:
        case PrUsdType_UInt64:
        case PrUsdType_Double:
        case PrUsdType_TimeCode:
        case PrUsdType_Double2:
        case PrUsdType_Double3:
        case PrUsdType_Double4:
        case PrUsdType_Quatd:
            alignment = 8;
            break;
        default:
            break;
    }
    if ((reinterpret_cast<uintptr_t>(stored) & (alignment - 1)) == 0)
        *out = PrUsdValue{type, true, uint32_t(count), count != 0 ? stored : nullptr};
    else
        *out = usdLayerValue(c.layer, type, true, uint32_t(count), stored);
    return true;
}

// Reads a value of a foundational type that is not an array.
bool readScalar(Crate& c, PrUsdType type, bool inlined, uint64_t payload, PrUsdValue* out) {
    alignas(16) uint8_t element[sizeof(PrDMatrix4)] = {};
    uint32_t size = usdTypeSize(type);
    uint32_t bits = uint32_t(payload);
    uint32_t rows = matrixRows(type);
    uint32_t components = componentCount(type);
    PrStringView text;
    switch (type) {
        case PrUsdType_String:
        case PrUsdType_Token:
        case PrUsdType_Asset:
            if (!inlined)
                return invalid(c, "a string, a token or an asset path is not stored in its representation");
            if (!(type == PrUsdType_String ? stringAt(c, payload, &text) : tokenAt(c, payload, &text)))
                return false;
            *out = usdLayerValue(c.layer, type, false, 1, &text);
            return true;
        case PrUsdType_PathExpression: {
            uint32_t index = bits;
            if (!inlined) {
                Reader r{c, payload};
                if (!r.read32(&index))
                    return false;
            }
            if (!stringAt(c, index, &text))
                return false;
            *out = usdLayerValue(c.layer, type, false, 1, &text);
            return true;
        }
        default:
            break;
    }
    if (!inlined) {
        Reader r{c, payload};
        if (rows != 0) {
            double matrix[16];
            if (!r.read(matrix, size))
                return false;
            transpose(matrix, rows, reinterpret_cast<double*>(element));
        } else if (!r.read(element, size)) {
            return false;
        }
        if (type == PrUsdType_Bool)
            element[0] = element[0] != 0;
        *out = usdLayerValue(c.layer, type, false, 1, element);
        return true;
    }
    if (rows != 0) {
        // The diagonal, each element in a signed byte.
        double* matrix = reinterpret_cast<double*>(element);
        for (uint32_t i = 0; i < rows; ++i)
            matrix[i * rows + i] = double(int8_t(bits >> (i * 8)));
    } else if (components != 1 && size > 4) {
        // Each component in a signed byte.
        for (uint32_t i = 0; i < components; ++i) {
            int8_t value = int8_t(bits >> (i * 8));
            switch (type) {
                case PrUsdType_Int2:
                case PrUsdType_Int3:
                case PrUsdType_Int4:
                    reinterpret_cast<int32_t*>(element)[i] = value;
                    break;
                case PrUsdType_Half3:
                case PrUsdType_Half4:
                    reinterpret_cast<PrFloat16*>(element)[i] = prFloat16FromFloat(float(value));
                    break;
                case PrUsdType_Float2:
                case PrUsdType_Float3:
                case PrUsdType_Float4:
                    reinterpret_cast<float*>(element)[i] = float(value);
                    break;
                default:
                    reinterpret_cast<double*>(element)[i] = double(value);
                    break;
            }
        }
    } else {
        switch (type) {
            case PrUsdType_Bool:
                element[0] = bits != 0;
                break;
            case PrUsdType_Int64:
                *reinterpret_cast<int64_t*>(element) = int32_t(bits);
                break;
            case PrUsdType_UInt64:
                *reinterpret_cast<uint64_t*>(element) = bits;
                break;
            case PrUsdType_Double:
            case PrUsdType_TimeCode: {
                // A double that a float holds exactly is stored as the float.
                float narrow = 0;
                prMemCopy(&narrow, &bits, 4);
                *reinterpret_cast<double*>(element) = double(narrow);
                break;
            }
            case PrUsdType_Quath:
            case PrUsdType_Quatf:
            case PrUsdType_Quatd:
                return invalid(c, "a quaternion is stored in its representation, which has no room for one");
            default:
                // The value's own bytes: an integer or a float of four bytes or fewer, or two halves.
                prMemCopy(element, &bits, size < 4 ? size : 4);
                break;
        }
    }
    *out = usdLayerValue(c.layer, type, false, 1, element);
    return true;
}

// Wraps one reference of a file older than 0.8.0, whose `payload` is a payload and not a list op, as the explicit list
// op that it stands for.
PrUsdValue explicitListOp(Crate& c, PrUsdType itemType, const void* item) {
    PrUsdListOp* listOp = c.layer->arena.allocZeroed<PrUsdListOp>();
    listOp->itemType = itemType;
    listOp->isExplicit = true;
    for (PrUsdValue& each : listOp->items)
        each = PrUsdValue{itemType, true, 0, nullptr};
    listOp->items[PrUsdListOpList_Explicit] = usdLayerValue(c.layer, itemType, true, 1, item);
    return PrUsdValue{PrUsdType_ListOp, false, 1, listOp};
}

// Reads the value that a representation holds or locates.
bool unpack(Crate& c, uint64_t rep, PrUsdValue* out) {
    if (++c.depth > kMaxDepth) {
        --c.depth;
        return invalid(c, "values nest too deeply, or a value holds itself");
    }
    uint32_t type = uint32_t((rep >> 48) & 0xff);
    bool isArray = (rep & kRepArray) != 0;
    bool inlined = (rep & kRepInlined) != 0;
    bool compressed = (rep & kRepCompressed) != 0;
    uint64_t payload = rep & kRepPayload;
    Reader r{c, payload};
    bool ok = true;
    PrUsdType element = PrUsdType_Block;
    if (type >= CrateType_Bool && type <= CrateType_Vec4i)
        element = kElementTypes[type];
    else if (type == CrateType_TimeCode)
        element = PrUsdType_TimeCode;
    else if (type == CrateType_PathExpression)
        element = PrUsdType_PathExpression;
    if (element != PrUsdType_Block) {
        if (isArray) {
            if (inlined)
                ok = invalid(c, "an array is stored in its representation");
            else if (payload == 0)
                *out = PrUsdValue{element, true, 0, nullptr};
            else
                ok = readArray(c, element, payload, compressed, out);
        } else {
            ok = readScalar(c, element, inlined, payload, out);
        }
        --c.depth;
        return ok;
    }
    if (isArray || compressed) {
        --c.depth;
        return invalid(c, "a value of a type that has no arrays is an array");
    }
    uint32_t bits = uint32_t(payload);
    uint8_t small = uint8_t(bits);
    switch (type) {
        case CrateType_Dictionary:
            if (inlined) {
                RawArray<PrUsdDictionaryEntry> none{};
                *out = usdLayerDictionary(c.layer, none);
            } else {
                ok = readDictionary(r, out);
            }
            break;
        case CrateType_TokenListOp:
            ok = readListOp(r, PrUsdType_Token, out);
            break;
        case CrateType_StringListOp:
            ok = readListOp(r, PrUsdType_String, out);
            break;
        case CrateType_PathListOp:
            ok = readListOp(r, PrUsdType_Path, out);
            break;
        case CrateType_ReferenceListOp:
            ok = readListOp(r, PrUsdType_Reference, out);
            break;
        case CrateType_PayloadListOp:
            ok = readListOp(r, PrUsdType_Payload, out);
            break;
        case CrateType_IntListOp:
            ok = readListOp(r, PrUsdType_Int, out);
            break;
        case CrateType_Int64ListOp:
            ok = readListOp(r, PrUsdType_Int64, out);
            break;
        case CrateType_UIntListOp:
            ok = readListOp(r, PrUsdType_UInt, out);
            break;
        case CrateType_UInt64ListOp:
            ok = readListOp(r, PrUsdType_UInt64, out);
            break;
        case CrateType_PathVector:
            ok = readItems(r, PrUsdType_Path, out);
            break;
        case CrateType_TokenVector:
            ok = readItems(r, PrUsdType_Token, out);
            break;
        case CrateType_StringVector:
            ok = readItems(r, PrUsdType_String, out);
            break;
        case CrateType_DoubleVector:
            ok = readItems(r, PrUsdType_Double, out);
            break;
        case CrateType_LayerOffsetVector:
            ok = readItems(r, PrUsdType_LayerOffset, out);
            break;
        case CrateType_Relocates:
            ok = readItems(r, PrUsdType_Relocate, out);
            break;
        case CrateType_VariantSelectionMap: {
            PrUsdValue pairs{};
            ok = readItems(r, PrUsdType_VariantSelection, &pairs);
            if (ok) {
                RawArray<PrUsdVariantSelection> selections{};
                selections.append(static_cast<const PrUsdVariantSelection*>(pairs.data), pairs.count);
                *out = usdLayerVariantSelections(c.layer, selections);
                selections.reset();
            }
            break;
        }
        case CrateType_Specifier:
            if (!inlined || bits > PrUsdSpecifier_Class)
                ok = invalid(c, "a specifier is not one of the specification's");
            else
                *out = usdLayerValue(c.layer, PrUsdType_Specifier, false, 1, &small);
            break;
        case CrateType_Permission:
            if (!inlined || bits > PrUsdPermission_Private)
                ok = invalid(c, "a permission is not one of the specification's");
            else
                *out = usdLayerValue(c.layer, PrUsdType_Permission, false, 1, &small);
            break;
        case CrateType_Variability:
            // Legacy content has a third variability, which reads as uniform.
            if (!inlined || bits > 2) {
                ok = invalid(c, "a variability is not one of the specification's");
            } else {
                small = bits == 0 ? PrUsdVariability_Varying : PrUsdVariability_Uniform;
                *out = usdLayerValue(c.layer, PrUsdType_Variability, false, 1, &small);
            }
            break;
        case CrateType_TimeSamples:
            ok = readTimeSamples(r, out);
            break;
        case CrateType_Payload: {
            PrUsdReference payloadItem{};
            ok = readArc(r, false, &payloadItem);
            if (ok)
                *out = explicitListOp(c, PrUsdType_Payload, &payloadItem);
            break;
        }
        case CrateType_ValueBlock:
            *out = PrUsdValue{PrUsdType_Block, false, 0, nullptr};
            break;
        case CrateType_Value:
            ok = readNestedValue(r, out);
            break;
        case CrateType_UnregisteredValue: {
            // The value of a field that its writer knew no type for: a string, which is the value's text, or a
            // dictionary.
            PrUsdValue inner{};
            ok = readNestedValue(r, &inner);
            if (ok && inner.type == PrUsdType_String && !inner.isArray)
                *out = PrUsdValue{PrUsdType_Unregistered, false, 1, inner.data};
            else if (ok && inner.type == PrUsdType_Dictionary)
                *out = inner;
            else if (ok)
                ok = invalid(c, "an unregistered value is neither a string nor a dictionary");
            break;
        }
        case CrateType_UnregisteredValueListOp:
            ok = readListOp(r, PrUsdType_Unregistered, out);
            break;
        case CrateType_Spline:
            ok = fail(c, PrStatus_NotSupported, "the file has a spline, which the module does not read");
            break;
        default:
            ok = invalid(c, "a value has a type that the format does not define");
            break;
    }
    --c.depth;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Specs

// A spec that waits for its children to be placed, and the kinds of children it has yet to place.
struct PendingSpec {
    uint32_t spec;              // in the file's specs
    uint32_t list;              // which of the lists of children comes next
    uint32_t next;              // the next child of that list
    bool hasNames;              // true once `names` holds the names of that list
    PrUsdValue names;
};

// The fields that list a spec's children, and the form that each list holds.
struct ChildList {
    PrStringView field;
    PrUsdSpecForm form;
};

constexpr ChildList kChildLists[] = {
    {"properties", PrUsdSpecForm_Attribute},
    {"variantSetChildren", PrUsdSpecForm_VariantSet},
    {"variantChildren", PrUsdSpecForm_Variant},
    {"primChildren", PrUsdSpecForm_Prim},
};

// Returns the value representation of the field named `name` in the field set at `fieldSet`, or false.
bool findRep(const Crate& c, uint32_t fieldSet, PrStringView name, uint64_t* rep) {
    for (uint32_t i = fieldSet; c.fieldSets.data[i] != kPrUsdNone; ++i) {
        uint32_t field = c.fieldSets.data[i];
        if (c.tokens.data[c.fieldTokens.data[field]] == name) {
            *rep = c.fieldReps.data[field];
            return true;
        }
    }
    return false;
}

PrUsdSpecForm formOf(uint32_t crateForm) {
    switch (crateForm) {
        case CrateForm_Attribute:
            return PrUsdSpecForm_Attribute;
        case CrateForm_Relationship:
            return PrUsdSpecForm_Relationship;
        case CrateForm_Variant:
            return PrUsdSpecForm_Variant;
        case CrateForm_VariantSet:
            return PrUsdSpecForm_VariantSet;
        case CrateForm_Layer:
            return PrUsdSpecForm_Layer;
        default:
            return PrUsdSpecForm_Prim;
    }
}

bool isNormative(uint32_t crateForm) {
    return crateForm == CrateForm_Attribute || crateForm == CrateForm_Prim || crateForm == CrateForm_Layer ||
        crateForm == CrateForm_Relationship || crateForm == CrateForm_Variant || crateForm == CrateForm_VariantSet;
}

// Sets the fields of a placed spec from its field set.
bool readSpecFields(Crate& c, const CrateSpec& spec) {
    for (uint32_t i = spec.fieldSet; c.fieldSets.data[i] != kPrUsdNone; ++i) {
        uint32_t field = c.fieldSets.data[i];
        PrStringView name = c.tokens.data[c.fieldTokens.data[field]];
        // The lists of children are the layer's own lists, and the children of legacy content are not specs of the
        // specification.
        if (usdFieldIsChildren(name) || name == "targetChildren" || name == "connectionChildren")
            continue;
        if (name.size == 0)
            return invalid(c, "a field has no name");
        PrUsdValue value{};
        if (!unpack(c, c.fieldReps.data[field], &value))
            return false;
        // The sublayers are asset paths (usd-core/#sublayers-asset), which files write as strings.
        if (name == "subLayers" && value.isArray && value.type == PrUsdType_String)
            value.type = PrUsdType_Asset;
        usdLayerSetField(c.layer, spec.spec, name, value);
    }
    return true;
}

// Places the specs in the layer, parents first and children in the order that their parent lists them, and reads their
// fields.
bool placeSpecs(Crate& c) {
    Atom rootAtom = atomTableFind(&c.layer->strings, "/");
    uint32_t root = kPrUsdNone;
    for (uint32_t p = 0; p < c.paths.count && root == kPrUsdNone; ++p) {
        if (c.paths.data[p] == "/" && rootAtom != kAtomNone)
            root = c.specOfPath.data[p];
    }
    if (root == kPrUsdNone || c.specs.data[root].form != CrateForm_Layer)
        return invalid(c, "the file has no layer spec");
    // The path that each text names, to find a child by the path that its parent's path and its name make.
    RawArray<uint32_t> pathOfAtom{};
    for (uint32_t p = 0; p < c.paths.count; ++p) {
        if (c.paths.data[p].size == 0)
            continue;
        Atom atom = atomTableFind(&c.layer->strings, c.paths.data[p]);
        while (pathOfAtom.count <= atom)
            pathOfAtom.push(kPrUsdNone);
        pathOfAtom.data[atom] = p;
    }
    RawArray<PendingSpec> pending{};
    c.specs.data[root].spec = kPrUsdLayerSpec;
    bool ok = readSpecFields(c, c.specs.data[root]);
    pending.push(PendingSpec{root, 0, 0, false, PrUsdValue{}});
    uint32_t placed = 1;
    while (ok && pending.count != 0) {
        PendingSpec& top = pending.last();
        if (top.list == 4) {
            pending.pop();
            continue;
        }
        const CrateSpec parent = c.specs.data[top.spec];
        const ChildList& list = kChildLists[top.list];
        if (!top.hasNames) {
            uint64_t rep = 0;
            if (!findRep(c, parent.fieldSet, list.field, &rep)) {
                ++top.list;
                continue;
            }
            if (!unpack(c, rep, &top.names)) {
                ok = false;
                break;
            }
            if (!top.names.isArray || top.names.type != PrUsdType_Token) {
                ok = invalid(c, "a list of children is not an array of tokens");
                break;
            }
            top.hasNames = true;
            top.next = 0;
        }
        if (top.next >= top.names.count) {
            ++top.list;
            top.hasNames = false;
            continue;
        }
        PrStringView name = static_cast<const PrStringView*>(top.names.data)[top.next++];
        PrUsdSpecForm parentForm = formOf(parent.form);
        bool holds = false;
        bool named = false;
        switch (list.form) {
            case PrUsdSpecForm_Prim:
                holds = parentForm == PrUsdSpecForm_Layer || parentForm == PrUsdSpecForm_Prim ||
                    parentForm == PrUsdSpecForm_Variant;
                named = usdNameIsIdentifier(name);
                break;
            case PrUsdSpecForm_VariantSet:
                holds = parentForm == PrUsdSpecForm_Prim || parentForm == PrUsdSpecForm_Variant;
                named = usdNameIsIdentifier(name);
                break;
            case PrUsdSpecForm_Variant:
                holds = parentForm == PrUsdSpecForm_VariantSet;
                named = usdNameIsVariant(name);
                break;
            default:
                holds = parentForm == PrUsdSpecForm_Prim || parentForm == PrUsdSpecForm_Variant;
                named = usdNameIsProperty(name);
                break;
        }
        if (!holds || !named) {
            ok = invalid(c, "a spec lists a child that its form does not hold, or whose name the grammar does not allow");
            break;
        }
        // The child's form is not known before its spec is found, and a property's two forms make one path.
        bool existed = false;
        PrUsdSpecIndex index = usdLayerAddSpec(c.layer, list.form, parent.spec, name, &existed);
        if (existed) {
            ok = invalid(c, "a spec lists a child twice");
            break;
        }
        Atom atom = atomTableFind(&c.layer->strings, c.layer->specs.data[index].path);
        uint32_t path = atom < pathOfAtom.count ? pathOfAtom.data[atom] : kPrUsdNone;
        uint32_t child = path != kPrUsdNone ? c.specOfPath.data[path] : kPrUsdNone;
        if (child == kPrUsdNone) {
            ok = invalid(c, "a spec lists a child that the file has no spec for");
            break;
        }
        CrateSpec& found = c.specs.data[child];
        PrUsdSpecForm form = formOf(found.form);
        bool matches = isNormative(found.form) && (form == list.form ||
            (list.form == PrUsdSpecForm_Attribute && form == PrUsdSpecForm_Relationship));
        if (!matches) {
            ok = invalid(c, "a child's spec has another form than the list that names it");
            break;
        }
        c.layer->specs.data[index].form = form;
        found.spec = index;
        ++placed;
        ok = readSpecFields(c, found);
        if (pending.count >= 4096) {
            ok = fail(c, PrStatus_Limit, "specs nest too deeply");
            break;
        }
        pending.push(PendingSpec{child, 0, 0, false, PrUsdValue{}});
    }
    // A spec of the specification's forms that no parent lists has no place in the layer.
    for (uint32_t i = 0; ok && i < c.specs.count; ++i) {
        if (isNormative(c.specs.data[i].form) && c.specs.data[i].spec == kPrUsdNone)
            ok = invalid(c, "a spec is not listed by a parent");
    }
    (void)placed;
    pathOfAtom.reset();
    pending.reset();
    return ok;
}

// Reads the table of contents, and the sections in the order that each needs the ones before it.
bool readFile(Crate& c) {
    if (c.size < 24)
        return invalid(c, "the file is shorter than a crate file's header");
    prMemCopy(c.version, c.data + 8, 3);
    char version[12];
    uint32_t digits = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        uint8_t part = c.version[i];
        if (part >= 100)
            version[digits++] = char('0' + part / 100);
        if (part >= 10)
            version[digits++] = char('0' + part / 10 % 10);
        version[digits++] = char('0' + part % 10);
        if (i != 2)
            version[digits++] = '.';
    }
    c.layer->version = usdLayerIntern(c.layer, PrStringView{version, digits});
    if (!usdCrateVersionIsSupported(c.version[0], c.version[1], c.version[2]))
        return fail(c, PrStatus_NotSupported, "the module does not read this version of the crate format");
    if (c.codec == nullptr)
        return fail(c, PrStatus_NotSupported, "a crate file needs a codec that decodes LZ4");
    Reader r{c, 16};
    uint64_t toc = 0;
    uint64_t count = 0;
    r.read64(&toc);
    r.at = toc;
    if (toc > c.size || !r.read64(&count))
        return invalid(c, "the table of contents lies outside the file");
    if (!inside(c, r.at, count, 32))
        return invalid(c, "the table of contents lies outside the file");
    static const PrStringView kNames[] = {"TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS"};
    Section sections[6] = {};
    for (uint64_t i = 0; i < count; ++i) {
        char name[16];
        uint64_t start = 0;
        uint64_t size = 0;
        r.read(name, 16);
        r.read64(&start);
        r.read64(&size);
        size_t length = 0;
        while (length < 16 && name[length] != 0)
            ++length;
        for (uint32_t s = 0; s < 6; ++s) {
            if (kNames[s] != PrStringView{name, length})
                continue;
            if (start > c.size || size > c.size - start)
                return invalid(c, "a section lies outside the file");
            sections[s] = Section{start, size, true};
        }
    }
    for (const Section& section : sections) {
        if (!section.found)
            return invalid(c, "the file lacks one of the format's sections");
    }
    return readTokens(c, sections[0]) && readStrings(c, sections[1]) && readFields(c, sections[2]) &&
        readFieldSets(c, sections[3]) && readPaths(c, sections[4]) && readSpecs(c, sections[5]) && placeSpecs(c);
}

} // namespace

bool usdCrateVersionIsSupported(uint8_t major, uint8_t minor, uint8_t) {
    return major == 0 && minor >= 4 && minor <= 12;
}

bool usdCrateMatches(PrByteView bytes) {
    return bytes.size >= 8 && prMemCompare(bytes.data, "PXR-USDC", 8) == 0;
}

PrStatus usdCrateRead(UsdLayer* layer, PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec,
        PrTextBuffer* PR_NULLABLE diagnostics) {
    Crate c{};
    c.layer = layer;
    c.data = bytes.data;
    c.size = bytes.size;
    c.codec = codec;
    c.diagnostics = diagnostics;
    readFile(c);
    c.tokens.reset();
    c.strings.reset();
    c.fieldTokens.reset();
    c.fieldReps.reset();
    c.fieldSets.reset();
    c.paths.reset();
    c.specs.reset();
    c.specOfPath.reset();
    return c.status;
}

} // namespace pr_impl
