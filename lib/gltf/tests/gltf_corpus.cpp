// The glTF corpus harness, which tools/suite/gltf.py drives.
//
// `gltf_corpus --supports <extension>...` writes one JSON object that maps each extension named to whether the module
// reads it. `gltf_corpus <path>...` reads the asset at each path, with the buffers and images that it names loaded from
// beside it, and writes a line of JSON for each: {"status": the name of prGltfCreate's status, "diagnostics": its
// sentence, "absent": the number of buffers and images that have no bytes, "problem": what reading the document's data
// found wrong, or the empty string}. With a document in hand, the harness unpacks every accessor, unrolls every
// primitive into triangles, evaluates every animation channel, computes every world matrix and every instance's, and
// finds the material of every primitive in every variant, which is everything a consumer reads through the module, and
// checks what the document guarantees of each.

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/gltf.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

using namespace harness;
using namespace pr;

namespace {

// The number of elements that the harness unpacks at a time.
constexpr uint32_t kBatch = 1024;

// Loads the files that an asset names from the asset's directory.
struct Loader {
    PrStringView directory;
};

// Loads the file that `uri`, a percent-encoded relative path, names in the loader's directory.
PrBlob* PR_NULLABLE load(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
    const Loader* loader = static_cast<const Loader*>(context);
    PrTextBuffer decoded = {};
    for (size_t i = 0; i < uri.size; ++i) {
        bool escape = uri.data[i] == '%' && i + 2 < uri.size && prAsciiIsHexDigit(uri.data[i + 1]) &&
            prAsciiIsHexDigit(uri.data[i + 2]);
        if (escape) {
            prTextBufferAppendChar(&decoded, char(prAsciiHexValue(uri.data[i + 1]) * 16 + prAsciiHexValue(uri.data[i + 2])));
            i += 2;
        } else {
            prTextBufferAppendChar(&decoded, uri.data[i]);
        }
    }
    PrTextBuffer path = {};
    prTextBufferAppend(&path, loader->directory);
    prFilePathAppend(&path, prTextBufferView(&decoded));
    PrBlob* blob = prFileOpen(prTextBufferView(&path));
    prTextBufferReset(&decoded);
    prTextBufferReset(&path);
    return blob;
}

bool allFinite(const float* values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!prF32IsFinite(values[i]))
            return false;
    }
    return true;
}

// Reads everything that the document offers. Returns what it found wrong, or null.
const char* PR_NULLABLE exercise(const PrGltf* gltf) {
    Array<float> floats;
    Array<uint32_t> integers;
    floats.resize(kBatch * 16);
    integers.resize(kBatch * 16);

    // An asset states the bounds of an accessor's data exactly (glTF-2.0/#accessor-bounds), so the bounds of what the
    // module unpacks check its reading of stride, of padding and of sparse elements against the asset's own word.
    const PrGltfAccessor* accessors = prGltfAccessors(gltf);
    for (uint32_t a = 0; a < prGltfAccessorCount(gltf); ++a) {
        const PrGltfAccessor& accessor = accessors[a];
        uint32_t components = prGltfElementTypeComponentCount(accessor.type);
        bool isFloat = accessor.componentType == PrGltfComponentType_Float;
        bool isSigned = accessor.componentType == PrGltfComponentType_Byte ||
            accessor.componentType == PrGltfComponentType_Short;
        float least[16];
        float greatest[16];
        for (uint32_t first = 0; first < accessor.count; first += kBatch) {
            uint32_t count = accessor.count - first < kBatch ? accessor.count - first : kBatch;
            if (!prGltfAccessorReadFloats(gltf, a, first, count, floats.data()) ||
                    !prGltfAccessorReadIntegers(gltf, a, first, count, integers.data()))
                return "an accessor has no bytes";
            for (uint32_t i = 0; i < count * components; ++i) {
                // The bounds of integers are those of the integers themselves, normalized or not.
                float value = floats[i];
                if (!isFloat)
                    value = isSigned ? float(int32_t(integers[i])) : float(integers[i]);
                uint32_t c = i % components;
                least[c] = first == 0 && i < components ? value : prF32Min(least[c], value);
                greatest[c] = first == 0 && i < components ? value : prF32Max(greatest[c], value);
            }
        }
        // The bounds of data that a meshopt filter decodes are those of the uncompressed data, which the filter only
        // approaches (KHR_meshopt_compression).
        bool filtered = accessor.draco || (accessor.bufferView != kPrGltfNone &&
            prGltfBufferViews(gltf)[accessor.bufferView].meshopt.filter != PrGltfMeshoptFilter_None);
        const float* statedLeast = prGltfListFloats(gltf, accessor.min);
        const float* statedGreatest = prGltfListFloats(gltf, accessor.max);
        for (uint32_t c = 0; c < components && !filtered; ++c) {
            if (accessor.min.count != 0 && statedLeast[c] != least[c])
                return "an accessor's min is not the least of its data";
            if (accessor.max.count != 0 && statedGreatest[c] != greatest[c])
                return "an accessor's max is not the greatest of its data";
        }
    }

    const PrGltfPrimitive* primitives = prGltfPrimitives(gltf);
    Array<uint32_t> triangles;
    for (uint32_t p = 0; p < prGltfPrimitiveCount(gltf); ++p) {
        uint32_t total = prGltfPrimitiveTriangles(gltf, p, nullptr, 0);
        triangles.resize(total);
        if (prGltfPrimitiveTriangles(gltf, p, triangles.data(), total) != total)
            return "a primitive's triangle list changed length";
        bool isTriangles = primitives[p].mode >= PrGltfMode_Triangles;
        if (isTriangles != (total != 0))
            return "a primitive has triangles if and only if its mode assembles them";
        for (uint32_t i = 0; i < total; ++i) {
            if (triangles[i] >= primitives[p].vertexCount)
                return "a triangle names no vertex";
        }
    }

    const PrGltfAnimation* animations = prGltfAnimations(gltf);
    const PrGltfAnimationChannel* channels = prGltfAnimationChannels(gltf);
    for (uint32_t a = 0; a < prGltfAnimationCount(gltf); ++a) {
        for (uint32_t c = 0; c < animations[a].channelCount; ++c) {
            uint32_t channel = animations[a].channelFirst + c;
            floats.resize(channels[channel].width > kBatch * 16 ? channels[channel].width : kBatch * 16);
            for (uint32_t step = 0; step <= 8; ++step) {
                float seconds = animations[a].duration * float(step) / 8;
                if (!prGltfAnimationChannelEvaluate(gltf, channel, seconds, floats.data()))
                    return "an animation channel has no bytes";
                if (!allFinite(floats.data(), channels[channel].width))
                    return "an animation channel's value is not finite";
            }
        }
    }

    Array<PrMatrix4> world;
    world.resize(prGltfNodeCount(gltf));
    prGltfWorldMatrices(gltf, world.data());
    for (uint32_t n = 0; n < world.count(); ++n) {
        if (!allFinite(&world[n].m[0][0], 16))
            return "a node's world matrix is not finite";
    }

    // The instances of a node's mesh, a batch at a time.
    Array<PrMatrix4> instances;
    instances.resize(kBatch);
    for (uint32_t n = 0; n < prGltfNodeCount(gltf); ++n) {
        uint32_t count = prGltfNodes(gltf)[n].instancing.count;
        for (uint32_t first = 0; first < count; first += kBatch) {
            uint32_t batch = count - first < kBatch ? count - first : kBatch;
            if (!prGltfInstanceMatrices(gltf, n, first, batch, instances.data()))
                return "a node's instances have no bytes";
            for (uint32_t i = 0; i < batch; ++i) {
                if (!allFinite(&instances[i].m[0][0], 16))
                    return "an instance's matrix is not finite";
            }
        }
    }

    // The material of each primitive in each variant, which is a material of the document or the default.
    for (uint32_t p = 0; p < prGltfPrimitiveCount(gltf); ++p) {
        for (uint32_t v = 0; v < prGltfVariantCount(gltf); ++v) {
            PrGltfMaterialIndex material = prGltfPrimitiveMaterial(gltf, p, v);
            if (material != kPrGltfNone && material >= prGltfMaterialCount(gltf))
                return "a variant's material is not a material of the document";
        }
    }
    return nullptr;
}

void run(const char* path) {
    PrTextBuffer out = {};
    PrTextBuffer diagnostics = {};
    prTextBufferAppend(&out, "{\"status\":");
    PrBlob* file = prFileOpen(path);
    if (file == nullptr) {
        prTextBufferAppend(&out, "\"Io\",\"diagnostics\":\"the file cannot be opened\",\"absent\":0,\"problem\":\"\"}");
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        return;
    }
    Loader loader = {prFilePathParent(path)};
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = load;
    options.loadContext = &loader;
    options.diagnostics = &diagnostics;
    PrGltf* gltf = nullptr;
    PrStatus status = prGltfCreate(file, &options, &gltf);
    prBlobRelease(file);

    uint32_t absent = 0;
    const char* problem = nullptr;
    if (gltf != nullptr) {
        for (uint32_t i = 0; i < prGltfBufferCount(gltf); ++i)
            absent += prGltfBuffers(gltf)[i].bytes.data == nullptr && !prGltfBuffers(gltf)[i].fallback;
        for (uint32_t i = 0; i < prGltfImageCount(gltf); ++i)
            absent += prGltfImages(gltf)[i].bytes.data == nullptr;
        if (absent == 0)
            problem = exercise(gltf);
    }
    prGltfDestroy(gltf);

    harnessWriteString(&out, prStatusName(status));
    prTextBufferAppend(&out, ",\"diagnostics\":");
    harnessWriteString(&out, prTextBufferView(&diagnostics));
    prTextBufferAppend(&out, ",\"absent\":");
    harnessWriteNumber(&out, absent);
    prTextBufferAppend(&out, ",\"problem\":");
    harnessWriteString(&out, problem != nullptr ? problem : "");
    prTextBufferAppendChar(&out, '}');
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
    prTextBufferReset(&diagnostics);
}

// Writes whether the module reads each extension named.
void supports(int count, char** names) {
    PrTextBuffer out = {};
    prTextBufferAppendChar(&out, '{');
    for (int i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&out, ',');
        harnessWriteString(&out, names[i]);
        prTextBufferAppend(&out, prGltfExtensionIsSupported(names[i]) ? ":true" : ":false");
    }
    prTextBufferAppendChar(&out, '}');
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 1 && PrStringView(argv[1]) == "--supports") {
        supports(argc - 2, argv + 2);
        return 0;
    }
    for (int i = 1; i < argc; ++i)
        run(argv[i]);
    return 0;
}
