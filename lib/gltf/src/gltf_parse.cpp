// Reading glTF JSON into a document, and checking it.
//
// The asset is read array by array, in an order that lets each object be checked against what it refers to as it is
// read. Once every array is read, the checks that depend on the bytes of buffers run. A failed check records the first
// status and its diagnostic, and reading carries on over safe defaults to the end of the array, where it stops.

#include "gltf/src/gltf_parse.h"

#include "pr/ascii.h"
#include "pr/base64.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/json.h"
#include "pr/mem.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "gltf/src/gltf_accessor.h"
#include "gltf/src/gltf_draco.h"
#include "gltf/src/gltf_meshopt.h"

namespace pr_impl {

namespace {

// The extensions that the module reads.
constexpr PrStringView kAnimationPointer = "KHR_animation_pointer";
constexpr PrStringView kInstancing = "EXT_mesh_gpu_instancing";
constexpr PrStringView kMeshoptExt = "EXT_meshopt_compression";
constexpr PrStringView kMeshoptKhr = "KHR_meshopt_compression";
constexpr PrStringView kDraco = "KHR_draco_mesh_compression";
constexpr PrStringView kQuantization = "KHR_mesh_quantization";
constexpr PrStringView kNodeVisibility = "KHR_node_visibility";
constexpr PrStringView kVariants = "KHR_materials_variants";
constexpr PrStringView kAnisotropy = "KHR_materials_anisotropy";
constexpr PrStringView kBasisu = "KHR_texture_basisu";
constexpr PrStringView kClearcoat = "KHR_materials_clearcoat";
constexpr PrStringView kDiffuseTransmission = "KHR_materials_diffuse_transmission";
constexpr PrStringView kDispersion = "KHR_materials_dispersion";
constexpr PrStringView kEmissiveStrength = "KHR_materials_emissive_strength";
constexpr PrStringView kIor = "KHR_materials_ior";
constexpr PrStringView kIridescence = "KHR_materials_iridescence";
constexpr PrStringView kLightsPunctual = "KHR_lights_punctual";
constexpr PrStringView kSheen = "KHR_materials_sheen";
constexpr PrStringView kSpecular = "KHR_materials_specular";
constexpr PrStringView kSpecularGlossiness = "KHR_materials_pbrSpecularGlossiness";
constexpr PrStringView kTextureTransform = "KHR_texture_transform";
constexpr PrStringView kTransmission = "KHR_materials_transmission";
constexpr PrStringView kUnlit = "KHR_materials_unlit";
constexpr PrStringView kVolume = "KHR_materials_volume";
constexpr PrStringView kWebp = "EXT_texture_webp";

// The greatest integer that a JSON number holds exactly.
constexpr double kMaxInteger = 9007199254740992.0;

// The number of elements that a check of a buffer's bytes unpacks at a time.
constexpr uint32_t kCheckBatch = 256;

// What reading the elements of an accessor found.
struct GltfScan {
    bool scanned;
    bool hasBytes;              // false if a buffer that the accessor reads has no bytes, so nothing else is known
    bool isTimes;               // true if the elements are finite, start at zero or more, and increase strictly
    uint32_t greatest;          // the greatest component, as an integer
};

// The number of attributes of each semantic that a primitive or a morph target has, and the greatest set among them.
struct GltfAttributeSets {
    uint32_t count[PrGltfSemantic_Custom];      // by PrGltfSemantic
    uint32_t greatest[PrGltfSemantic_Custom];   // by PrGltfSemantic
};

// What a group of attributes belongs to, which decides the types that the specification gives each semantic.
enum {
    GltfAttributeOwner_Primitive,
    GltfAttributeOwner_MorphTarget,
};
typedef uint8_t GltfAttributeOwner;

// The state of reading one asset.
struct GltfParser {
    Gltf* gltf;
    PrJson* json;
    const PrGltfOptions* options;
    PrByteView binaryChunk;
    PrStatus status;

    // The object that is being read, which a diagnostic names: element `index` of the root's array `kind`, within it
    // element `partIndex` of its array `part`, and within that the member `scope`. A name is null if there is none.
    const char* PR_NULLABLE kind;
    uint32_t index;
    const char* PR_NULLABLE part;
    uint32_t partIndex;
    const char* PR_NULLABLE scope;

    // The number of elements in each array of the root that another object can name before the array is read.
    uint32_t nodeCount;
    uint32_t skinCount;

    bool quantized;                     // the asset uses KHR_mesh_quantization, which widens the types of attributes

    RawArray<uint32_t> morphCounts;     // by mesh: the number of morph targets
    RawArray<uint32_t> jointSets;       // by primitive: the number of its sets of joints

    // What the checks of bytes have found, kept so that an accessor that many objects share is read once.
    RawArray<GltfScan> scans;           // by accessor
    RawArray<uint32_t> jointLimits;     // by mesh: one more than the greatest joint that a vertex names, or zero
    RawArray<bool> skinnable;           // by mesh: true if every primitive has joints

    // Marks that find a repeated item in a list: an item is marked with the stamp of the list being read.
    RawArray<uint32_t> marks;
    uint32_t stamp;
};

// A name of a JSON enumeration and the value that it reads as.
struct GltfName {
    PrStringView name;
    uint8_t value;
};

// A number of a JSON enumeration and the value that it reads as.
struct GltfCode {
    uint32_t code;
    uint8_t value;
};

bool failed(const GltfParser* parser) {
    return parser->status != PrStatus_Ok;
}

// Records that reading failed with `status` because `property` of the object being read `message`. Only the first
// failure is kept.
void failWith(GltfParser* parser, PrStatus status, PrStringView property, PrStringView message) {
    if (failed(parser))
        return;
    parser->status = status;
    PrTextBuffer* out = parser->options->diagnostics;
    if (out == nullptr)
        return;
    // The path of the property, each name after the first following a dot.
    char buffer[96];
    uint32_t start = out->size;
    if (parser->kind != nullptr)
        prTextBufferAppend(out, formatToBuffer(Span<char>(buffer), "{}[{}]", parser->kind, parser->index));
    if (parser->part != nullptr)
        prTextBufferAppend(out, formatToBuffer(Span<char>(buffer), ".{}[{}]", parser->part, parser->partIndex));
    if (parser->scope != nullptr) {
        if (out->size != start)
            prTextBufferAppendChar(out, '.');
        prTextBufferAppend(out, parser->scope);
    }
    if (property.size != 0) {
        if (out->size != start)
            prTextBufferAppendChar(out, '.');
        prTextBufferAppendUtf8(out, property);
    }
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppend(out, message);
}

void fail(GltfParser* parser, PrStringView property, PrStringView message) {
    failWith(parser, PrStatus_InvalidData, property, message);
}

// ---------------------------------------------------------------------------------------------------------------------
// JSON values

PrJsonValueIndex member(const GltfParser* parser, PrJsonValueIndex object, PrStringView key) {
    return prJsonMember(parser->json, object, key);
}

// Returns the member `key` of `object` if it is an object or an array, as `kind` says. Returns kPrJsonMissing if there
// is no such member, and fails if it is of another kind.
PrJsonValueIndex container(GltfParser* parser, PrJsonValueIndex object, PrStringView key, PrJsonKind kind) {
    PrJsonValueIndex value = member(parser, object, key);
    if (value == kPrJsonMissing || prJsonKind(parser->json, value) == kind)
        return value;
    fail(parser, key, kind == PrJsonKind_Object ? "must be an object" : "must be an array");
    return kPrJsonMissing;
}

// Returns the object of the extension `name` in the `extensions` of `object`, or kPrJsonMissing.
PrJsonValueIndex extension(GltfParser* parser, PrJsonValueIndex object, PrStringView name) {
    PrJsonValueIndex extensions = container(parser, object, "extensions", PrJsonKind_Object);
    if (extensions == kPrJsonMissing)
        return kPrJsonMissing;
    return container(parser, extensions, name, PrJsonKind_Object);
}

// Reads the number at `value`, which `property` names in a diagnostic. Returns false if it is not a finite number.
bool numberAt(GltfParser* parser, PrJsonValueIndex value, PrStringView property, double* out) {
    if (prJsonKind(parser->json, value) != PrJsonKind_Number) {
        fail(parser, property, "must be a number");
        return false;
    }
    *out = prJsonNumber(parser->json, value, 0);
    if (!prF64IsFinite(*out)) {
        fail(parser, property, "must be finite");
        return false;
    }
    return true;
}

// Reads the integer at `value`, which must not be negative.
bool integerAt(GltfParser* parser, PrJsonValueIndex value, PrStringView property, uint64_t* out) {
    double number = 0;
    if (!numberAt(parser, value, property, &number))
        return false;
    if (number < 0 || number > kMaxInteger || number != prF64Floor(number)) {
        fail(parser, property, "must be a non-negative integer");
        return false;
    }
    *out = uint64_t(number);
    return true;
}

// Reads the float at `value`, which must lie in [low, high].
bool floatAt(GltfParser* parser, PrJsonValueIndex value, PrStringView property, float low, float high, float* out) {
    double number = 0;
    if (!numberAt(parser, value, property, &number))
        return false;
    float narrowed = float(number);
    if (!prF32IsFinite(narrowed) || narrowed < low || narrowed > high) {
        fail(parser, property, "is out of range");
        return false;
    }
    *out = narrowed;
    return true;
}

// Returns the member `key` as a float in [low, high], or `fallback` if the object has no such member.
float readFloat(GltfParser* parser, PrJsonValueIndex object, PrStringView key, float fallback, float low, float high) {
    PrJsonValueIndex value = member(parser, object, key);
    float result = fallback;
    if (value != kPrJsonMissing)
        floatAt(parser, value, key, low, high, &result);
    return result;
}

// Reads the member `key`, an array of exactly `count` floats in [low, high], into `out`. Leaves `out` as it is if the
// object has no such member. Returns true if the member is there.
bool readFloats(GltfParser* parser, PrJsonValueIndex object, PrStringView key, float* out, uint32_t count,
        float low, float high) {
    PrJsonValueIndex array = container(parser, object, key, PrJsonKind_Array);
    if (array == kPrJsonMissing)
        return false;
    if (prJsonCount(parser->json, array) != count) {
        fail(parser, key, "has the wrong number of elements");
        return false;
    }
    uint32_t at = 0;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, array); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, array, i)) {
        if (!floatAt(parser, i, key, low, high, &out[at++]))
            return false;
    }
    return true;
}

// Reads the member `key` as an integer. Returns false if the object has no such member, or it is not an integer.
bool readInteger(GltfParser* parser, PrJsonValueIndex object, PrStringView key, uint64_t* out) {
    PrJsonValueIndex value = member(parser, object, key);
    return value != kPrJsonMissing && integerAt(parser, value, key, out);
}

// Returns the index at `value`, which must be less than `count`, the size of the array that it indexes.
uint32_t indexAt(GltfParser* parser, PrJsonValueIndex value, PrStringView property, uint32_t count) {
    uint64_t index = 0;
    if (!integerAt(parser, value, property, &index))
        return kPrGltfNone;
    if (index >= count) {
        fail(parser, property, "is not the index of an object");
        return kPrGltfNone;
    }
    return uint32_t(index);
}

// Returns the member `key` as an index into an array of `count` objects, or kPrGltfNone if the object has no such
// member.
uint32_t readIndex(GltfParser* parser, PrJsonValueIndex object, PrStringView key, uint32_t count) {
    PrJsonValueIndex value = member(parser, object, key);
    return value != kPrJsonMissing ? indexAt(parser, value, key, count) : kPrGltfNone;
}

// Returns the member `key` as an index, and fails if the object has no such member.
uint32_t requireIndex(GltfParser* parser, PrJsonValueIndex object, PrStringView key, uint32_t count) {
    PrJsonValueIndex value = member(parser, object, key);
    if (value == kPrJsonMissing) {
        fail(parser, key, "is required");
        return kPrGltfNone;
    }
    return indexAt(parser, value, key, count);
}

// Returns a copy of the string member `key` that the document keeps, or the null view if the object has no such member.
PrStringView readString(GltfParser* parser, PrJsonValueIndex object, PrStringView key) {
    PrJsonValueIndex value = member(parser, object, key);
    if (value == kPrJsonMissing)
        return PrStringView{nullptr, 0};
    if (prJsonKind(parser->json, value) != PrJsonKind_String) {
        fail(parser, key, "must be a string");
        return PrStringView{nullptr, 0};
    }
    return gltfCopyString(parser->gltf, prJsonString(parser->json, value));
}

bool readBool(GltfParser* parser, PrJsonValueIndex object, PrStringView key, bool fallback) {
    PrJsonValueIndex value = member(parser, object, key);
    if (value == kPrJsonMissing)
        return fallback;
    PrJsonKind kind = prJsonKind(parser->json, value);
    if (kind != PrJsonKind_True && kind != PrJsonKind_False) {
        fail(parser, key, "must be true or false");
        return fallback;
    }
    return kind == PrJsonKind_True;
}

// Returns the value that `names` gives the string member `key`, or `fallback` if the object has no such member.
uint8_t readName(GltfParser* parser, PrJsonValueIndex object, PrStringView key, Span<const GltfName> names,
        uint8_t fallback) {
    PrJsonValueIndex value = member(parser, object, key);
    if (value == kPrJsonMissing)
        return fallback;
    PrStringView text = prJsonString(parser->json, value);
    for (const GltfName& name : names) {
        if (text.data != nullptr && text == name.name)
            return name.value;
    }
    fail(parser, key, "is not a value of its enumeration");
    return fallback;
}

// Returns the value that `codes` gives the integer member `key`, or `fallback` if the object has no such member.
uint8_t readCode(GltfParser* parser, PrJsonValueIndex object, PrStringView key, Span<const GltfCode> codes,
        uint8_t fallback) {
    uint64_t number = 0;
    if (member(parser, object, key) == kPrJsonMissing || !readInteger(parser, object, key, &number))
        return fallback;
    for (const GltfCode& code : codes) {
        if (number == code.code)
            return code.value;
    }
    fail(parser, key, "is not a value of its enumeration");
    return fallback;
}

// Begins a list whose items must not repeat.
void beginUnique(GltfParser* parser, uint32_t count) {
    if (parser->marks.count < count) {
        parser->marks.reset();
        parser->marks.resizeZeroed(count);
        parser->stamp = 0;
    }
    ++parser->stamp;
}

// Returns false if `item` was already added to the list that beginUnique began.
bool addUnique(GltfParser* parser, uint32_t item) {
    if (parser->marks.data[item] == parser->stamp)
        return false;
    parser->marks.data[item] = parser->stamp;
    return true;
}

// Reads the member `key`, an array of indices into an array of `count` objects, as a list, in which no index repeats.
// An empty array reads as no list.
PrGltfList readIndexList(GltfParser* parser, PrJsonValueIndex object, PrStringView key, uint32_t count) {
    PrGltfList list = {parser->gltf->listIndices.count, 0};
    PrJsonValueIndex array = container(parser, object, key, PrJsonKind_Array);
    if (array == kPrJsonMissing)
        return list;
    beginUnique(parser, count);
    for (PrJsonValueIndex i = prJsonFirst(parser->json, array); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, array, i)) {
        uint32_t index = indexAt(parser, i, key, count);
        if (index == kPrGltfNone)
            break;
        if (!addUnique(parser, index)) {
            fail(parser, key, "names an object twice");
            break;
        }
        parser->gltf->listIndices.push(index);
        ++list.count;
    }
    return list;
}

// Reads the member `key`, an array of floats, as a list.
PrGltfList readFloatList(GltfParser* parser, PrJsonValueIndex object, PrStringView key) {
    PrGltfList list = {parser->gltf->listFloats.count, 0};
    PrJsonValueIndex array = container(parser, object, key, PrJsonKind_Array);
    if (array == kPrJsonMissing)
        return list;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, array); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, array, i)) {
        float value = 0;
        if (!floatAt(parser, i, key, -kPrF32Infinity, kPrF32Infinity, &value))
            break;
        parser->gltf->listFloats.push(value);
        ++list.count;
    }
    return list;
}

// Calls `read` with each object of the root's array `kind`, with the parser naming that object. Stops at the first
// object that fails, and does nothing if reading has already failed.
template <typename Read>
void eachObject(GltfParser* parser, const char* kind, PrJsonValueIndex array, Read read) {
    if (array == kPrJsonMissing || failed(parser))
        return;
    parser->kind = kind;
    parser->index = 0;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, array); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, array, i)) {
        if (prJsonKind(parser->json, i) != PrJsonKind_Object)
            fail(parser, "", "must be an object");
        else
            read(i);
        if (failed(parser))
            break;
        ++parser->index;
    }
    parser->kind = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// URIs

bool isDataUri(PrStringView uri) {
    return prAsciiStartsWithFold(uri, "data:");
}

// Decodes a data: URL (RFC 2397) into bytes that the document keeps, and writes its media type, without parameters, to
// `*mediaType`. Returns false if the URL is malformed.
bool decodeDataUri(Gltf* gltf, PrStringView uri, PrByteView* bytes, PrStringView* mediaType) {
    PrStringView rest = prStringFrom(uri, 5);
    size_t comma = prStringFindByte(rest, ',');
    if (comma >= rest.size)
        return false;
    PrStringView header = prStringHead(rest, comma);
    PrStringView data = prStringFrom(rest, comma + 1);
    bool base64 = prAsciiEndsWithFold(header, ";base64");
    size_t semicolon = prStringFindByte(header, ';');
    *mediaType = prStringHead(header, semicolon < header.size ? semicolon : header.size);

    if (base64) {
        size_t capacity = prBase64DecodedCapacity(data.size);
        char* out = reinterpret_cast<char*>(gltfAllocDecoded(gltf, capacity));
        size_t size = 0;
        if (!prBase64Decode(data, out, capacity, &size))
            return false;
        *bytes = PrByteView{reinterpret_cast<const uint8_t*>(out), size};
        return true;
    }

    // The data is the bytes themselves, with those outside the URL's characters percent-encoded.
    uint8_t* out = gltfAllocDecoded(gltf, data.size);
    size_t size = 0;
    for (size_t i = 0; i < data.size; ++i) {
        char c = data.data[i];
        if (c == '%' && i + 2 < data.size && prAsciiIsHexDigit(data.data[i + 1]) && prAsciiIsHexDigit(data.data[i + 2])) {
            out[size++] = uint8_t(prAsciiHexValue(data.data[i + 1]) * 16 + prAsciiHexValue(data.data[i + 2]));
            i += 2;
        } else {
            out[size++] = uint8_t(c);
        }
    }
    *bytes = PrByteView{out, size};
    return true;
}

// Returns the bytes that the host loads for `uri`, which the document keeps, or the null view.
PrByteView loadExternal(GltfParser* parser, PrGltfExternal kind, PrStringView uri) {
    if (parser->options->load == nullptr)
        return PrByteView{nullptr, 0};
    PrBlob* blob = parser->options->load(parser->options->loadContext, kind, uri);
    if (blob == nullptr)
        return PrByteView{nullptr, 0};
    parser->gltf->blobs.push(blob);
    return prBlobBytes(blob);
}

// Returns the media type of an image whose bytes begin as a PNG or a JPEG file does (glTF-2.0/#images-overview), or as
// the formats of the texture extensions do, a WebP file (a RIFF file of the form WEBP) or a KTX 2 file, or the null
// view.
PrStringView sniffImage(PrByteView bytes) {
    static const uint8_t kPng[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    static const uint8_t kJpeg[] = {0xff, 0xd8, 0xff};
    static const uint8_t kKtx2[] = {0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size >= sizeof(kPng) && prMemEquals(bytes.data, kPng, sizeof(kPng)))
        return "image/png";
    if (bytes.size >= sizeof(kJpeg) && prMemEquals(bytes.data, kJpeg, sizeof(kJpeg)))
        return "image/jpeg";
    if (bytes.size >= 12 && prMemEquals(bytes.data, "RIFF", 4) && prMemEquals(bytes.data + 8, "WEBP", 4))
        return "image/webp";
    if (bytes.size >= sizeof(kKtx2) && prMemEquals(bytes.data, kKtx2, sizeof(kKtx2)))
        return "image/ktx2";
    return PrStringView{nullptr, 0};
}

// ---------------------------------------------------------------------------------------------------------------------
// The asset and its extensions

// Reads a version string, "<major>.<minor>" in decimal digits (glTF-2.0/#asset).
bool parseVersion(PrStringView text, uint32_t* major, uint32_t* minor) {
    uint32_t parts[2] = {};
    uint32_t part = 0;
    uint32_t digits = 0;
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '.' && part == 0 && digits != 0) {
            part = 1;
            digits = 0;
        } else if (prAsciiIsDigit(c) && digits < 9) {
            parts[part] = parts[part] * 10 + uint32_t(c - '0');
            ++digits;
        } else {
            return false;
        }
    }
    *major = parts[0];
    *minor = parts[1];
    return part == 1 && digits != 0;
}

void readAsset(GltfParser* parser, PrJsonValueIndex root) {
    PrJsonValueIndex asset = container(parser, root, "asset", PrJsonKind_Object);
    if (asset == kPrJsonMissing) {
        fail(parser, "asset", "is required");
        return;
    }
    PrGltfAsset& out = parser->gltf->asset;
    parser->scope = "asset";
    out.version = readString(parser, asset, "version");
    out.minVersion = readString(parser, asset, "minVersion");
    out.generator = readString(parser, asset, "generator");
    out.copyright = readString(parser, asset, "copyright");

    uint32_t major = 0;
    uint32_t minor = 0;
    if (out.version.data == nullptr) {
        fail(parser, "version", "is required");
    } else if (!parseVersion(out.version, &major, &minor)) {
        fail(parser, "version", "is not a version");
    } else if (major != 2) {
        failWith(parser, PrStatus_NotSupported, "version", "is not glTF 2");
    } else if (out.minVersion.data != nullptr) {
        // A minimum version opts the asset out of being read by an earlier minor version (glTF-2.0/#versioning).
        if (!parseVersion(out.minVersion, &major, &minor))
            fail(parser, "minVersion", "is not a version");
        else if (major != 2 || minor != 0)
            failWith(parser, PrStatus_NotSupported, "minVersion", "is later than glTF 2.0");
    }
    parser->scope = nullptr;
}

void readExtensions(GltfParser* parser, PrJsonValueIndex root) {
    PrJsonValueIndex used = container(parser, root, "extensionsUsed", PrJsonKind_Array);
    if (used != kPrJsonMissing) {
        for (PrJsonValueIndex i = prJsonFirst(parser->json, used); i != kPrJsonMissing;
                i = prJsonAfter(parser->json, used, i)) {
            if (prJsonKind(parser->json, i) != PrJsonKind_String) {
                fail(parser, "extensionsUsed", "must hold strings");
                return;
            }
            PrStringView name = prJsonString(parser->json, i);
            parser->gltf->usedExtensions.push(gltfCopyString(parser->gltf, name));
            parser->quantized = parser->quantized || name == kQuantization;
        }
    }
    PrJsonValueIndex required = container(parser, root, "extensionsRequired", PrJsonKind_Array);
    if (required == kPrJsonMissing)
        return;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, required); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, required, i)) {
        PrStringView name = prJsonString(parser->json, i);
        if (name.data == nullptr) {
            fail(parser, "extensionsRequired", "must hold strings");
            return;
        }
        parser->quantized = parser->quantized || name == kQuantization;
        if (!prGltfExtensionIsSupported(name)) {
            failWith(parser, PrStatus_NotSupported, name, "is a required extension that the module does not read");
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Binary data

// Returns the object of whichever meshopt extension `object` has, and sets `*khr` if it is KHR_meshopt_compression.
// Fails if it has both, which the extensions forbid.
PrJsonValueIndex meshoptExtension(GltfParser* parser, PrJsonValueIndex object, bool* khr) {
    PrJsonValueIndex ext = extension(parser, object, kMeshoptExt);
    PrJsonValueIndex ratified = extension(parser, object, kMeshoptKhr);
    if (ext != kPrJsonMissing && ratified != kPrJsonMissing)
        fail(parser, "extensions", "must not have both EXT_meshopt_compression and KHR_meshopt_compression");
    *khr = ratified != kPrJsonMissing;
    return ratified != kPrJsonMissing ? ratified : ext;
}

void readBuffer(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfBuffer buffer = {};
    buffer.name = readString(parser, object, "name");
    uint64_t byteLength = 0;
    if (!readInteger(parser, object, "byteLength", &byteLength) || byteLength == 0)
        fail(parser, "byteLength", "must be at least 1");
    buffer.byteLength = byteLength;
    bool khr = false;
    PrJsonValueIndex meshopt = meshoptExtension(parser, object, &khr);
    if (meshopt != kPrJsonMissing) {
        parser->scope = khr ? kMeshoptKhr.data : kMeshoptExt.data;
        buffer.fallback = readBool(parser, meshopt, "fallback", false);
        parser->scope = nullptr;
    }

    PrJsonValueIndex uri = member(parser, object, "uri");
    PrStringView text = prJsonString(parser->json, uri);
    if (uri != kPrJsonMissing && text.data == nullptr) {
        fail(parser, "uri", "must be a string");
    } else if (uri == kPrJsonMissing) {
        // The first buffer of binary glTF is the binary chunk (glTF-2.0/#glb-stored-buffer). Any other buffer with no
        // URI has no source that the specification defines.
        if (parser->index == 0)
            buffer.bytes = parser->binaryChunk;
    } else if (isDataUri(text)) {
        PrStringView mediaType = {};
        buffer.uri = gltfCopyString(gltf, "");
        if (!decodeDataUri(gltf, text, &buffer.bytes, &mediaType))
            fail(parser, "uri", "is a malformed data: URI");
    } else {
        buffer.uri = gltfCopyString(gltf, text);
        // A fallback holds what the compressed views decode to, which the module decodes instead of loading it.
        if (!buffer.fallback)
            buffer.bytes = loadExternal(parser, PrGltfExternal_Buffer, buffer.uri);
    }
    if (buffer.bytes.data != nullptr && buffer.bytes.size < buffer.byteLength)
        fail(parser, "byteLength", "is more than the bytes that the buffer has");
    gltf->buffers.push(buffer);
}

// Reads a buffer view's compressed form, checks it against the rules of its extension, and decodes it into bytes that
// the document keeps, if its buffer has bytes.
void readMeshopt(GltfParser* parser, PrJsonValueIndex object, PrGltfBufferView* view) {
    static const GltfName kModes[] = {
        {"ATTRIBUTES", PrGltfMeshoptMode_Attributes},
        {"TRIANGLES", PrGltfMeshoptMode_Triangles},
        {"INDICES", PrGltfMeshoptMode_Indices},
    };
    static const GltfName kFilters[] = {
        {"NONE", PrGltfMeshoptFilter_None},
        {"OCTAHEDRAL", PrGltfMeshoptFilter_Octahedral},
        {"QUATERNION", PrGltfMeshoptFilter_Quaternion},
        {"EXPONENTIAL", PrGltfMeshoptFilter_Exponential},
        {"COLOR", PrGltfMeshoptFilter_Color},
    };
    Gltf* gltf = parser->gltf;
    PrGltfMeshopt& m = view->meshopt;
    m.buffer = kPrGltfNone;
    bool khr = false;
    PrJsonValueIndex meshopt = meshoptExtension(parser, object, &khr);
    if (failed(parser))
        return;
    // Only a compressed view names a fallback buffer.
    if (meshopt == kPrJsonMissing) {
        if (gltf->buffers.data[view->buffer].fallback)
            fail(parser, "buffer", "is a fallback buffer, which only a compressed buffer view names");
        return;
    }
    parser->scope = khr ? kMeshoptKhr.data : kMeshoptExt.data;
    m.v1 = khr;
    m.buffer = requireIndex(parser, meshopt, "buffer", gltf->buffers.count);
    readInteger(parser, meshopt, "byteOffset", &m.byteOffset);
    if (!readInteger(parser, meshopt, "byteLength", &m.byteLength) || m.byteLength == 0)
        fail(parser, "byteLength", "must be at least 1");
    uint64_t stride = 0;
    if (!readInteger(parser, meshopt, "byteStride", &stride) || stride == 0 || stride > 256)
        fail(parser, "byteStride", "must be from 1 to 256");
    uint64_t count = 0;
    if (!readInteger(parser, meshopt, "count", &count) || count == 0 || count >= kPrGltfNone)
        fail(parser, "count", "must be at least 1");
    if (member(parser, meshopt, "mode") == kPrJsonMissing)
        fail(parser, "mode", "is required");
    m.mode = readName(parser, meshopt, "mode", Span<const GltfName>(kModes), PrGltfMeshoptMode_Attributes);
    m.filter = readName(parser, meshopt, "filter", Span<const GltfName>(kFilters), PrGltfMeshoptFilter_None);
    m.byteStride = uint16_t(stride);
    m.count = uint32_t(count);
    if (failed(parser)) {
        parser->scope = nullptr;
        return;
    }

    // The rules of the extensions' JSON (KHR_meshopt_compression's "JSON schema updates").
    bool indices = m.mode != PrGltfMeshoptMode_Attributes;
    const PrGltfBuffer& source = gltf->buffers.data[m.buffer];
    if (source.fallback)
        fail(parser, "buffer", "is a fallback buffer, which holds no compressed data");
    else if (m.byteOffset > source.byteLength || m.byteLength > source.byteLength - m.byteOffset)
        fail(parser, "byteLength", "runs past the end of the buffer");
    else if (view->byteLength != uint64_t(m.byteStride) * m.count)
        fail(parser, "count", "times byteStride is not the buffer view's byteLength");
    else if (!indices && m.byteStride % 4 != 0)
        fail(parser, "byteStride", "of attributes must be a multiple of 4");
    else if (indices && m.byteStride != 2 && m.byteStride != 4)
        fail(parser, "byteStride", "of indices must be 2 or 4");
    else if (m.mode == PrGltfMeshoptMode_Triangles && m.count % 3 != 0)
        fail(parser, "count", "of triangles' indices must be a multiple of 3");
    else if (indices && m.filter != PrGltfMeshoptFilter_None)
        fail(parser, "filter", "must be NONE for indices");
    else if (!khr && m.filter == PrGltfMeshoptFilter_Color)
        fail(parser, "filter", "is not a value of its enumeration");
    else if (!khr && view->byteStride != 0 && view->byteStride != m.byteStride)
        fail(parser, "byteStride", "is not the buffer view's");
    else if (m.filter == PrGltfMeshoptFilter_Octahedral && m.byteStride != 4 && m.byteStride != 8)
        fail(parser, "byteStride", "of the OCTAHEDRAL filter must be 4 or 8");
    else if (m.filter == PrGltfMeshoptFilter_Quaternion && m.byteStride != 8)
        fail(parser, "byteStride", "of the QUATERNION filter must be 8");
    else if (m.filter == PrGltfMeshoptFilter_Color && m.byteStride != 4 && m.byteStride != 8)
        fail(parser, "byteStride", "of the COLOR filter must be 4 or 8");

    // The view's bytes are those that its stream decodes to, and it has none while the stream's buffer has none.
    if (!failed(parser) && source.bytes.data != nullptr) {
        uint8_t* decoded = gltfAllocDecoded(gltf, size_t(view->byteLength));
        PrByteView stream = {source.bytes.data + m.byteOffset, size_t(m.byteLength)};
        if (gltfMeshoptDecode(stream, m, decoded))
            gltf->decodedViews.data[gltf->bufferViews.count] = decoded;
        else
            fail(parser, "", "is not a stream that the extension defines");
    }
    parser->scope = nullptr;
}

void readBufferView(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfCode kTargets[] = {{34962, PrGltfBufferTarget_Vertices}, {34963, PrGltfBufferTarget_Indices}};
    Gltf* gltf = parser->gltf;
    PrGltfBufferView view = {};
    view.name = readString(parser, object, "name");
    view.buffer = requireIndex(parser, object, "buffer", gltf->buffers.count);
    view.target = readCode(parser, object, "target", Span<const GltfCode>(kTargets), PrGltfBufferTarget_None);
    readInteger(parser, object, "byteOffset", &view.byteOffset);
    if (!readInteger(parser, object, "byteLength", &view.byteLength) || view.byteLength == 0)
        fail(parser, "byteLength", "must be at least 1");
    uint64_t stride = 0;
    if (readInteger(parser, object, "byteStride", &stride)) {
        if (stride < 4 || stride > 252 || stride % 4 != 0)
            fail(parser, "byteStride", "must be a multiple of 4 from 4 to 252");
        else if (stride > view.byteLength)
            fail(parser, "byteStride", "is more than the byteLength");
        view.byteStride = uint8_t(stride);
    }
    if (!failed(parser)) {
        uint64_t available = gltf->buffers.data[view.buffer].byteLength;
        if (view.byteOffset > available || view.byteLength > available - view.byteOffset)
            fail(parser, "byteLength", "runs past the end of the buffer");
    }
    gltf->decodedViews.push(nullptr);
    view.meshopt.buffer = kPrGltfNone;
    if (!failed(parser))
        readMeshopt(parser, object, &view);
    gltf->bufferViews.push(view);
}

// Checks that `count` elements of `size` bytes, `stride` bytes apart, from `byteOffset` lie within a buffer view, and
// that the offsets are multiples of the component size (glTF-2.0/#data-alignment). `property` is the property that
// names the buffer view.
void checkElements(GltfParser* parser, PrStringView property, PrGltfBufferViewIndex bufferView, uint64_t byteOffset,
        uint64_t count, uint64_t stride, uint64_t size, uint32_t componentSize) {
    if (failed(parser))
        return;
    const PrGltfBufferView& view = parser->gltf->bufferViews.data[bufferView];
    if (byteOffset % componentSize != 0 || view.byteOffset % componentSize != 0) {
        fail(parser, property, "is not aligned to the size of a component");
        return;
    }
    // An offset is at most 2^53, a count less than 2^32 and a stride less than 2^8, so the sum cannot overflow.
    if (byteOffset + (count - 1) * stride + size > view.byteLength)
        fail(parser, property, "has elements past the end of the buffer view");
}

void readSparse(GltfParser* parser, PrJsonValueIndex object, PrGltfAccessor* accessor) {
    static const GltfCode kIndexTypes[] = {
        {5121, PrGltfComponentType_UnsignedByte},
        {5123, PrGltfComponentType_UnsignedShort},
        {5125, PrGltfComponentType_UnsignedInt},
    };
    Gltf* gltf = parser->gltf;
    PrGltfSparse& sparse = accessor->sparse;
    sparse.indicesBufferView = kPrGltfNone;
    sparse.valuesBufferView = kPrGltfNone;
    PrJsonValueIndex value = container(parser, object, "sparse", PrJsonKind_Object);
    if (value == kPrJsonMissing)
        return;
    parser->scope = "sparse";
    uint64_t count = 0;
    if (!readInteger(parser, value, "count", &count) || count == 0 || count > accessor->count)
        fail(parser, "count", "must be from 1 to the accessor's count");
    PrJsonValueIndex indices = container(parser, value, "indices", PrJsonKind_Object);
    PrJsonValueIndex values = container(parser, value, "values", PrJsonKind_Object);
    if (indices == kPrJsonMissing || values == kPrJsonMissing)
        fail(parser, "", "must have indices and values");
    parser->scope = "sparse.indices";
    sparse.indicesBufferView = requireIndex(parser, indices, "bufferView", gltf->bufferViews.count);
    readInteger(parser, indices, "byteOffset", &sparse.indicesByteOffset);
    if (member(parser, indices, "componentType") == kPrJsonMissing)
        fail(parser, "componentType", "is required");
    sparse.indicesComponentType =
        readCode(parser, indices, "componentType", Span<const GltfCode>(kIndexTypes), PrGltfComponentType_UnsignedInt);
    parser->scope = "sparse.values";
    sparse.valuesBufferView = requireIndex(parser, values, "bufferView", gltf->bufferViews.count);
    readInteger(parser, values, "byteOffset", &sparse.valuesByteOffset);
    parser->scope = nullptr;
    if (failed(parser))
        return;
    // Sparse data is tightly packed, so its buffer views have no stride (glTF-2.0/#sparse-accessor-indices).
    if (gltf->bufferViews.data[sparse.indicesBufferView].byteStride != 0 ||
            gltf->bufferViews.data[sparse.valuesBufferView].byteStride != 0) {
        fail(parser, "sparse", "reads a buffer view that has a byteStride");
        return;
    }
    uint32_t indexSize = gltfComponentSize(sparse.indicesComponentType);
    uint32_t elementSize = gltfElementSize(accessor->type, accessor->componentType);
    checkElements(parser, "sparse.indices.bufferView", sparse.indicesBufferView, sparse.indicesByteOffset, count,
        indexSize, indexSize, indexSize);
    checkElements(parser, "sparse.values.bufferView", sparse.valuesBufferView, sparse.valuesByteOffset, count,
        elementSize, elementSize, gltfComponentSize(accessor->componentType));
    sparse.count = uint32_t(count);
}

void readAccessor(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfCode kComponentTypes[] = {
        {5120, PrGltfComponentType_Byte},
        {5121, PrGltfComponentType_UnsignedByte},
        {5122, PrGltfComponentType_Short},
        {5123, PrGltfComponentType_UnsignedShort},
        {5125, PrGltfComponentType_UnsignedInt},
        {5126, PrGltfComponentType_Float},
    };
    static const GltfName kTypes[] = {
        {"SCALAR", PrGltfElementType_Scalar},
        {"VEC2", PrGltfElementType_Vec2},
        {"VEC3", PrGltfElementType_Vec3},
        {"VEC4", PrGltfElementType_Vec4},
        {"MAT2", PrGltfElementType_Mat2},
        {"MAT3", PrGltfElementType_Mat3},
        {"MAT4", PrGltfElementType_Mat4},
    };
    Gltf* gltf = parser->gltf;
    PrGltfAccessor accessor = {};
    accessor.name = readString(parser, object, "name");
    accessor.bufferView = readIndex(parser, object, "bufferView", gltf->bufferViews.count);
    if (member(parser, object, "componentType") == kPrJsonMissing || member(parser, object, "type") == kPrJsonMissing)
        fail(parser, "", "must have a componentType and a type");
    accessor.componentType = readCode(parser, object, "componentType", Span<const GltfCode>(kComponentTypes),
        PrGltfComponentType_Float);
    accessor.type = readName(parser, object, "type", Span<const GltfName>(kTypes), PrGltfElementType_Scalar);
    accessor.normalized = readBool(parser, object, "normalized", false);
    uint64_t count = 0;
    if (!readInteger(parser, object, "count", &count) || count == 0)
        fail(parser, "count", "must be at least 1");
    else if (count >= kPrGltfNone)
        fail(parser, "count", "is out of range");
    accessor.count = uint32_t(count);
    bool hasOffset = readInteger(parser, object, "byteOffset", &accessor.byteOffset);
    if (failed(parser))
        return;

    if (accessor.normalized && (accessor.componentType == PrGltfComponentType_Float ||
            accessor.componentType == PrGltfComponentType_UnsignedInt))
        fail(parser, "normalized", "must not be true for this componentType");
    uint32_t elementSize = gltfElementSize(accessor.type, accessor.componentType);
    if (accessor.bufferView == kPrGltfNone) {
        if (hasOffset)
            fail(parser, "byteOffset", "must not be defined without a bufferView");
    } else {
        uint32_t stride = gltf->bufferViews.data[accessor.bufferView].byteStride;
        if (stride != 0 && stride < elementSize)
            fail(parser, "bufferView", "has a byteStride less than the size of an element");
        checkElements(parser, "bufferView", accessor.bufferView, accessor.byteOffset, accessor.count,
            stride != 0 ? stride : elementSize, elementSize, gltfComponentSize(accessor.componentType));
    }

    // The bounds are kept as floats, one per component (glTF-2.0/#accessor-bounds).
    accessor.min = readFloatList(parser, object, "min");
    accessor.max = readFloatList(parser, object, "max");
    uint32_t components = gltfComponentCount(accessor.type);
    if ((accessor.min.count != 0 && accessor.min.count != components) ||
            (accessor.max.count != 0 && accessor.max.count != components))
        fail(parser, "min", "and max must have a number for each component");

    readSparse(parser, object, &accessor);
    gltf->accessors.push(accessor);
    gltf->decodedAccessors.push(nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images, samplers and textures

void readImage(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfImage image = {};
    image.name = readString(parser, object, "name");
    image.mimeType = readString(parser, object, "mimeType");
    image.bufferView = readIndex(parser, object, "bufferView", gltf->bufferViews.count);
    PrJsonValueIndex uri = member(parser, object, "uri");
    PrStringView text = prJsonString(parser->json, uri);
    if (failed(parser))
        return;

    // Exactly one of a URI and a buffer view gives the image (glTF-2.0/#images-overview).
    if (uri != kPrJsonMissing && text.data == nullptr) {
        fail(parser, "uri", "must be a string");
    } else if ((uri != kPrJsonMissing) == (image.bufferView != kPrGltfNone)) {
        fail(parser, "", "must have either a uri or a bufferView");
    } else if (image.bufferView != kPrGltfNone) {
        if (image.mimeType.data == nullptr)
            fail(parser, "mimeType", "is required with a bufferView");
        else if (gltf->bufferViews.data[image.bufferView].byteStride != 0)
            fail(parser, "bufferView", "must not have a byteStride");
        else
            image.bytes = gltfBufferViewBytes(gltf, image.bufferView);
    } else if (isDataUri(text)) {
        PrStringView mediaType = {};
        image.uri = gltfCopyString(gltf, "");
        if (!decodeDataUri(gltf, text, &image.bytes, &mediaType))
            fail(parser, "uri", "is a malformed data: URI");
        else if (image.mimeType.data == nullptr && mediaType.size != 0)
            image.mimeType = gltfCopyString(gltf, mediaType);
    } else {
        image.uri = gltfCopyString(gltf, text);
        image.bytes = loadExternal(parser, PrGltfExternal_Image, image.uri);
    }
    if (image.mimeType.data == nullptr && image.bytes.data != nullptr)
        image.mimeType = sniffImage(image.bytes);
    gltf->images.push(image);
}

void readSampler(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfCode kMagFilters[] = {{9728, PrGltfFilter_Nearest}, {9729, PrGltfFilter_Linear}};
    static const GltfCode kMinFilters[] = {
        {9728, PrGltfFilter_Nearest},
        {9729, PrGltfFilter_Linear},
        {9984, PrGltfFilter_NearestMipmapNearest},
        {9985, PrGltfFilter_LinearMipmapNearest},
        {9986, PrGltfFilter_NearestMipmapLinear},
        {9987, PrGltfFilter_LinearMipmapLinear},
    };
    static const GltfCode kWraps[] = {
        {10497, PrGltfWrap_Repeat},
        {33071, PrGltfWrap_ClampToEdge},
        {33648, PrGltfWrap_MirroredRepeat},
    };
    PrGltfSampler sampler = {};
    sampler.name = readString(parser, object, "name");
    sampler.magFilter = readCode(parser, object, "magFilter", Span<const GltfCode>(kMagFilters), PrGltfFilter_None);
    sampler.minFilter = readCode(parser, object, "minFilter", Span<const GltfCode>(kMinFilters), PrGltfFilter_None);
    sampler.wrapS = readCode(parser, object, "wrapS", Span<const GltfCode>(kWraps), PrGltfWrap_Repeat);
    sampler.wrapT = readCode(parser, object, "wrapT", Span<const GltfCode>(kWraps), PrGltfWrap_Repeat);
    parser->gltf->samplers.push(sampler);
}

void readTexture(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfTexture texture = {};
    texture.name = readString(parser, object, "name");
    texture.sampler = readIndex(parser, object, "sampler", gltf->samplers.count);
    texture.source = readIndex(parser, object, "source", gltf->images.count);

    // The extensions that give the texture an image of another format, most compact first, and then the core's.
    texture.sources = PrGltfList{gltf->listIndices.count, 0};
    static const PrStringView kFormats[] = {kBasisu, kWebp};
    for (PrStringView name : kFormats) {
        PrJsonValueIndex other = extension(parser, object, name);
        if (other == kPrJsonMissing)
            continue;
        parser->scope = name.data;
        PrGltfImageIndex image = requireIndex(parser, other, "source", gltf->images.count);
        parser->scope = nullptr;
        if (image == kPrGltfNone)
            break;
        gltf->listIndices.push(image);
        ++texture.sources.count;
    }
    if (texture.source != kPrGltfNone) {
        gltf->listIndices.push(texture.source);
        ++texture.sources.count;
    }
    gltf->textures.push(texture);
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials

PrGltfTextureInfo noTexture() {
    PrGltfTextureInfo info = {};
    info.texture = kPrGltfNone;
    info.transform.scale[0] = 1;
    info.transform.scale[1] = 1;
    return info;
}

// Reads the texture that the member `key` of `object` describes. A normal texture and an occlusion texture hold a
// number of their own, a scale and a strength: `factorKey` names it, and it is written to `*factor` if it is there and
// lies in [low, high].
PrGltfTextureInfo readTextureInfo(GltfParser* parser, PrJsonValueIndex object, const char* key,
        PrStringView factorKey = "", float low = 0, float high = 0, float* PR_NULLABLE factor = nullptr) {
    PrGltfTextureInfo info = noTexture();
    PrJsonValueIndex value = container(parser, object, key, PrJsonKind_Object);
    if (value == kPrJsonMissing)
        return info;
    parser->scope = key;
    info.texture = requireIndex(parser, value, "index", parser->gltf->textures.count);
    uint64_t texCoord = 0;
    readInteger(parser, value, "texCoord", &texCoord);
    if (factor != nullptr)
        *factor = readFloat(parser, value, factorKey, *factor, low, high);

    PrJsonValueIndex transform = extension(parser, value, kTextureTransform);
    if (transform != kPrJsonMissing) {
        info.hasTransform = true;
        readFloats(parser, transform, "offset", info.transform.offset, 2, -kPrF32Infinity, kPrF32Infinity);
        readFloats(parser, transform, "scale", info.transform.scale, 2, -kPrF32Infinity, kPrF32Infinity);
        info.transform.rotation = readFloat(parser, transform, "rotation", 0, -kPrF32Infinity, kPrF32Infinity);
        // The extension's own texCoord replaces the texture's for a reader of the extension.
        readInteger(parser, transform, "texCoord", &texCoord);
    }
    if (texCoord > 0xffffffffu)
        fail(parser, "texCoord", "is out of range");
    info.texCoord = uint32_t(texCoord);
    parser->scope = nullptr;
    return info;
}

// Reads the extensions of Khronos's material model that `object` has into `*material`, and gives those that it does not
// have their defaults.
void readMaterialExtensions(GltfParser* parser, PrJsonValueIndex object, PrGltfMaterial* material) {
    constexpr float kInf = kPrF32Infinity;
    material->ior = 1.5f;
    material->clearcoat = PrGltfClearcoat{
        .texture = noTexture(),
        .roughnessTexture = noTexture(),
        .normalTexture = noTexture(),
        .normalScale = 1,
    };
    material->sheen = PrGltfSheen{.colorTexture = noTexture(), .roughnessTexture = noTexture()};
    material->transmission = PrGltfTransmission{.texture = noTexture()};
    material->volume = PrGltfVolume{
        .thicknessTexture = noTexture(),
        .attenuationDistance = kInf,
        .attenuationColor = {1, 1, 1},
    };
    material->specular =
        PrGltfSpecular{.factor = 1, .texture = noTexture(), .colorFactor = {1, 1, 1}, .colorTexture = noTexture()};
    material->iridescence = PrGltfIridescence{
        .texture = noTexture(),
        .ior = 1.3f,
        .thicknessMinimum = 100,
        .thicknessMaximum = 400,
        .thicknessTexture = noTexture(),
    };
    material->anisotropy = PrGltfAnisotropy{.texture = noTexture()};
    material->diffuseTransmission =
        PrGltfDiffuseTransmission{.texture = noTexture(), .colorFactor = {1, 1, 1}, .colorTexture = noTexture()};
    material->specularGlossiness = PrGltfSpecularGlossiness{
        .diffuseFactor = {1, 1, 1, 1},
        .diffuseTexture = noTexture(),
        .specularFactor = {1, 1, 1},
        .glossinessFactor = 1,
        .specularGlossinessTexture = noTexture(),
    };

    // Each extension's object, which names its members in a diagnostic. A texture's members name the texture instead.
    PrJsonValueIndex at = kPrJsonMissing;
    auto has = [&](PrStringView name, PrGltfMaterialExtensionFlags flag) {
        at = extension(parser, object, name);
        if (at == kPrJsonMissing)
            return false;
        material->extensions |= flag;
        parser->scope = name.data;
        return true;
    };
    auto number = [&](const char* key, float fallback, float low, float high) {
        return readFloat(parser, at, key, fallback, low, high);
    };
    auto texture = [&](const char* key, PrStringView factorKey = "", float* PR_NULLABLE factor = nullptr) {
        const char* scope = parser->scope;
        PrGltfTextureInfo info = readTextureInfo(parser, at, key, factorKey, -kInf, kInf, factor);
        parser->scope = scope;
        return info;
    };

    if (has(kEmissiveStrength, PrGltfMaterialExtensionFlag_EmissiveStrength))
        material->emissiveStrength = number("emissiveStrength", 1, 0, kInf);
    has(kUnlit, PrGltfMaterialExtensionFlag_Unlit);
    if (has(kClearcoat, PrGltfMaterialExtensionFlag_Clearcoat)) {
        PrGltfClearcoat& out = material->clearcoat;
        out.factor = number("clearcoatFactor", 0, 0, 1);
        out.texture = texture("clearcoatTexture");
        out.roughnessFactor = number("clearcoatRoughnessFactor", 0, 0, 1);
        out.roughnessTexture = texture("clearcoatRoughnessTexture");
        out.normalTexture = texture("clearcoatNormalTexture", "scale", &out.normalScale);
    }
    if (has(kSheen, PrGltfMaterialExtensionFlag_Sheen)) {
        PrGltfSheen& out = material->sheen;
        readFloats(parser, at, "sheenColorFactor", out.colorFactor, 3, 0, 1);
        out.colorTexture = texture("sheenColorTexture");
        out.roughnessFactor = number("sheenRoughnessFactor", 0, 0, 1);
        out.roughnessTexture = texture("sheenRoughnessTexture");
    }
    if (has(kTransmission, PrGltfMaterialExtensionFlag_Transmission)) {
        material->transmission.factor = number("transmissionFactor", 0, 0, 1);
        material->transmission.texture = texture("transmissionTexture");
    }
    if (has(kVolume, PrGltfMaterialExtensionFlag_Volume)) {
        PrGltfVolume& out = material->volume;
        out.thicknessFactor = number("thicknessFactor", 0, 0, kInf);
        out.thicknessTexture = texture("thicknessTexture");
        out.attenuationDistance = number("attenuationDistance", kInf, 0, kInf);
        if (out.attenuationDistance <= 0)
            fail(parser, "attenuationDistance", "must be greater than zero");
        readFloats(parser, at, "attenuationColor", out.attenuationColor, 3, 0, 1);
    }
    if (has(kIor, PrGltfMaterialExtensionFlag_Ior)) {
        material->ior = number("ior", 1.5f, 0, kInf);
        if (material->ior != 0 && material->ior < 1)
            fail(parser, "ior", "must be zero or at least 1");
    }
    if (has(kSpecular, PrGltfMaterialExtensionFlag_Specular)) {
        PrGltfSpecular& out = material->specular;
        out.factor = number("specularFactor", 1, 0, 1);
        out.texture = texture("specularTexture");
        readFloats(parser, at, "specularColorFactor", out.colorFactor, 3, 0, kInf);
        out.colorTexture = texture("specularColorTexture");
    }
    if (has(kIridescence, PrGltfMaterialExtensionFlag_Iridescence)) {
        PrGltfIridescence& out = material->iridescence;
        out.factor = number("iridescenceFactor", 0, 0, 1);
        out.texture = texture("iridescenceTexture");
        out.ior = number("iridescenceIor", 1.3f, 1, kInf);
        out.thicknessMinimum = number("iridescenceThicknessMinimum", 100, 0, kInf);
        out.thicknessMaximum = number("iridescenceThicknessMaximum", 400, 0, kInf);
        out.thicknessTexture = texture("iridescenceThicknessTexture");
    }
    if (has(kAnisotropy, PrGltfMaterialExtensionFlag_Anisotropy)) {
        material->anisotropy.strength = number("anisotropyStrength", 0, 0, 1);
        material->anisotropy.rotation = number("anisotropyRotation", 0, -kInf, kInf);
        material->anisotropy.texture = texture("anisotropyTexture");
    }
    if (has(kDispersion, PrGltfMaterialExtensionFlag_Dispersion))
        material->dispersion = number("dispersion", 0, 0, kInf);
    if (has(kDiffuseTransmission, PrGltfMaterialExtensionFlag_DiffuseTransmission)) {
        PrGltfDiffuseTransmission& out = material->diffuseTransmission;
        out.factor = number("diffuseTransmissionFactor", 0, 0, 1);
        out.texture = texture("diffuseTransmissionTexture");
        readFloats(parser, at, "diffuseTransmissionColorFactor", out.colorFactor, 3, 0, 1);
        out.colorTexture = texture("diffuseTransmissionColorTexture");
    }
    if (has(kSpecularGlossiness, PrGltfMaterialExtensionFlag_SpecularGlossiness)) {
        PrGltfSpecularGlossiness& out = material->specularGlossiness;
        readFloats(parser, at, "diffuseFactor", out.diffuseFactor, 4, 0, 1);
        out.diffuseTexture = texture("diffuseTexture");
        readFloats(parser, at, "specularFactor", out.specularFactor, 3, 0, 1);
        out.glossinessFactor = number("glossinessFactor", 1, 0, 1);
        out.specularGlossinessTexture = texture("specularGlossinessTexture");
    }
    parser->scope = nullptr;
}

void readMaterial(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfName kAlphaModes[] = {
        {"OPAQUE", PrGltfAlphaMode_Opaque},
        {"MASK", PrGltfAlphaMode_Mask},
        {"BLEND", PrGltfAlphaMode_Blend},
    };
    PrGltfMaterial material = {};
    material.name = readString(parser, object, "name");
    material.baseColorFactor[0] = material.baseColorFactor[1] = 1;
    material.baseColorFactor[2] = material.baseColorFactor[3] = 1;
    material.metallicFactor = 1;
    material.roughnessFactor = 1;
    material.baseColorTexture = noTexture();
    material.metallicRoughnessTexture = noTexture();
    material.normalScale = 1;
    material.occlusionStrength = 1;
    material.emissiveStrength = 1;

    PrJsonValueIndex pbr = container(parser, object, "pbrMetallicRoughness", PrJsonKind_Object);
    if (pbr != kPrJsonMissing) {
        parser->scope = "pbrMetallicRoughness";
        readFloats(parser, pbr, "baseColorFactor", material.baseColorFactor, 4, 0, 1);
        material.metallicFactor = readFloat(parser, pbr, "metallicFactor", 1, 0, 1);
        material.roughnessFactor = readFloat(parser, pbr, "roughnessFactor", 1, 0, 1);
        parser->scope = nullptr;
        material.baseColorTexture = readTextureInfo(parser, pbr, "baseColorTexture");
        material.metallicRoughnessTexture = readTextureInfo(parser, pbr, "metallicRoughnessTexture");
    }
    material.normalTexture = readTextureInfo(parser, object, "normalTexture", "scale", -kPrF32Infinity, kPrF32Infinity,
        &material.normalScale);
    material.occlusionTexture =
        readTextureInfo(parser, object, "occlusionTexture", "strength", 0, 1, &material.occlusionStrength);
    material.emissiveTexture = readTextureInfo(parser, object, "emissiveTexture");
    readFloats(parser, object, "emissiveFactor", material.emissiveFactor, 3, 0, 1);
    material.alphaMode =
        readName(parser, object, "alphaMode", Span<const GltfName>(kAlphaModes), PrGltfAlphaMode_Opaque);
    material.alphaCutoff = readFloat(parser, object, "alphaCutoff", 0.5f, 0, kPrF32Infinity);
    material.doubleSided = readBool(parser, object, "doubleSided", false);

    readMaterialExtensions(parser, object, &material);
    parser->gltf->materials.push(material);
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

// Reads the set of a numbered attribute from the text after its underscore: decimal digits, at most nine, with no
// leading zero unless the set is zero (glTF-2.0/#meshes-attributes).
bool parseSet(PrStringView text, uint32_t* set) {
    if (text.size == 0 || text.size > 9 || (text.size > 1 && text.data[0] == '0'))
        return false;
    *set = 0;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsDigit(text.data[i]))
            return false;
        *set = *set * 10 + uint32_t(text.data[i] - '0');
    }
    return true;
}

// Reads an attribute's name into its semantic and set. Returns false for a numbered semantic with a malformed set.
bool parseSemantic(PrStringView name, PrGltfSemantic* semantic, uint32_t* set) {
    static const GltfName kPlain[] = {
        {"POSITION", PrGltfSemantic_Position},
        {"NORMAL", PrGltfSemantic_Normal},
        {"TANGENT", PrGltfSemantic_Tangent},
    };
    static const GltfName kNumbered[] = {
        {"TEXCOORD_", PrGltfSemantic_TexCoord},
        {"COLOR_", PrGltfSemantic_Color},
        {"JOINTS_", PrGltfSemantic_Joints},
        {"WEIGHTS_", PrGltfSemantic_Weights},
    };
    *set = 0;
    for (const GltfName& plain : kPlain) {
        if (name == plain.name) {
            *semantic = plain.value;
            return true;
        }
    }
    for (const GltfName& numbered : kNumbered) {
        if (prStringStartsWith(name, numbered.name)) {
            *semantic = numbered.value;
            return parseSet(prStringFrom(name, numbered.name.size), set);
        }
    }
    *semantic = PrGltfSemantic_Custom;
    return true;
}

bool isFloat(const PrGltfAccessor& accessor) {
    return accessor.componentType == PrGltfComponentType_Float;
}

// Returns true if the accessor's components are unsigned normalized integers of 8 or 16 bits.
bool isUnorm(const PrGltfAccessor& accessor) {
    return accessor.normalized && (accessor.componentType == PrGltfComponentType_UnsignedByte ||
        accessor.componentType == PrGltfComponentType_UnsignedShort);
}

// Returns true if the accessor's components are signed normalized integers of 8 or 16 bits.
bool isSnorm(const PrGltfAccessor& accessor) {
    return accessor.normalized &&
        (accessor.componentType == PrGltfComponentType_Byte || accessor.componentType == PrGltfComponentType_Short);
}

// Returns true if the accessor's components are integers of 8 or 16 bits that are not normalized, and are signed if
// `signedOnly`.
bool isSmallInteger(const PrGltfAccessor& accessor, bool signedOnly) {
    if (accessor.normalized)
        return false;
    switch (accessor.componentType) {
        case PrGltfComponentType_Byte:
        case PrGltfComponentType_Short:
            return true;
        case PrGltfComponentType_UnsignedByte:
        case PrGltfComponentType_UnsignedShort:
            return !signedOnly;
        default:
            return false;
    }
}

// Returns true if the accessor has one of the types that KHR_mesh_quantization adds for a semantic, beyond the core's.
bool suitsQuantized(const PrGltfAccessor& accessor, PrGltfSemantic semantic, GltfAttributeOwner owner) {
    bool morphTarget = owner == GltfAttributeOwner_MorphTarget;
    switch (semantic) {
        case PrGltfSemantic_Position:
            // A morph target's displacements are signed.
            return accessor.type == PrGltfElementType_Vec3 &&
                (isSmallInteger(accessor, morphTarget) || isSnorm(accessor) || (!morphTarget && isUnorm(accessor)));
        case PrGltfSemantic_Normal:
            return accessor.type == PrGltfElementType_Vec3 && isSnorm(accessor);
        case PrGltfSemantic_Tangent:
            return accessor.type == (morphTarget ? PrGltfElementType_Vec3 : PrGltfElementType_Vec4) &&
                isSnorm(accessor);
        case PrGltfSemantic_TexCoord:
            return accessor.type == PrGltfElementType_Vec2 && (isSmallInteger(accessor, morphTarget) ||
                (!morphTarget && isSnorm(accessor)));
        default:
            return false;
    }
}

// Returns true if the accessor has the element type and component type that the specification gives a semantic, for a
// primitive's attribute (glTF-2.0/#meshes-attributes) or a morph target's (glTF-2.0/#morph-targets-storage), or, if the
// asset uses KHR_mesh_quantization, one that the extension adds.
bool suitsSemantic(const PrGltfAccessor& accessor, PrGltfSemantic semantic, GltfAttributeOwner owner, bool quantized) {
    bool morphTarget = owner == GltfAttributeOwner_MorphTarget;
    if (quantized && suitsQuantized(accessor, semantic, owner))
        return true;

    switch (semantic) {
        case PrGltfSemantic_Position:
        case PrGltfSemantic_Normal:
            return accessor.type == PrGltfElementType_Vec3 && isFloat(accessor);
        case PrGltfSemantic_Tangent:
            return accessor.type == (morphTarget ? PrGltfElementType_Vec3 : PrGltfElementType_Vec4) &&
                isFloat(accessor);
        case PrGltfSemantic_TexCoord:
            return accessor.type == PrGltfElementType_Vec2 &&
                (isFloat(accessor) || isUnorm(accessor) || (morphTarget && isSnorm(accessor)));
        case PrGltfSemantic_Color:
            return (accessor.type == PrGltfElementType_Vec3 || accessor.type == PrGltfElementType_Vec4) &&
                (isFloat(accessor) || isUnorm(accessor) || (morphTarget && isSnorm(accessor)));
        case PrGltfSemantic_Joints:
            return !morphTarget && accessor.type == PrGltfElementType_Vec4 && !accessor.normalized &&
                (accessor.componentType == PrGltfComponentType_UnsignedByte ||
                    accessor.componentType == PrGltfComponentType_UnsignedShort);
        case PrGltfSemantic_Weights:
            return !morphTarget && accessor.type == PrGltfElementType_Vec4 && (isFloat(accessor) || isUnorm(accessor));
        default:
            return true;
    }
}

// Reads the attributes of a primitive or of a morph target, the members of `object`, onto the end of the document's
// attributes, and counts them by semantic in `*sets`. Each must have `vertexCount` elements, or with a count of zero,
// as many as the first. Returns the number of attributes.
uint32_t readAttributes(GltfParser* parser, PrJsonValueIndex object, PrStringView property, GltfAttributeOwner owner,
        uint32_t* vertexCount, GltfAttributeSets* sets) {
    Gltf* gltf = parser->gltf;
    uint32_t count = 0;
    *sets = {};
    for (PrJsonValueIndex i = prJsonFirst(parser->json, object); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, object, i)) {
        PrGltfAttribute attribute = {};
        PrStringView name = prJsonString(parser->json, i);
        attribute.accessor = indexAt(parser, i + 1, property, gltf->accessors.count);
        if (failed(parser))
            return count;
        if (!parseSemantic(name, &attribute.semantic, &attribute.set)) {
            fail(parser, property, "has a numbered semantic with a malformed number");
            return count;
        }
        const PrGltfAccessor& accessor = gltf->accessors.data[attribute.accessor];
        if (!suitsSemantic(accessor, attribute.semantic, owner, parser->quantized)) {
            fail(parser, property, "has an accessor of a type that its semantic does not allow");
            return count;
        }
        if (*vertexCount == 0)
            *vertexCount = accessor.count;
        if (accessor.count != *vertexCount) {
            fail(parser, property, "has accessors of different counts");
            return count;
        }
        // Positions state their bounds (glTF-2.0/#meshes-attributes).
        if (attribute.semantic == PrGltfSemantic_Position && (accessor.min.count == 0 || accessor.max.count == 0)) {
            fail(parser, property, "has a POSITION accessor without min and max");
            return count;
        }
        if (attribute.semantic != PrGltfSemantic_Custom) {
            uint32_t& greatest = sets->greatest[attribute.semantic];
            ++sets->count[attribute.semantic];
            greatest = attribute.set > greatest ? attribute.set : greatest;
        }
        attribute.name = gltfCopyString(gltf, name);
        gltf->attributes.push(attribute);
        ++count;
    }
    return count;
}

// Returns true if `count` vertices can be assembled in `mode` (glTF-2.0/#meshes-topology).
bool suitsMode(PrGltfMode mode, uint32_t count) {
    switch (mode) {
        case PrGltfMode_Lines:
            return count % 2 == 0;
        case PrGltfMode_LineLoop:
        case PrGltfMode_LineStrip:
            return count >= 2;
        case PrGltfMode_Triangles:
            return count % 3 == 0;
        case PrGltfMode_TriangleStrip:
        case PrGltfMode_TriangleFan:
            return count >= 3;
        default:
            return true;
    }
}

// Reads the materials that a primitive takes in the asset's variants. A variant is in one mapping of the primitive at
// most.
void readVariantMappings(GltfParser* parser, PrJsonValueIndex object, PrGltfPrimitive* primitive) {
    Gltf* gltf = parser->gltf;
    primitive->mappingFirst = gltf->variantMappings.count;
    PrJsonValueIndex variants = extension(parser, object, kVariants);
    if (variants == kPrJsonMissing)
        return;
    parser->scope = kVariants.data;
    PrJsonValueIndex mappings = container(parser, variants, "mappings", PrJsonKind_Array);
    if (mappings == kPrJsonMissing)
        fail(parser, "mappings", "is required");
    if (failed(parser))
        return;
    // Every mapping's variants are marked with one stamp, so that a variant that two mappings name is found.
    beginUnique(parser, gltf->variants.count);
    for (PrJsonValueIndex i = prJsonFirst(parser->json, mappings); i != kPrJsonMissing && !failed(parser);
            i = prJsonAfter(parser->json, mappings, i)) {
        if (prJsonKind(parser->json, i) != PrJsonKind_Object) {
            fail(parser, "mappings", "must hold objects");
            break;
        }
        PrGltfVariantMapping mapping = {};
        mapping.material = requireIndex(parser, i, "material", gltf->materials.count);
        mapping.name = readString(parser, i, "name");
        mapping.variants = PrGltfList{gltf->listIndices.count, 0};
        PrJsonValueIndex list = container(parser, i, "variants", PrJsonKind_Array);
        if (list == kPrJsonMissing || prJsonCount(parser->json, list) == 0)
            fail(parser, "variants", "must name at least one variant");
        PrJsonValueIndex first = list != kPrJsonMissing ? prJsonFirst(parser->json, list) : kPrJsonMissing;
        for (PrJsonValueIndex v = first; v != kPrJsonMissing && !failed(parser); v = prJsonAfter(parser->json, list, v)) {
            uint32_t variant = indexAt(parser, v, "variants", gltf->variants.count);
            if (variant == kPrGltfNone)
                break;
            if (!addUnique(parser, variant)) {
                fail(parser, "variants", "names a variant that another mapping of the primitive names");
                break;
            }
            gltf->listIndices.push(variant);
            ++mapping.variants.count;
        }
        gltf->variantMappings.push(mapping);
        ++primitive->mappingCount;
    }
    parser->scope = nullptr;
}

// Reads a primitive's Draco stream, checks it against the rules of KHR_draco_mesh_compression, and fills the accessors
// of the indices and of the attributes that it names from it, if its buffer has bytes.
void readDraco(GltfParser* parser, PrJsonValueIndex object, PrGltfPrimitive* primitive) {
    Gltf* gltf = parser->gltf;
    primitive->draco = kPrGltfNone;
    PrJsonValueIndex draco = extension(parser, object, kDraco);
    if (draco == kPrJsonMissing)
        return;
    parser->scope = kDraco.data;
    primitive->draco = requireIndex(parser, draco, "bufferView", gltf->bufferViews.count);
    PrJsonValueIndex attributes = container(parser, draco, "attributes", PrJsonKind_Object);
    if (attributes == kPrJsonMissing || prJsonCount(parser->json, attributes) == 0)
        fail(parser, "attributes", "must hold at least one attribute");
    // The extension codes triangles and strips of them. It does not say how the triangles that a stream decodes to
    // become the indices of a strip, so a strip is not read.
    if (primitive->mode == PrGltfMode_TriangleStrip)
        failWith(parser, PrStatus_NotSupported, "", "is of a triangle strip, which the module does not read");
    else if (primitive->mode != PrGltfMode_Triangles)
        fail(parser, "", "is of a primitive whose mode is not triangles");
    // A stream decodes to indexed triangles, which are the primitive's only if it has indices.
    else if (primitive->indices == kPrGltfNone)
        fail(parser, "", "is of a primitive that has no indices");
    else if (!failed(parser) && gltf->bufferViews.data[primitive->draco].byteStride != 0)
        fail(parser, "bufferView", "must not have a byteStride");
    if (failed(parser)) {
        parser->scope = nullptr;
        return;
    }

    // Each attribute of the stream is an attribute of the primitive, by its name.
    RawArray<GltfDracoAttribute> filled;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, attributes); i != kPrJsonMissing && !failed(parser);
            i = prJsonAfter(parser->json, attributes, i)) {
        PrStringView name = prJsonString(parser->json, i);
        uint64_t id = 0;
        if (!integerAt(parser, i + 1, "attributes", &id) || id >= kPrGltfNone) {
            fail(parser, "attributes", "must hold the ids of the stream's attributes");
            break;
        }
        PrGltfAccessorIndex accessor = kPrGltfNone;
        for (uint32_t a = 0; a < primitive->attributeCount; ++a) {
            const PrGltfAttribute& attribute = gltf->attributes.data[primitive->attributeFirst + a];
            if (attribute.name == name)
                accessor = attribute.accessor;
        }
        if (accessor == kPrGltfNone) {
            fail(parser, "attributes", "names an attribute that the primitive does not have");
            break;
        }
        gltf->accessors.data[accessor].draco = true;
        filled.push(GltfDracoAttribute{accessor, uint32_t(id)});
    }
    PrByteView stream = failed(parser) ? PrByteView{nullptr, 0} : gltfBufferViewBytes(gltf, primitive->draco);
    if (!failed(parser))
        gltf->accessors.data[primitive->indices].draco = true;
    // The accessors have the stream's elements, and none while the stream's buffer has no bytes.
    if (stream.data != nullptr) {
        PrStringView message = {};
        PrStatus status = gltfDracoDecode(gltf, stream, primitive->indices, primitive->vertexCount,
            Span<const GltfDracoAttribute>(filled.data, filled.count), &message);
        if (status != PrStatus_Ok)
            failWith(parser, status, "bufferView", message);
    }
    filled.reset();
    parser->scope = nullptr;
}

void readPrimitive(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfCode kModes[] = {
        {0, PrGltfMode_Points},
        {1, PrGltfMode_Lines},
        {2, PrGltfMode_LineLoop},
        {3, PrGltfMode_LineStrip},
        {4, PrGltfMode_Triangles},
        {5, PrGltfMode_TriangleStrip},
        {6, PrGltfMode_TriangleFan},
    };
    Gltf* gltf = parser->gltf;
    PrGltfPrimitive primitive = {};
    primitive.indices = readIndex(parser, object, "indices", gltf->accessors.count);
    primitive.material = readIndex(parser, object, "material", gltf->materials.count);
    primitive.mode = readCode(parser, object, "mode", Span<const GltfCode>(kModes), PrGltfMode_Triangles);
    PrJsonValueIndex attributes = container(parser, object, "attributes", PrJsonKind_Object);
    if (attributes == kPrJsonMissing || prJsonCount(parser->json, attributes) == 0)
        fail(parser, "attributes", "must hold at least one attribute");
    if (failed(parser))
        return;

    GltfAttributeSets sets;
    primitive.attributeFirst = gltf->attributes.count;
    primitive.attributeCount = readAttributes(parser, attributes, "attributes", GltfAttributeOwner_Primitive,
        &primitive.vertexCount, &sets);
    if (failed(parser))
        return;
    // The sets of a semantic are numbered from zero without gaps, so the greatest is one less than their number. The
    // same holds of a semantic that takes no set, of which there is one attribute at most.
    for (uint32_t semantic = 0; semantic < PrGltfSemantic_Custom; ++semantic) {
        if (sets.count[semantic] != 0 && sets.greatest[semantic] + 1 != sets.count[semantic]) {
            fail(parser, "attributes", "repeats a semantic or numbers its sets with a gap");
            return;
        }
    }
    // Each set of joints has a set of weights (glTF-2.0/#skinned-mesh-primitive-attributes).
    if (sets.count[PrGltfSemantic_Joints] != sets.count[PrGltfSemantic_Weights]) {
        fail(parser, "attributes", "has joints without weights or weights without joints");
        return;
    }

    uint32_t assembled = primitive.vertexCount;
    if (primitive.indices != kPrGltfNone) {
        const PrGltfAccessor& indices = gltf->accessors.data[primitive.indices];
        bool unsignedInteger = indices.componentType == PrGltfComponentType_UnsignedByte ||
            indices.componentType == PrGltfComponentType_UnsignedShort ||
            indices.componentType == PrGltfComponentType_UnsignedInt;
        if (indices.type != PrGltfElementType_Scalar || !unsignedInteger || indices.normalized) {
            fail(parser, "indices", "must be a SCALAR accessor of unsigned integers");
            return;
        }
        if (indices.bufferView != kPrGltfNone && gltf->bufferViews.data[indices.bufferView].byteStride != 0) {
            fail(parser, "indices", "reads a buffer view that has a byteStride");
            return;
        }
        assembled = indices.count;
    }
    if (!suitsMode(primitive.mode, assembled)) {
        fail(parser, "mode", "cannot assemble the primitive's number of vertices");
        return;
    }

    PrJsonValueIndex targets = container(parser, object, "targets", PrJsonKind_Array);
    primitive.morphTargetFirst = gltf->morphTargets.count;
    if (targets != kPrJsonMissing) {
        for (PrJsonValueIndex i = prJsonFirst(parser->json, targets); i != kPrJsonMissing;
                i = prJsonAfter(parser->json, targets, i)) {
            if (prJsonKind(parser->json, i) != PrJsonKind_Object) {
                fail(parser, "targets", "must hold objects");
                return;
            }
            PrGltfMorphTarget target = {};
            GltfAttributeSets displaced;
            target.attributeFirst = gltf->attributes.count;
            target.attributeCount = readAttributes(parser, i, "targets", GltfAttributeOwner_MorphTarget,
                &primitive.vertexCount, &displaced);
            if (failed(parser))
                return;
            // A morph target displaces only attributes that the primitive has (glTF-2.0/#morph-targets-overview). The
            // primitive's sets of a semantic are numbered without gaps, so it has every set below their number.
            for (uint32_t semantic = 0; semantic < PrGltfSemantic_Custom; ++semantic) {
                if (displaced.count[semantic] != 0 && displaced.greatest[semantic] >= sets.count[semantic]) {
                    fail(parser, "targets", "displaces an attribute that the primitive does not have");
                    return;
                }
            }
            gltf->morphTargets.push(target);
            ++primitive.morphTargetCount;
        }
    }
    readVariantMappings(parser, object, &primitive);
    readDraco(parser, object, &primitive);
    parser->jointSets.push(sets.count[PrGltfSemantic_Joints]);
    gltf->primitives.push(primitive);
}

void readMesh(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfMesh mesh = {};
    mesh.name = readString(parser, object, "name");
    mesh.weights = readFloatList(parser, object, "weights");
    mesh.primitiveFirst = gltf->primitives.count;
    PrJsonValueIndex primitives = container(parser, object, "primitives", PrJsonKind_Array);
    if (primitives == kPrJsonMissing || prJsonCount(parser->json, primitives) == 0)
        fail(parser, "primitives", "must hold at least one primitive");
    if (failed(parser))
        return;

    // The mesh has the number of morph targets of its first primitive that has any, and every primitive that has morph
    // targets has that number (glTF-2.0/#morph-targets-overview).
    uint32_t morphCount = 0;
    parser->part = "primitives";
    parser->partIndex = 0;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, primitives); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, primitives, i)) {
        if (prJsonKind(parser->json, i) != PrJsonKind_Object)
            fail(parser, "", "must be an object");
        else
            readPrimitive(parser, i);
        if (failed(parser))
            return;
        uint32_t count = gltf->primitives.last().morphTargetCount;
        if (morphCount == 0)
            morphCount = count;
        if (count != 0 && count != morphCount) {
            fail(parser, "targets", "has another number of morph targets than an earlier primitive");
            return;
        }
        ++mesh.primitiveCount;
        ++parser->partIndex;
    }
    parser->part = nullptr;
    if (mesh.weights.count != 0 && mesh.weights.count != morphCount)
        fail(parser, "weights", "must have a weight for each morph target");
    parser->morphCounts.push(morphCount);
    gltf->meshes.push(mesh);
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

void readCamera(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfName kTypes[] = {
        {"perspective", PrGltfCameraType_Perspective},
        {"orthographic", PrGltfCameraType_Orthographic},
    };
    PrGltfCamera camera = {};
    camera.name = readString(parser, object, "name");
    if (member(parser, object, "type") == kPrJsonMissing)
        fail(parser, "type", "is required");
    camera.type = readName(parser, object, "type", Span<const GltfName>(kTypes), PrGltfCameraType_Perspective);
    if (failed(parser))
        return;

    // A required number that is missing reads as -1, which no range allows.
    if (camera.type == PrGltfCameraType_Perspective) {
        PrJsonValueIndex perspective = container(parser, object, "perspective", PrJsonKind_Object);
        if (perspective == kPrJsonMissing) {
            fail(parser, "perspective", "is required");
            return;
        }
        parser->scope = "perspective";
        camera.yfov = readFloat(parser, perspective, "yfov", -1, 0, kPrF32Infinity);
        camera.znear = readFloat(parser, perspective, "znear", -1, 0, kPrF32Infinity);
        camera.aspectRatio = readFloat(parser, perspective, "aspectRatio", 0, 0, kPrF32Infinity);
        camera.zfar = readFloat(parser, perspective, "zfar", kPrF32Infinity, 0, kPrF32Infinity);
        bool hasAspect = member(parser, perspective, "aspectRatio") != kPrJsonMissing;
        if (camera.yfov <= 0 || camera.znear <= 0 || (hasAspect && camera.aspectRatio <= 0))
            fail(parser, "", "must have a yfov, a znear and any aspectRatio greater than zero");
        else if (camera.zfar <= camera.znear)
            fail(parser, "zfar", "must be greater than znear");
    } else {
        PrJsonValueIndex orthographic = container(parser, object, "orthographic", PrJsonKind_Object);
        if (orthographic == kPrJsonMissing) {
            fail(parser, "orthographic", "is required");
            return;
        }
        parser->scope = "orthographic";
        camera.xmag = readFloat(parser, orthographic, "xmag", 0, -kPrF32Infinity, kPrF32Infinity);
        camera.ymag = readFloat(parser, orthographic, "ymag", 0, -kPrF32Infinity, kPrF32Infinity);
        camera.znear = readFloat(parser, orthographic, "znear", -1, 0, kPrF32Infinity);
        camera.zfar = readFloat(parser, orthographic, "zfar", -1, 0, kPrF32Infinity);
        if (camera.xmag == 0 || camera.ymag == 0)
            fail(parser, "", "must have an xmag and a ymag other than zero");
        else if (camera.znear < 0 || camera.zfar <= camera.znear)
            fail(parser, "", "must have a znear of zero or more and a zfar greater than it");
    }
    parser->scope = nullptr;
    parser->gltf->cameras.push(camera);
}

void readLight(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfName kTypes[] = {
        {"directional", PrGltfLightType_Directional},
        {"point", PrGltfLightType_Point},
        {"spot", PrGltfLightType_Spot},
    };
    constexpr float kQuarterTurn = 1.5707964f;
    PrGltfLight light = {};
    light.name = readString(parser, object, "name");
    if (member(parser, object, "type") == kPrJsonMissing)
        fail(parser, "type", "is required");
    light.type = readName(parser, object, "type", Span<const GltfName>(kTypes), PrGltfLightType_Point);
    light.color[0] = light.color[1] = light.color[2] = 1;
    readFloats(parser, object, "color", light.color, 3, 0, kPrF32Infinity);
    light.intensity = readFloat(parser, object, "intensity", 1, 0, kPrF32Infinity);
    light.range = readFloat(parser, object, "range", kPrF32Infinity, 0, kPrF32Infinity);
    if (light.range <= 0)
        fail(parser, "range", "must be greater than zero");
    light.outerConeAngle = kQuarterTurn / 2;
    PrJsonValueIndex spot = container(parser, object, "spot", PrJsonKind_Object);
    if (light.type == PrGltfLightType_Spot && spot == kPrJsonMissing) {
        fail(parser, "spot", "is required for a spot light");
    } else if (light.type == PrGltfLightType_Spot) {
        parser->scope = "spot";
        light.innerConeAngle = readFloat(parser, spot, "innerConeAngle", 0, 0, kQuarterTurn);
        light.outerConeAngle = readFloat(parser, spot, "outerConeAngle", kQuarterTurn / 2, 0, kQuarterTurn);
        if (light.innerConeAngle >= light.outerConeAngle)
            fail(parser, "innerConeAngle", "must be less than outerConeAngle");
        parser->scope = nullptr;
    }
    parser->gltf->lights.push(light);
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes, skins and scenes

// Reads the instances of a node's mesh (EXT_mesh_gpu_instancing). Their transforms have the types of the extension's
// table, and every attribute has one element for each instance.
void readInstancing(GltfParser* parser, PrJsonValueIndex object, PrGltfNode* node) {
    Gltf* gltf = parser->gltf;
    PrGltfInstancing& out = node->instancing;
    out = PrGltfInstancing{
        .translation = kPrGltfNone,
        .rotation = kPrGltfNone,
        .scale = kPrGltfNone,
        .attributeFirst = gltf->attributes.count,
    };
    PrJsonValueIndex instancing = extension(parser, object, kInstancing);
    if (instancing == kPrJsonMissing || failed(parser))
        return;
    parser->scope = kInstancing.data;
    PrJsonValueIndex attributes = container(parser, instancing, "attributes", PrJsonKind_Object);
    if (attributes == kPrJsonMissing || prJsonCount(parser->json, attributes) == 0)
        fail(parser, "attributes", "must hold at least one attribute");
    else if (node->mesh == kPrGltfNone)
        fail(parser, "", "needs the node to have a mesh");
    for (PrJsonValueIndex i = failed(parser) ? kPrJsonMissing : prJsonFirst(parser->json, attributes);
            i != kPrJsonMissing; i = prJsonAfter(parser->json, attributes, i)) {
        PrStringView name = prJsonString(parser->json, i);
        PrGltfAccessorIndex index = indexAt(parser, i + 1, "attributes", gltf->accessors.count);
        if (failed(parser))
            break;
        const PrGltfAccessor& accessor = gltf->accessors.data[index];
        bool suits = true;
        if (name == "TRANSLATION" || name == "SCALE") {
            suits = accessor.type == PrGltfElementType_Vec3 && isFloat(accessor);
            (name == "SCALE" ? out.scale : out.translation) = index;
        } else if (name == "ROTATION") {
            bool snorm = isSnorm(accessor);
            suits = accessor.type == PrGltfElementType_Vec4 && (isFloat(accessor) || snorm);
            out.rotation = index;
        } else {
            gltf->attributes.push(PrGltfAttribute{
                .name = gltfCopyString(gltf, name),
                .semantic = PrGltfSemantic_Custom,
                .set = 0,
                .accessor = index,
            });
            ++out.attributeCount;
        }
        if (!suits) {
            fail(parser, "attributes", "has an accessor of a type that its attribute does not allow");
            break;
        }
        if (out.count == 0)
            out.count = accessor.count;
        if (accessor.count != out.count) {
            fail(parser, "attributes", "has accessors of different counts");
            break;
        }
    }
    parser->scope = nullptr;
}

void readNode(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfNode node = {};
    node.name = readString(parser, object, "name");
    node.parent = kPrGltfNone;
    node.children = readIndexList(parser, object, "children", parser->nodeCount);
    node.mesh = readIndex(parser, object, "mesh", gltf->meshes.count);
    node.skin = readIndex(parser, object, "skin", parser->skinCount);
    node.camera = readIndex(parser, object, "camera", gltf->cameras.count);
    node.light = kPrGltfNone;
    PrJsonValueIndex light = extension(parser, object, kLightsPunctual);
    if (light != kPrJsonMissing)
        node.light = requireIndex(parser, light, "light", gltf->lights.count);
    node.weights = readFloatList(parser, object, "weights");

    node.matrix = prMatrix4Identity();
    node.rotation[3] = 1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1;
    float matrix[16];
    node.hasMatrix = readFloats(parser, object, "matrix", matrix, 16, -kPrF32Infinity, kPrF32Infinity);
    bool hasTranslation =
        readFloats(parser, object, "translation", node.translation, 3, -kPrF32Infinity, kPrF32Infinity);
    bool hasRotation = readFloats(parser, object, "rotation", node.rotation, 4, -1, 1);
    bool hasScale = readFloats(parser, object, "scale", node.scale, 3, -kPrF32Infinity, kPrF32Infinity);
    if (failed(parser))
        return;

    if (node.hasMatrix) {
        if (hasTranslation || hasRotation || hasScale) {
            fail(parser, "matrix", "must not be defined with a translation, a rotation or a scale");
            return;
        }
        // The asset's matrix is in column-major order.
        for (uint32_t column = 0; column < 4; ++column) {
            for (uint32_t row = 0; row < 4; ++row)
                node.matrix.m[row][column] = matrix[column * 4 + row];
        }
    }
    if (node.skin != kPrGltfNone && node.mesh == kPrGltfNone) {
        fail(parser, "skin", "needs the node to have a mesh");
        return;
    }
    if (node.weights.count != 0 &&
            (node.mesh == kPrGltfNone || node.weights.count != parser->morphCounts.data[node.mesh])) {
        fail(parser, "weights", "must have a weight for each morph target of the node's mesh");
        return;
    }

    node.visible = true;
    PrJsonValueIndex visibility = extension(parser, object, kNodeVisibility);
    if (visibility != kPrJsonMissing) {
        parser->scope = kNodeVisibility.data;
        node.visible = readBool(parser, visibility, "visible", true);
        parser->scope = nullptr;
    }
    readInstancing(parser, object, &node);
    gltf->nodes.push(node);
}

// Sets each node's parent from its parent's list of children, and checks that the nodes form trees
// (glTF-2.0/#nodes-and-hierarchy).
void linkNodes(GltfParser* parser) {
    Gltf* gltf = parser->gltf;
    if (failed(parser))
        return;
    for (uint32_t i = 0; i < gltf->nodes.count; ++i) {
        PrGltfList children = gltf->nodes.data[i].children;
        for (uint32_t c = 0; c < children.count; ++c) {
            PrGltfNode& child = gltf->nodes.data[gltf->listIndices.data[children.first + c]];
            if (child.parent != kPrGltfNone) {
                parser->kind = "nodes";
                parser->index = i;
                fail(parser, "children", "names a node that already has a parent");
                parser->kind = nullptr;
                return;
            }
            child.parent = i;
        }
    }

    // Every node has at most one parent, so a node that no root reaches is on a cycle.
    RawArray<uint32_t> stack;
    uint32_t reached = 0;
    for (uint32_t i = 0; i < gltf->nodes.count; ++i) {
        if (gltf->nodes.data[i].parent == kPrGltfNone)
            stack.push(i);
    }
    while (stack.count != 0) {
        uint32_t index = stack.last();
        stack.pop();
        ++reached;
        PrGltfList children = gltf->nodes.data[index].children;
        stack.append(gltf->listIndices.data + children.first, children.count);
    }
    stack.reset();
    if (reached != gltf->nodes.count)
        fail(parser, "nodes", "has a node that is its own ancestor");
}

void readSkin(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfSkin skin = {};
    skin.name = readString(parser, object, "name");
    skin.joints = readIndexList(parser, object, "joints", gltf->nodes.count);
    skin.inverseBindMatrices = readIndex(parser, object, "inverseBindMatrices", gltf->accessors.count);
    skin.skeleton = readIndex(parser, object, "skeleton", gltf->nodes.count);
    if (failed(parser))
        return;
    if (skin.joints.count == 0) {
        fail(parser, "joints", "must name at least one node");
        return;
    }
    if (skin.inverseBindMatrices != kPrGltfNone) {
        const PrGltfAccessor& accessor = gltf->accessors.data[skin.inverseBindMatrices];
        if (accessor.type != PrGltfElementType_Mat4 || !isFloat(accessor) || accessor.count < skin.joints.count) {
            fail(parser, "inverseBindMatrices", "must be a MAT4 accessor of floats with a matrix for each joint");
            return;
        }
    }
    gltf->skins.push(skin);
}

void readScene(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfScene scene = {};
    scene.name = readString(parser, object, "name");
    scene.nodes = readIndexList(parser, object, "nodes", gltf->nodes.count);
    if (failed(parser))
        return;
    for (uint32_t i = 0; i < scene.nodes.count; ++i) {
        if (gltf->nodes.data[gltf->listIndices.data[scene.nodes.first + i]].parent != kPrGltfNone) {
            fail(parser, "nodes", "names a node that is not a root");
            return;
        }
    }
    gltf->scenes.push(scene);
}

// ---------------------------------------------------------------------------------------------------------------------
// Animations

void readAnimationSampler(GltfParser* parser, PrJsonValueIndex object) {
    static const GltfName kInterpolations[] = {
        {"LINEAR", PrGltfInterpolation_Linear},
        {"STEP", PrGltfInterpolation_Step},
        {"CUBICSPLINE", PrGltfInterpolation_CubicSpline},
    };
    Gltf* gltf = parser->gltf;
    PrGltfAnimationSampler sampler = {};
    sampler.input = requireIndex(parser, object, "input", gltf->accessors.count);
    sampler.output = requireIndex(parser, object, "output", gltf->accessors.count);
    sampler.interpolation = readName(parser, object, "interpolation", Span<const GltfName>(kInterpolations),
        PrGltfInterpolation_Linear);
    if (failed(parser))
        return;
    // The times are float scalars that state their bounds (glTF-2.0/#animations-samplers-input).
    const PrGltfAccessor& input = gltf->accessors.data[sampler.input];
    if (input.type != PrGltfElementType_Scalar || !isFloat(input)) {
        fail(parser, "input", "must be a SCALAR accessor of floats");
        return;
    }
    if (input.min.count == 0 || input.max.count == 0) {
        fail(parser, "input", "must be an accessor with min and max");
        return;
    }
    if (sampler.interpolation == PrGltfInterpolation_CubicSpline && input.count < 2) {
        fail(parser, "input", "must have at least two keyframes for a cubic spline");
        return;
    }
    gltf->animationSamplers.push(sampler);
}

// The kinds of object that an animation's pointer names a property of (KHR_animation_pointer).
enum {
    GltfPointerKind_Node,
    GltfPointerKind_Mesh,
    GltfPointerKind_Camera,
    GltfPointerKind_Light,
    GltfPointerKind_Material,
};
typedef uint8_t GltfPointerKind;

// A property that a pointer names, by what follows the index of its object, and the number of floats in its value. A
// width of zero is that of the weights of the object's morph targets. A material's property is defined only if the
// material has the extension that gives it.
struct GltfPointerProperty {
    PrStringView suffix;
    PrGltfPath path;
    uint8_t width;
    PrGltfMaterialExtensionFlags extension;
};

constexpr GltfPointerProperty kNodeProperties[] = {
    {"/translation", PrGltfPath_Translation, 3, 0},
    {"/rotation", PrGltfPath_Rotation, 4, 0},
    {"/scale", PrGltfPath_Scale, 3, 0},
    {"/weights", PrGltfPath_Weights, 0, 0},
    {"/extensions/KHR_node_visibility/visible", PrGltfPath_NodeVisible, 1, 0},
};

constexpr GltfPointerProperty kMeshWeights = {"/weights", PrGltfPath_MeshWeights, 0, 0};

constexpr GltfPointerProperty kCameraProperties[] = {
    {"/perspective/aspectRatio", PrGltfPath_CameraAspectRatio, 1, 0},
    {"/perspective/yfov", PrGltfPath_CameraYfov, 1, 0},
    {"/perspective/znear", PrGltfPath_CameraZnear, 1, 0},
    {"/perspective/zfar", PrGltfPath_CameraZfar, 1, 0},
    {"/orthographic/xmag", PrGltfPath_CameraXmag, 1, 0},
    {"/orthographic/ymag", PrGltfPath_CameraYmag, 1, 0},
    {"/orthographic/znear", PrGltfPath_CameraZnear, 1, 0},
    {"/orthographic/zfar", PrGltfPath_CameraZfar, 1, 0},
};

constexpr GltfPointerProperty kLightProperties[] = {
    {"/color", PrGltfPath_LightColor, 3, 0},
    {"/intensity", PrGltfPath_LightIntensity, 1, 0},
    {"/range", PrGltfPath_LightRange, 1, 0},
    {"/spot/innerConeAngle", PrGltfPath_LightInnerConeAngle, 1, 0},
    {"/spot/outerConeAngle", PrGltfPath_LightOuterConeAngle, 1, 0},
};

constexpr GltfPointerProperty kMaterialProperties[] = {
    {"/pbrMetallicRoughness/baseColorFactor", PrGltfPath_MaterialBaseColorFactor, 4, 0},
    {"/pbrMetallicRoughness/metallicFactor", PrGltfPath_MaterialMetallicFactor, 1, 0},
    {"/pbrMetallicRoughness/roughnessFactor", PrGltfPath_MaterialRoughnessFactor, 1, 0},
    {"/alphaCutoff", PrGltfPath_MaterialAlphaCutoff, 1, 0},
    {"/emissiveFactor", PrGltfPath_MaterialEmissiveFactor, 3, 0},
    {"/normalTexture/scale", PrGltfPath_MaterialNormalScale, 1, 0},
    {"/occlusionTexture/strength", PrGltfPath_MaterialOcclusionStrength, 1, 0},
    {
        "/extensions/KHR_materials_emissive_strength/emissiveStrength", PrGltfPath_MaterialEmissiveStrength, 1,
        PrGltfMaterialExtensionFlag_EmissiveStrength,
    },
    {
        "/extensions/KHR_materials_clearcoat/clearcoatFactor", PrGltfPath_MaterialClearcoatFactor, 1,
        PrGltfMaterialExtensionFlag_Clearcoat,
    },
    {
        "/extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", PrGltfPath_MaterialClearcoatRoughnessFactor, 1,
        PrGltfMaterialExtensionFlag_Clearcoat,
    },
    {
        "/extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale", PrGltfPath_MaterialClearcoatNormalScale, 1,
        PrGltfMaterialExtensionFlag_Clearcoat,
    },
    {
        "/extensions/KHR_materials_sheen/sheenColorFactor", PrGltfPath_MaterialSheenColorFactor, 3,
        PrGltfMaterialExtensionFlag_Sheen,
    },
    {
        "/extensions/KHR_materials_sheen/sheenRoughnessFactor", PrGltfPath_MaterialSheenRoughnessFactor, 1,
        PrGltfMaterialExtensionFlag_Sheen,
    },
    {
        "/extensions/KHR_materials_transmission/transmissionFactor", PrGltfPath_MaterialTransmissionFactor, 1,
        PrGltfMaterialExtensionFlag_Transmission,
    },
    {
        "/extensions/KHR_materials_volume/thicknessFactor", PrGltfPath_MaterialThicknessFactor, 1,
        PrGltfMaterialExtensionFlag_Volume,
    },
    {
        "/extensions/KHR_materials_volume/attenuationDistance", PrGltfPath_MaterialAttenuationDistance, 1,
        PrGltfMaterialExtensionFlag_Volume,
    },
    {
        "/extensions/KHR_materials_volume/attenuationColor", PrGltfPath_MaterialAttenuationColor, 3,
        PrGltfMaterialExtensionFlag_Volume,
    },
    {"/extensions/KHR_materials_ior/ior", PrGltfPath_MaterialIor, 1, PrGltfMaterialExtensionFlag_Ior},
    {
        "/extensions/KHR_materials_specular/specularFactor", PrGltfPath_MaterialSpecularFactor, 1,
        PrGltfMaterialExtensionFlag_Specular,
    },
    {
        "/extensions/KHR_materials_specular/specularColorFactor", PrGltfPath_MaterialSpecularColorFactor, 3,
        PrGltfMaterialExtensionFlag_Specular,
    },
    {
        "/extensions/KHR_materials_iridescence/iridescenceFactor", PrGltfPath_MaterialIridescenceFactor, 1,
        PrGltfMaterialExtensionFlag_Iridescence,
    },
    {
        "/extensions/KHR_materials_iridescence/iridescenceIor", PrGltfPath_MaterialIridescenceIor, 1,
        PrGltfMaterialExtensionFlag_Iridescence,
    },
    {
        "/extensions/KHR_materials_iridescence/iridescenceThicknessMinimum",
        PrGltfPath_MaterialIridescenceThicknessMinimum, 1, PrGltfMaterialExtensionFlag_Iridescence,
    },
    {
        "/extensions/KHR_materials_iridescence/iridescenceThicknessMaximum",
        PrGltfPath_MaterialIridescenceThicknessMaximum, 1, PrGltfMaterialExtensionFlag_Iridescence,
    },
    {
        "/extensions/KHR_materials_anisotropy/anisotropyStrength", PrGltfPath_MaterialAnisotropyStrength, 1,
        PrGltfMaterialExtensionFlag_Anisotropy,
    },
    {
        "/extensions/KHR_materials_anisotropy/anisotropyRotation", PrGltfPath_MaterialAnisotropyRotation, 1,
        PrGltfMaterialExtensionFlag_Anisotropy,
    },
    {
        "/extensions/KHR_materials_dispersion/dispersion", PrGltfPath_MaterialDispersion, 1,
        PrGltfMaterialExtensionFlag_Dispersion,
    },
    {
        "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
        PrGltfPath_MaterialDiffuseTransmissionFactor, 1, PrGltfMaterialExtensionFlag_DiffuseTransmission,
    },
    {
        "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",
        PrGltfPath_MaterialDiffuseTransmissionColorFactor, 3, PrGltfMaterialExtensionFlag_DiffuseTransmission,
    },
};

// The properties of a texture's transform, after the pointer to the texture.
constexpr GltfPointerProperty kTransformProperties[] = {
    {"/extensions/KHR_texture_transform/offset", PrGltfPath_TextureOffset, 2, 0},
    {"/extensions/KHR_texture_transform/rotation", PrGltfPath_TextureRotation, 1, 0},
    {"/extensions/KHR_texture_transform/scale", PrGltfPath_TextureScale, 2, 0},
};

// Reads the index of an array's element at the start of `*text`, the token after a slash: decimal digits without a
// leading zero (RFC 6901). Moves `*text` past it. Returns false if there is none.
bool pointerIndex(PrStringView* text, uint32_t* index) {
    size_t digits = 0;
    uint64_t value = 0;
    while (digits < text->size && prAsciiIsDigit(text->data[digits]) && digits < 10)
        value = value * 10 + uint64_t(text->data[digits++] - '0');
    bool ends = digits == text->size || text->data[digits] == '/';
    if (digits == 0 || !ends || (digits > 1 && text->data[0] == '0') || value >= kPrGltfNone)
        return false;
    *index = uint32_t(value);
    *text = prStringFrom(*text, digits);
    return true;
}

// Returns true if `rest`, what a pointer names within its object, is in `extras` or in an extension that the module
// does not read, which a pointer may name and the module does not resolve.
bool pointerIsForeign(PrStringView rest) {
    for (size_t at = 0; at < rest.size; ++at) {
        if (rest.data[at] != '/')
            continue;
        PrStringView token = prStringFrom(rest, at + 1);
        size_t end = prStringFindByte(token, '/');
        token = prStringHead(token, end < token.size ? end : token.size);
        if (token == "extras")
            return true;
        PrStringView next = prStringFrom(rest, at + 1 + token.size);
        if (token == "extensions" && next.size > 1) {
            PrStringView name = prStringFrom(next, 1);
            size_t stop = prStringFindByte(name, '/');
            name = prStringHead(name, stop < name.size ? stop : name.size);
            if (!prGltfExtensionIsSupported(name))
                return true;
        }
    }
    return false;
}

// Finds the property of `properties` that `rest` names. Returns null if none does.
const GltfPointerProperty* PR_NULLABLE findProperty(Span<const GltfPointerProperty> properties, PrStringView rest) {
    for (const GltfPointerProperty& property : properties) {
        if (rest == property.suffix)
            return &property;
    }
    return nullptr;
}

// Resolves a channel's pointer (KHR_animation_pointer) to the property that it names, which must be one that the Asset
// Object Model lets an animation target and that the asset defines: stated, or of an object that the asset has with a
// default. Sets the channel's path, object, element and width, and returns false if the pointer is invalid.
bool resolvePointer(GltfParser* parser, PrStringView text, PrGltfAnimationChannel* channel) {
    const Gltf* gltf = parser->gltf;
    auto invalid = [&](PrStringView message) {
        fail(parser, "pointer", message);
        return false;
    };
    if (text.size == 0 || text.data[0] != '/')
        return invalid("is not a JSON pointer");

    // The object: a node, a mesh, a camera, a material, or a light of KHR_lights_punctual.
    struct Prefix {
        PrStringView prefix;
        GltfPointerKind kind;
    };
    static const Prefix kKinds[] = {
        {"/nodes/", GltfPointerKind_Node}, {"/meshes/", GltfPointerKind_Mesh}, {"/cameras/", GltfPointerKind_Camera},
        {"/materials/", GltfPointerKind_Material},
        {"/extensions/KHR_lights_punctual/lights/", GltfPointerKind_Light},
    };
    PrStringView rest = {};
    GltfPointerKind kind = GltfPointerKind_Node;
    bool found = false;
    for (const Prefix& candidate : kKinds) {
        if (prStringStartsWith(text, candidate.prefix)) {
            rest = prStringFrom(text, candidate.prefix.size);
            kind = candidate.kind;
            found = true;
            break;
        }
    }
    if (!found) {
        // The root's extras, and the root's extensions that the module does not read, are the asset's own.
        if (pointerIsForeign(text))
            return true;
        return invalid("names no property that an animation can target");
    }
    uint32_t index = 0;
    if (!pointerIndex(&rest, &index))
        return invalid("names no object by its index");
    uint32_t counts[] = {
        gltf->nodes.count, gltf->meshes.count, gltf->cameras.count, gltf->lights.count,
        gltf->materials.count,
    };
    if (index >= counts[kind])
        return invalid("names an object that the asset does not have");
    if (pointerIsForeign(rest))
        return true;

    // The weights of a node's or a mesh's morph targets, and one of them.
    uint32_t morphs = 0;
    if (kind == GltfPointerKind_Node && gltf->nodes.data[index].mesh != kPrGltfNone)
        morphs = parser->morphCounts.data[gltf->nodes.data[index].mesh];
    else if (kind == GltfPointerKind_Mesh)
        morphs = parser->morphCounts.data[index];
    if ((kind == GltfPointerKind_Node || kind == GltfPointerKind_Mesh) && prStringStartsWith(rest, "/weights/")) {
        PrStringView element = prStringFrom(rest, 9);
        uint32_t target = 0;
        if (!pointerIndex(&element, &target) || element.size != 0)
            return invalid("names no property that an animation can target");
        if (target >= morphs)
            return invalid("names a weight of a morph target that the mesh does not have");
        channel->path = kind == GltfPointerKind_Node ? PrGltfPath_NodeWeight : PrGltfPath_MeshWeight;
        channel->object = index;
        channel->element = target;
        channel->width = 1;
        channel->node = kind == GltfPointerKind_Node ? index : kPrGltfNone;
        return true;
    }

    const GltfPointerProperty* property = nullptr;
    bool defined = true;
    switch (kind) {
        case GltfPointerKind_Node:
            property = findProperty(Span<const GltfPointerProperty>(kNodeProperties), rest);
            defined = property == nullptr || property->width != 0 || morphs != 0;
            break;
        case GltfPointerKind_Mesh:
            property = rest == kMeshWeights.suffix ? &kMeshWeights : nullptr;
            defined = morphs != 0;
            break;
        case GltfPointerKind_Camera: {
            property = findProperty(Span<const GltfPointerProperty>(kCameraProperties), rest);
            const PrGltfCamera& camera = gltf->cameras.data[index];
            bool perspective = prStringStartsWith(rest, "/perspective/");
            defined = perspective == (camera.type == PrGltfCameraType_Perspective);
            // A perspective camera's aspect ratio and far plane have no default.
            if (property != nullptr && perspective && property->path == PrGltfPath_CameraAspectRatio)
                defined = defined && camera.aspectRatio > 0;
            if (property != nullptr && perspective && property->path == PrGltfPath_CameraZfar)
                defined = defined && prF32IsFinite(camera.zfar);
            break;
        }
        case GltfPointerKind_Light: {
            property = findProperty(Span<const GltfPointerProperty>(kLightProperties), rest);
            const PrGltfLight& light = gltf->lights.data[index];
            // A light's range has no default, and only a spot light has a cone.
            if (property != nullptr && property->path == PrGltfPath_LightRange)
                defined = prF32IsFinite(light.range);
            else if (prStringStartsWith(rest, "/spot/"))
                defined = light.type == PrGltfLightType_Spot;
            break;
        }
        default: {
            const PrGltfMaterial& material = gltf->materials.data[index];
            property = findProperty(Span<const GltfPointerProperty>(kMaterialProperties), rest);
            if (property != nullptr) {
                defined = property->extension == 0 || (material.extensions & property->extension) != 0;
                if (property->path == PrGltfPath_MaterialNormalScale)
                    defined = material.normalTexture.texture != kPrGltfNone;
                else if (property->path == PrGltfPath_MaterialOcclusionStrength)
                    defined = material.occlusionTexture.texture != kPrGltfNone;
                else if (property->path == PrGltfPath_MaterialClearcoatNormalScale)
                    defined = defined && material.clearcoat.normalTexture.texture != kPrGltfNone;
                break;
            }
            // A property of the transform of one of the material's textures, which the asset gives the texture.
            for (uint32_t t = 0; t < PrGltfMaterialTexture_Count && property == nullptr; ++t) {
                const GltfMaterialTextureInfo& texture = kGltfMaterialTextures[t];
                if (!prStringStartsWith(rest, texture.pointer))
                    continue;
                property = findProperty(Span<const GltfPointerProperty>(kTransformProperties),
                    prStringFrom(rest, texture.pointer.size));
                const PrGltfTextureInfo* info = gltfMaterialTexture(const_cast<PrGltfMaterial*>(&material),
                    PrGltfMaterialTexture(t));
                defined = (texture.extension == 0 || (material.extensions & texture.extension) != 0) &&
                    info->texture != kPrGltfNone && info->hasTransform;
                channel->element = t;
            }
            break;
        }
    }
    if (property == nullptr)
        return invalid("names no property that an animation can target");
    if (!defined)
        return invalid("names a property that the asset does not define");
    channel->path = property->path;
    channel->object = index;
    channel->width = property->width != 0 ? property->width : morphs;
    channel->node = kind == GltfPointerKind_Node ? index : kPrGltfNone;
    return true;
}

// Returns the element type of the values of a property of `width` floats.
PrGltfElementType elementTypeOf(uint32_t width) {
    switch (width) {
        case 2:
            return PrGltfElementType_Vec2;
        case 3:
            return PrGltfElementType_Vec3;
        case 4:
            return PrGltfElementType_Vec4;
        default:
            return PrGltfElementType_Scalar;
    }
}

// Returns true if a path's value is the weights of morph targets, one scalar element for each.
bool isWeights(PrGltfPath path) {
    return path == PrGltfPath_Weights || path == PrGltfPath_MeshWeights;
}

// Reads a channel of the animation whose samplers are the `samplerCount` from `samplerFirst`.
void readAnimationChannel(GltfParser* parser, PrJsonValueIndex object, uint32_t samplerFirst, uint32_t samplerCount) {
    static const GltfName kPaths[] = {
        {"translation", PrGltfPath_Translation},
        {"rotation", PrGltfPath_Rotation},
        {"scale", PrGltfPath_Scale},
        {"weights", PrGltfPath_Weights},
    };
    Gltf* gltf = parser->gltf;
    PrGltfAnimationChannel channel = {};
    channel.node = kPrGltfNone;
    channel.object = kPrGltfNone;
    uint32_t sampler = requireIndex(parser, object, "sampler", samplerCount);
    PrJsonValueIndex target = container(parser, object, "target", PrJsonKind_Object);
    if (target == kPrJsonMissing)
        fail(parser, "target", "is required");
    if (failed(parser))
        return;
    channel.sampler = samplerFirst + sampler;
    parser->scope = "target";
    PrGltfNodeIndex node = readIndex(parser, target, "node", gltf->nodes.count);
    PrStringView path = prJsonString(parser->json, member(parser, target, "path"));
    if (path.data == nullptr)
        fail(parser, "path", "is required");
    PrJsonValueIndex pointer = extension(parser, target, kAnimationPointer);
    parser->scope = nullptr;
    if (failed(parser))
        return;

    if (path == "pointer" && pointer != kPrJsonMissing) {
        // A pointer names the property, and the channel names no node (KHR_animation_pointer).
        parser->scope = "target.extensions.KHR_animation_pointer";
        PrStringView text = prJsonString(parser->json, member(parser, pointer, "pointer"));
        if (node != kPrGltfNone)
            fail(parser, "node", "must not be defined with a pointer");
        else if (text.data == nullptr)
            fail(parser, "pointer", "is required");
        else
            resolvePointer(parser, text, &channel);
        parser->scope = nullptr;
        if (failed(parser))
            return;
        channel.pointer = gltfCopyString(gltf, text);
        if (channel.path == PrGltfPath_None)
            channel.path = PrGltfPath_Pointer;
    } else if (node == kPrGltfNone) {
        // A channel with no node animates nothing that the specification defines, whatever its path
        // (glTF-2.0/#animations-channels-target).
        gltf->animationChannels.push(channel);
        return;
    } else {
        for (const GltfName& name : kPaths) {
            if (path == name.name)
                channel.path = name.value;
        }
        if (channel.path == PrGltfPath_None) {
            fail(parser, "target.path", "is not a value of its enumeration");
            return;
        }
        channel.node = node;
        channel.object = node;
        uint32_t mesh = gltf->nodes.data[node].mesh;
        channel.width = channel.path == PrGltfPath_Rotation ? 4 : 3;
        if (channel.path == PrGltfPath_Weights)
            channel.width = mesh != kPrGltfNone ? parser->morphCounts.data[mesh] : 0;
    }
    if (channel.path == PrGltfPath_Pointer) {
        gltf->animationChannels.push(channel);
        return;
    }

    const PrGltfAnimationSampler& samples = gltf->animationSamplers.data[channel.sampler];
    const PrGltfAccessor& output = gltf->accessors.data[samples.output];
    bool normalized = isFloat(output) || isUnorm(output) || isSnorm(output);
    bool suits = false;
    switch (channel.path) {
        case PrGltfPath_Translation:
        case PrGltfPath_Scale:
            suits = output.type == PrGltfElementType_Vec3 && isFloat(output);
            break;
        case PrGltfPath_Rotation:
            suits = output.type == PrGltfElementType_Vec4 && normalized;
            break;
        case PrGltfPath_Weights:
            suits = output.type == PrGltfElementType_Scalar && normalized;
            break;
        case PrGltfPath_NodeVisible:
            // A boolean is an unsigned byte, held from one keyframe to the next.
            suits = output.type == PrGltfElementType_Scalar && output.componentType == PrGltfComponentType_UnsignedByte &&
                !output.normalized && samples.interpolation == PrGltfInterpolation_Step;
            break;
        default:
            // Any component converts to a float (KHR_animation_pointer).
            suits = output.type == elementTypeOf(isWeights(channel.path) ? 1 : channel.width);
            break;
    }
    if (!suits) {
        fail(parser, "sampler", "has an output accessor of a type that the path does not allow");
        return;
    }
    bool hasMatrix = channel.node != kPrGltfNone && gltf->nodes.data[channel.node].hasMatrix;
    bool transforms = channel.path == PrGltfPath_Translation || channel.path == PrGltfPath_Rotation ||
        channel.path == PrGltfPath_Scale;
    if (isWeights(channel.path) ? channel.width == 0 : transforms && hasMatrix) {
        fail(parser, "target", "names a property that its node cannot animate");
        return;
    }
    // A keyframe has one value, or for a cubic spline an in-tangent, a value and an out-tangent. A value of weights is
    // an element for each morph target, and any other value is one element (glTF-2.0/#animations-channels-sampler).
    uint64_t perKeyframe = isWeights(channel.path) ? channel.width : 1;
    if (samples.interpolation == PrGltfInterpolation_CubicSpline)
        perKeyframe *= 3;
    if (output.count != gltf->accessors.data[samples.input].count * perKeyframe) {
        fail(parser, "sampler", "has an output accessor whose count does not match its keyframes");
        return;
    }
    gltf->animationChannels.push(channel);
}

// What a channel animates, for finding two channels of an animation that animate one property: its path, its object,
// and its element, which is kPrGltfNone for every weight of an object's morph targets at once, so that it meets each
// weight alone.
struct GltfTargetKey {
    uint32_t path;
    uint32_t object;
    uint32_t element;
};

// Returns the key of a channel that has a target.
GltfTargetKey targetKey(const PrGltfAnimationChannel& channel) {
    switch (channel.path) {
        case PrGltfPath_Weights:
            return GltfTargetKey{PrGltfPath_NodeWeight, channel.object, kPrGltfNone};
        case PrGltfPath_MeshWeights:
            return GltfTargetKey{PrGltfPath_MeshWeight, channel.object, kPrGltfNone};
        default:
            return GltfTargetKey{channel.path, channel.object, channel.element};
    }
}

// Checks that no two channels of an animation animate one property, nor one weight and every weight of an object
// (glTF-2.0/#animations-usage). Sorts the channels' keys, so that those of one property are side by side.
void checkTargets(GltfParser* parser, const PrGltfAnimation& animation) {
    const Gltf* gltf = parser->gltf;
    RawArray<GltfTargetKey> keys;
    for (uint32_t i = 0; i < animation.channelCount; ++i) {
        const PrGltfAnimationChannel& channel = gltf->animationChannels.data[animation.channelFirst + i];
        if (channel.path != PrGltfPath_None && channel.path != PrGltfPath_Pointer)
            keys.push(targetKey(channel));
    }
    sortItems(Span<GltfTargetKey>(keys.data, keys.count), [](const GltfTargetKey& a, const GltfTargetKey& b) {
        if (a.path != b.path)
            return a.path < b.path;
        if (a.object != b.object)
            return a.object < b.object;
        return a.element < b.element;
    });
    for (uint32_t i = 1; i < keys.count; ++i) {
        const GltfTargetKey& a = keys.data[i - 1];
        const GltfTargetKey& b = keys.data[i];
        // Every weight sorts after each weight alone.
        if (a.path == b.path && a.object == b.object && (a.element == b.element || b.element == kPrGltfNone)) {
            fail(parser, "channels", "animate one property twice");
            break;
        }
    }
    keys.reset();
}

void readAnimation(GltfParser* parser, PrJsonValueIndex object) {
    Gltf* gltf = parser->gltf;
    PrGltfAnimation animation = {};
    animation.name = readString(parser, object, "name");
    animation.samplerFirst = gltf->animationSamplers.count;
    animation.channelFirst = gltf->animationChannels.count;
    PrJsonValueIndex samplers = container(parser, object, "samplers", PrJsonKind_Array);
    PrJsonValueIndex channels = container(parser, object, "channels", PrJsonKind_Array);
    if (samplers == kPrJsonMissing || channels == kPrJsonMissing)
        fail(parser, "", "must have channels and samplers");
    if (failed(parser))
        return;

    parser->part = "samplers";
    parser->partIndex = 0;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, samplers); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, samplers, i)) {
        readAnimationSampler(parser, i);
        if (failed(parser))
            return;
        ++animation.samplerCount;
        ++parser->partIndex;
    }
    parser->part = "channels";
    parser->partIndex = 0;
    for (PrJsonValueIndex i = prJsonFirst(parser->json, channels); i != kPrJsonMissing;
            i = prJsonAfter(parser->json, channels, i)) {
        readAnimationChannel(parser, i, animation.samplerFirst, animation.samplerCount);
        if (failed(parser))
            return;
        // The animation lasts until the last keyframe of a channel that has a target, which the bounds of the times
        // give without reading them.
        const PrGltfAnimationChannel& channel = gltf->animationChannels.last();
        if (channel.path != PrGltfPath_None) {
            const PrGltfAccessor& input = gltf->accessors.data[gltf->animationSamplers.data[channel.sampler].input];
            animation.duration = prF32Max(animation.duration, gltf->listFloats.data[input.max.first]);
        }
        ++animation.channelCount;
        ++parser->partIndex;
    }
    parser->part = nullptr;
    checkTargets(parser, animation);
    gltf->animations.push(animation);
}

// ---------------------------------------------------------------------------------------------------------------------
// Checks of the bytes of buffers

// Checks that an accessor's sparse indices increase strictly and name its elements (glTF-2.0/#sparse-accessor-indices).
void checkSparse(GltfParser* parser, uint32_t index) {
    const Gltf* gltf = parser->gltf;
    const PrGltfAccessor& accessor = gltf->accessors.data[index];
    if (accessor.sparse.count == 0 || !gltfAccessorHasBytes(gltf, accessor))
        return;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < accessor.sparse.count; ++i) {
        uint32_t element = gltfSparseIndex(gltf, accessor, i);
        if (element >= accessor.count || (i != 0 && element <= previous)) {
            fail(parser, "sparse.indices", "must increase strictly and be less than the accessor's count");
            return;
        }
        previous = element;
    }
}

// Reads every element of an accessor, once, and returns what that found.
const GltfScan& scan(GltfParser* parser, PrGltfAccessorIndex index) {
    const Gltf* gltf = parser->gltf;
    if (parser->scans.count == 0)
        parser->scans.resizeZeroed(gltf->accessors.count);
    GltfScan& found = parser->scans.data[index];
    if (found.scanned)
        return found;
    found.scanned = true;
    const PrGltfAccessor& accessor = gltf->accessors.data[index];
    uint32_t components = gltfComponentCount(accessor.type);
    // A time is a float scalar. The first is zero or more, and each later one is greater than the one before it. Both
    // comparisons are false for a NaN.
    bool isTimes = accessor.type == PrGltfElementType_Scalar && accessor.componentType == PrGltfComponentType_Float;
    float previous = 0;
    uint32_t integers[kCheckBatch];
    float floats[kCheckBatch];
    uint32_t batch = kCheckBatch / components;
    for (uint32_t first = 0; first < accessor.count; first += batch) {
        uint32_t count = accessor.count - first < batch ? accessor.count - first : batch;
        if (!gltfAccessorRead(gltf, accessor, first, count, GltfNumber_Integer, integers))
            return found;
        for (uint32_t i = 0; i < count * components; ++i)
            found.greatest = integers[i] > found.greatest ? integers[i] : found.greatest;
        if (!isTimes)
            continue;
        gltfAccessorRead(gltf, accessor, first, count, GltfNumber_Float, floats);
        for (uint32_t i = 0; i < count && isTimes; ++i) {
            bool ordered = first + i == 0 ? floats[i] >= previous : floats[i] > previous;
            isTimes = ordered && prF32IsFinite(floats[i]);
            previous = floats[i];
        }
    }
    found.hasBytes = true;
    found.isTimes = isTimes;
    return found;
}

// Checks that the indices of a mesh's primitives name their vertices (glTF-2.0/#meshes-topology), and finds what a skin
// needs to be drawn with the mesh.
void checkMesh(GltfParser* parser, uint32_t index) {
    const Gltf* gltf = parser->gltf;
    const PrGltfMesh& mesh = gltf->meshes.data[index];
    uint32_t jointLimit = 0;
    bool skinnable = true;
    for (uint32_t p = mesh.primitiveFirst; p < mesh.primitiveFirst + mesh.primitiveCount; ++p) {
        const PrGltfPrimitive& primitive = gltf->primitives.data[p];
        if (primitive.indices != kPrGltfNone) {
            const GltfScan& indices = scan(parser, primitive.indices);
            if (indices.hasBytes && indices.greatest >= primitive.vertexCount) {
                parser->part = "primitives";
                parser->partIndex = p - mesh.primitiveFirst;
                fail(parser, "indices", "has an index that names no vertex");
                parser->part = nullptr;
                return;
            }
        }
        skinnable = skinnable && parser->jointSets.data[p] != 0;
        for (uint32_t a = 0; a < primitive.attributeCount; ++a) {
            const PrGltfAttribute& attribute = gltf->attributes.data[primitive.attributeFirst + a];
            if (attribute.semantic != PrGltfSemantic_Joints)
                continue;
            const GltfScan& joints = scan(parser, attribute.accessor);
            if (joints.hasBytes && joints.greatest >= jointLimit)
                jointLimit = joints.greatest + 1;
        }
    }
    parser->jointLimits.push(jointLimit);
    parser->skinnable.push(skinnable);
}

// Checks that a node's skin has every joint that the vertices of the node's mesh name
// (glTF-2.0/#instantiation-skinned).
void checkNodeSkin(GltfParser* parser, uint32_t index) {
    const Gltf* gltf = parser->gltf;
    const PrGltfNode& node = gltf->nodes.data[index];
    if (node.skin == kPrGltfNone)
        return;
    if (!parser->skinnable.data[node.mesh])
        fail(parser, "skin", "is used with a mesh that has a primitive without joints");
    else if (parser->jointLimits.data[node.mesh] > gltf->skins.data[node.skin].joints.count)
        fail(parser, "skin", "has fewer joints than a vertex of the node's mesh names");
}

// Checks that the times of an animation's samplers are finite, start at zero or more, and increase strictly
// (glTF-2.0/#animations-samplers-input).
void checkAnimation(GltfParser* parser, uint32_t index) {
    const Gltf* gltf = parser->gltf;
    const PrGltfAnimation& animation = gltf->animations.data[index];
    for (uint32_t i = 0; i < animation.samplerCount; ++i) {
        const GltfScan& times = scan(parser, gltf->animationSamplers.data[animation.samplerFirst + i].input);
        if (times.hasBytes && !times.isTimes) {
            parser->part = "samplers";
            parser->partIndex = i;
            fail(parser, "input", "has times that do not increase strictly from zero or more");
            parser->part = nullptr;
            return;
        }
    }
}

// Runs `check` with each index below `count`, with the parser naming that element of the root's array `kind`.
template <typename Check>
void checkEach(GltfParser* parser, const char* kind, uint32_t count, Check check) {
    for (uint32_t i = 0; i < count && !failed(parser); ++i) {
        parser->kind = kind;
        parser->index = i;
        check(parser, i);
    }
    parser->kind = nullptr;
}

// Runs the checks that read the bytes of buffers. Sparse indices come first, because every later read of an accessor
// relies on them.
void checkBytes(GltfParser* parser) {
    const Gltf* gltf = parser->gltf;
    checkEach(parser, "accessors", gltf->accessors.count, checkSparse);
    checkEach(parser, "meshes", gltf->meshes.count, checkMesh);
    checkEach(parser, "nodes", gltf->nodes.count, checkNodeSkin);
    checkEach(parser, "animations", gltf->animations.count, checkAnimation);
}

// ---------------------------------------------------------------------------------------------------------------------
// The root

void readRoot(GltfParser* parser, PrJsonValueIndex root) {
    Gltf* gltf = parser->gltf;
    readAsset(parser, root);
    readExtensions(parser, root);
    if (failed(parser))
        return;

    PrJsonValueIndex nodes = container(parser, root, "nodes", PrJsonKind_Array);
    PrJsonValueIndex skins = container(parser, root, "skins", PrJsonKind_Array);
    parser->nodeCount = nodes != kPrJsonMissing ? prJsonCount(parser->json, nodes) : 0;
    parser->skinCount = skins != kPrJsonMissing ? prJsonCount(parser->json, skins) : 0;
    PrJsonValueIndex lights = extension(parser, root, kLightsPunctual);
    if (lights != kPrJsonMissing)
        lights = container(parser, lights, "lights", PrJsonKind_Array);

    eachObject(parser, "buffers", container(parser, root, "buffers", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readBuffer(parser, object); });
    eachObject(parser, "bufferViews", container(parser, root, "bufferViews", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readBufferView(parser, object); });
    eachObject(parser, "accessors", container(parser, root, "accessors", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readAccessor(parser, object); });
    eachObject(parser, "images", container(parser, root, "images", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readImage(parser, object); });
    eachObject(parser, "samplers", container(parser, root, "samplers", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readSampler(parser, object); });
    eachObject(parser, "textures", container(parser, root, "textures", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readTexture(parser, object); });
    eachObject(parser, "materials", container(parser, root, "materials", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readMaterial(parser, object); });
    PrJsonValueIndex variants = extension(parser, root, kVariants);
    if (variants != kPrJsonMissing)
        variants = container(parser, variants, "variants", PrJsonKind_Array);
    eachObject(parser, "extensions.KHR_materials_variants.variants", variants, [&](PrJsonValueIndex object) {
        PrStringView name = readString(parser, object, "name");
        if (name.data == nullptr)
            fail(parser, "name", "is required");
        gltf->variants.push(name);
    });
    eachObject(parser, "meshes", container(parser, root, "meshes", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readMesh(parser, object); });
    eachObject(parser, "cameras", container(parser, root, "cameras", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readCamera(parser, object); });
    eachObject(parser, "extensions.KHR_lights_punctual.lights", lights,
        [&](PrJsonValueIndex object) { readLight(parser, object); });
    eachObject(parser, "nodes", nodes, [&](PrJsonValueIndex object) { readNode(parser, object); });
    linkNodes(parser);
    eachObject(parser, "skins", skins, [&](PrJsonValueIndex object) { readSkin(parser, object); });
    eachObject(parser, "scenes", container(parser, root, "scenes", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readScene(parser, object); });
    eachObject(parser, "animations", container(parser, root, "animations", PrJsonKind_Array),
        [&](PrJsonValueIndex object) { readAnimation(parser, object); });
    if (failed(parser))
        return;

    gltf->scene = readIndex(parser, root, "scene", gltf->scenes.count);
    checkBytes(parser);
}

} // namespace

PrStatus gltfParse(Gltf* gltf, PrStringView json, PrByteView binaryChunk, const PrGltfOptions& options) {
    GltfParser parser = {};
    parser.gltf = gltf;
    parser.options = &options;
    parser.binaryChunk = binaryChunk;
    parser.json = prJsonCreate();
    gltf->scene = kPrGltfNone;

    if (!prJsonParse(parser.json, json, 0, nullptr))
        failWith(&parser, PrStatus_Syntax, "the asset", "is not JSON");
    else if (prJsonKind(parser.json, 0) != PrJsonKind_Object)
        fail(&parser, "the asset", "must be a JSON object");
    else
        readRoot(&parser, 0);

    prJsonDestroy(parser.json);
    parser.morphCounts.reset();
    parser.jointSets.reset();
    parser.scans.reset();
    parser.jointLimits.reset();
    parser.skinnable.reset();
    parser.marks.reset();
    return parser.status;
}

} // namespace pr_impl
