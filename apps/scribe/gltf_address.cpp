// The address handler of glTF assets, JSON and binary: a step is a JSON Pointer into the asset's JSON.

#include "apps/scribe/gltf_address.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/construct.h"
#include "pr/format.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

namespace scribe {

namespace {

// The arrays of an asset whose elements are bytes, by the name that a pointer gives them.
enum {
    GltfBytes_None,
    GltfBytes_Image,
    GltfBytes_BufferView,
    GltfBytes_Buffer,
};
typedef uint8_t GltfBytes;

// A nested resource that an asset embeds.
struct GltfChild {
    String step;
    String path;
    uint64_t size = 0;
};

// An opened asset: its document, its JSON, and the images that it embeds.
struct GltfState {
    PrGltf* gltf = nullptr;
    PrJson* json = nullptr;
    Array<GltfChild> children;
};

// Loads what an asset names by URI beside the asset, through the node's relative reads.
PrBlob* PR_NULLABLE gltfLoad(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
    AddressNode* node = static_cast<AddressNode*>(context);
    // A URI is a percent-encoded relative path (glTF-2.0/#uris).
    String path;
    for (size_t i = 0; i < uri.size; ++i) {
        if (uri.data[i] == '%' && i + 2 < uri.size && prAsciiIsHexDigit(uri.data[i + 1]) && prAsciiIsHexDigit(uri.data[i + 2])) {
            path.append(char(prAsciiHexValue(uri.data[i + 1]) * 16 + prAsciiHexValue(uri.data[i + 2])));
            i += 2;
        } else {
            path.append(uri.data[i]);
        }
    }
    PrBlob* blob = nullptr;
    addressNodeReadRelative(node, path, &blob);
    return blob;
}

// Returns the JSON of an asset: the bytes of a JSON asset, or the first chunk of binary glTF
// (glTF-2.0/#binary-gltf-layout).
PrStringView jsonOf(PrByteView bytes) {
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    if (bytes.size < 20 || !prMemEquals(bytes.data, "glTF", 4))
        return text;
    uint32_t length = prMemReadLeU32(bytes.data + 12);
    if (!prMemEquals(bytes.data + 16, "JSON", 4) || length > bytes.size - 20)
        return "";
    return PrStringView{reinterpret_cast<const char*>(bytes.data + 20), length};
}

// Returns the array that `step`, a pointer of exactly two reference tokens, names the bytes of an element of, and
// writes the element's index to `*index`.
GltfBytes bytesOf(PrStringView step, uint32_t* index) {
    struct Named {
        PrStringView prefix;
        GltfBytes bytes;
    };
    constexpr Named kNamed[] = {
        {"/images/", GltfBytes_Image},
        {"/bufferViews/", GltfBytes_BufferView},
        {"/buffers/", GltfBytes_Buffer},
    };
    for (const Named& named : kNamed) {
        if (!prStringStartsWith(step, named.prefix))
            continue;
        PrStringView digits = prStringFrom(step, named.prefix.size);
        if (digits.size == 0 || digits.size > 9)
            return GltfBytes_None;
        uint32_t value = 0;
        for (size_t i = 0; i < digits.size; ++i) {
            if (!prAsciiIsDigit(digits.data[i]))
                return GltfBytes_None;
            value = value * 10 + uint32_t(digits.data[i] - '0');
        }
        *index = value;
        return named.bytes;
    }
    return GltfBytes_None;
}

// Returns the file extension of an image's media type, or "bin".
PrStringView extensionOf(PrStringView mimeType) {
    struct Extension {
        PrStringView type;
        PrStringView extension;
    };
    constexpr Extension kExtensions[] = {
        {"image/png", "png"}, {"image/jpeg", "jpg"}, {"image/webp", "webp"}, {"image/ktx2", "ktx2"},
    };
    for (const Extension& known : kExtensions) {
        if (mimeType == known.type)
            return known.extension;
    }
    return "bin";
}

// Returns the name that an image is classified and listed by: its own name, or the last part of its URI, or `image` and
// its index, with the extension of its media type.
String imageName(const PrGltfImage& image, uint32_t index) {
    PrStringView uri = image.uri;
    if (uri.size != 0 && !prStringStartsWith(uri, "data:")) {
        size_t slash = prStringFindLastByte(uri, '/');
        return slash == uri.size ? String(uri) : String(prStringFrom(uri, slash + 1));
    }
    String name;
    if (image.name.size != 0 && prStringFindByte(image.name, '/') == image.name.size)
        name = image.name;
    else
        name = format("image{}", index);
    PrStringView extension = extensionOf(image.mimeType);
    if (!prAsciiEndsWithFold(name, extension))
        formatTo(name, ".{}", extension);
    return name;
}

bool gltfOpens(const AddressHandler*, PrMimeFormat format) {
    return format == PrMimeFormat_Gltf || format == PrMimeFormat_Glb;
}

void gltfClose(AddressHandler*, void* PR_NULLABLE state) {
    GltfState* asset = static_cast<GltfState*>(state);
    prJsonDestroy(asset->json);
    prGltfDestroy(asset->gltf);
    destroy(asset);
}

PrStatus gltfOpen(AddressHandler*, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) {
    PrBlob* bytes = addressNodeBytes(node);
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = gltfLoad;
    options.loadContext = node;
    PrGltf* gltf = nullptr;
    PrStatus status = prGltfCreate(bytes, &options, &gltf);
    if (status != PrStatus_Ok)
        return status;
    GltfState* asset = create<GltfState>();
    asset->gltf = gltf;
    asset->json = prJsonCreate();
    // The document read the JSON, so it parses.
    prJsonParse(asset->json, jsonOf(prBlobBytes(bytes)), 0, nullptr);
    // The images that the asset embeds, which no file beside it holds.
    const PrGltfImage* images = prGltfImages(gltf);
    for (uint32_t i = 0; i < prGltfImageCount(gltf); ++i) {
        const PrGltfImage& image = images[i];
        bool embedded = image.uri.data == nullptr || prStringStartsWith(image.uri, "data:") || image.uri.size == 0;
        if (!embedded || image.bytes.data == nullptr)
            continue;
        asset->children.push(GltfChild{
            .step = format("/images/{}", i),
            .path = format("images/{}", imageName(image, i)),
            .size = image.bytes.size,
        });
    }
    *state = asset;
    return PrStatus_Ok;
}

PrStatus gltfStep(AddressHandler*, AddressNode* node, void* PR_NULLABLE state, PrStringView step, uint64_t,
        AddressStepResult* out) {
    const GltfState* asset = static_cast<const GltfState*>(state);
    // A step is a pointer to a value, and the empty pointer, the whole asset, is no step.
    if (step.size == 0)
        return PrStatus_Syntax;
    PrJsonValueIndex value = kPrJsonMissing;
    PrStatus status = prJsonValueFromPointer(asset->json, step, &value);
    if (status != PrStatus_Ok)
        return status;
    const PrGltf* gltf = asset->gltf;
    uint32_t index = 0;
    GltfBytes kind = bytesOf(step, &index);
    PrByteView bytes{};
    if (kind == GltfBytes_Image && index < prGltfImageCount(gltf)) {
        bytes = prGltfImages(gltf)[index].bytes;
        out->name = imageName(prGltfImages(gltf)[index], index);
    } else if (kind == GltfBytes_BufferView && index < prGltfBufferViewCount(gltf)) {
        bytes = prGltfBufferViewBytes(gltf, index);
    } else if (kind == GltfBytes_Buffer && index < prGltfBufferCount(gltf)) {
        const PrGltfBuffer& buffer = prGltfBuffers(gltf)[index];
        bytes = PrByteView{buffer.bytes.data, buffer.bytes.data != nullptr ? size_t(buffer.byteLength) : 0};
    } else {
        out->kind = AddressNodeKind_Location;
        return PrStatus_Ok;
    }
    // Bytes that were not loaded are not there.
    if (bytes.data == nullptr)
        return PrStatus_NotFound;
    out->kind = AddressNodeKind_Resource;
    out->bytes = addressNodeShare(node, bytes);
    return PrStatus_Ok;
}

uint32_t gltfChildCount(const AddressHandler*, const void* PR_NULLABLE state) {
    return static_cast<const GltfState*>(state)->children.count();
}

AddressChild gltfChild(const AddressHandler*, const void* PR_NULLABLE state, uint32_t index) {
    const GltfChild& child = static_cast<const GltfState*>(state)->children[index];
    return AddressChild{.step = child.step, .path = child.path, .size = child.size};
}

void gltfDestroy(AddressHandler* handler) {
    destroy(handler);
}

// Returns the state of a node that the glTF handler opens, opening it if it is not open, or null.
const GltfState* PR_NULLABLE stateOf(AddressNode* node) {
    const AddressHandler* handler = addressNodeHandler(node);
    if (handler == nullptr || handler->openFn != gltfOpen || addressNodeOpen(node) != PrStatus_Ok)
        return nullptr;
    return static_cast<const GltfState*>(addressNodeState(node, handler));
}

} // namespace

AddressHandler* gltfAddressHandlerCreate() {
    AddressHandler* handler = create<AddressHandler>();
    handler->destroyFn = gltfDestroy;
    handler->opensFn = gltfOpens;
    handler->openFn = gltfOpen;
    handler->closeFn = gltfClose;
    handler->stepFn = gltfStep;
    handler->childCountFn = gltfChildCount;
    handler->childFn = gltfChild;
    return handler;
}

const PrGltf* PR_NULLABLE gltfAddressDocument(AddressNode* node) {
    const GltfState* asset = stateOf(node);
    return asset != nullptr ? asset->gltf : nullptr;
}

const PrJson* PR_NULLABLE gltfAddressJson(AddressNode* node) {
    const GltfState* asset = stateOf(node);
    return asset != nullptr ? asset->json : nullptr;
}

} // namespace scribe
