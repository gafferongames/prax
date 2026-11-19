// The fixture for the tests of lib/render and of the libraries over it: a renderer and a graph on each device that the
// tests run on, small scenes of quads, and views rendered offscreen and read back as floats.

#pragma once

#include "pr/array.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/map.h"
#include "pr/mem.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_shaders.h"
#include "pr/render_view.h"
#include "pr/span.h"

#if defined(PR_RENDER_TEST_VULKAN)
#include "pr/gpu_vulkan.h"
#endif
#if defined(PR_RENDER_TEST_WEBGPU)
#include "pr/gpu_webgpu.h"
#endif

#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Texels read back from a texture, as four floats each.
struct Image {
    PrUVec2 extent{};
    Array<float> texels;

    const float* at(uint32_t x, uint32_t y) const {
        return &texels[(y * extent.x + x) * 4];
    }
};

// How two images of one extent differ, over their red, green and blue.
struct Difference {
    float largest = 0;          // the largest difference of a channel
    double mean = 0;            // the mean difference of a channel
    uint32_t over = 0;          // the pixels with a channel that differs by more than the tolerance
};

// How far the two binding tiers may draw a channel apart: a step of a half float below 1. The tiers compile the same
// arithmetic into two libraries, beside different code to reach a texture, and fast math lets a compiler fuse and
// reassociate each where that code allows, so they may round apart. On Metal they differed by 1.2e-4 at one pixel of a
// page of examples/render_features, and by nothing with fast math off (pr_render_shaders' PRECISE_MATH).
constexpr float kTierTolerance = 1.0f / 2048;

// Returns how two images differ, counting the pixels that differ by more than `tolerance`.
inline Difference compare(const Image& a, const Image& b, float tolerance) {
    REQUIRE(a.extent == b.extent);
    Difference difference;
    uint32_t pixels = a.extent.x * a.extent.y;
    for (uint32_t i = 0; i < pixels; ++i) {
        float worst = 0;
        for (uint32_t channel = 0; channel < 3; ++channel) {
            float d = prF32Abs(a.texels[i * 4 + channel] - b.texels[i * 4 + channel]);
            worst = prF32Max(worst, d);
            difference.mean += double(d);
        }
        difference.largest = prF32Max(difference.largest, worst);
        if (worst > tolerance)
            ++difference.over;
    }
    difference.mean /= double(pixels) * 3;
    return difference;
}

// Returns true if the first three channels of a texel are each within `tolerance` of a color.
inline bool near(const float* texel, PrVec3 color, float tolerance) {
    return prF32Abs(texel[0] - color.x) <= tolerance && prF32Abs(texel[1] - color.y) <= tolerance &&
        prF32Abs(texel[2] - color.z) <= tolerance;
}

// Copies texels of four half floats each into an image. Called on the device's thread.
inline void readHalves(void* PR_NULLABLE context, PrPixelView texels) {
    Image& image = *static_cast<Image*>(context);
    image.extent = texels.extent;
    image.texels.resize(texels.extent.x * texels.extent.y * 4);
    for (uint32_t y = 0; y < texels.extent.y; ++y) {
        const uint8_t* row = texels.data + size_t(y) * texels.stride;
        float* out = &image.texels[y * texels.extent.x * 4];
        for (uint32_t i = 0; i < texels.extent.x * 4; ++i) {
            PrFloat16 half;
            prMemCopy(&half, row + i * 2, 2);
            out[i] = prFloat16ToFloat(half);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Devices

// A device that the tests share, made once for the binary, since making one costs more than a test.
struct TestDevice {
    PrGpuBackend backend = PrGpuBackend_Default;
    bool slotsOnly = false;
    PrGpuDevice* PR_NULLABLE device = nullptr;
};

// The devices that the tests run on: Metal, Metal 4, Vulkan and Direct3D 12, each with handles and with slots only, and
// WebGPU, which has slots alone, where the build has the backend and the system a device for it. They validate. The
// last test destroys them (destroyTestDevices), because a validation layer is gone by the time a binary's statics are
// destroyed.
struct TestDevices {
    Array<TestDevice> devices;

    TestDevices() {
        gpuValidation();
        const PrGpuBackend backends[] =
            {PrGpuBackend_Metal, PrGpuBackend_Metal4, PrGpuBackend_Vulkan, PrGpuBackend_D3d12, PrGpuBackend_Webgpu};
        const bool slotModes[] = {false, true};
        for (PrGpuBackend backend : backends) {
            for (bool slotsOnly : slotModes) {
                // A device of WebGPU is of slots alone with either option, so it is made once.
                if (backend == PrGpuBackend_Webgpu && slotsOnly)
                    continue;
                PrGpuDeviceOptions options{.validation = true, .slotsOnly = slotsOnly};
                PrGpuDevice* device = nullptr;
                if (gpuTests() && gpuDeviceCreate(backend, &options, &device) == PrStatus_Ok)
                    devices.push(TestDevice{backend, slotsOnly, device});
            }
        }
    }
};

// Returns the shared devices, which the first call creates.
inline Array<TestDevice>& testDevices() {
    static TestDevices devices;
    return devices.devices;
}

// Destroys the shared devices. No test runs on them after the call.
inline void destroyTestDevices() {
    for (const TestDevice& device : testDevices())
        prGpuDeviceDestroy(device.device);
    testDevices().clear();
}

// Returns the number of validation errors that a device has reported, where the device counts them. Metal's validation
// ends the process instead.
inline uint32_t validationErrors(const TestDevice& device) {
#if defined(PR_RENDER_TEST_VULKAN)
    if (device.backend == PrGpuBackend_Vulkan)
        return prGpuVulkanValidationErrors(device.device);
#endif
#if defined(PR_RENDER_TEST_WEBGPU)
    if (device.backend == PrGpuBackend_Webgpu)
        return prGpuWebgpuValidationErrors(device.device);
#endif
    (void)device;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The fixture

// The extent of the tests' views, which has a pixel at its center.
constexpr PrUVec2 kExtent{33, 33};
constexpr uint32_t kCenter = 16;

// The width of a face of the cube of an environment that the tests make from an equirectangular image or from a sky.
constexpr uint32_t kEnvironmentSize = 64;

// A renderer of a test's own on one of the shared devices, and a graph to build its frames in. The renderer draws with
// `shaders`, or with the library's own shader set if that is null. `tables` fill the tables of the set's own.
struct Fixture {
    TestDevice shared;
    PrGpuDevice* device = nullptr;
    PrRender* render = nullptr;
    PrGpuGraph* graph = nullptr;
    bool bindless = false;
    PrGpuGraphStats stats{};                  // what recording the last frame's graph did
    Map<uint32_t, PrGpuSamplerDesc> samplers; // by texture id, the sampler that the fixture's uses of it read by

    explicit Fixture(const TestDevice& testDevice, const PrRenderShaderSet* PR_NULLABLE shaders = nullptr,
            Span<const PrRenderTableSource> tables = {}) : shared(testDevice), device(testDevice.device) {
        bindless = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_Bindless) != 0;
        // A small shadow map and small environments keep the tests fast.
        PrRenderOptions options{
            .shadowMapSize = 512,
            .environmentSize = kEnvironmentSize,
            .shaders = shaders,
            .tables = tables.begin(),
            .tableCount = uint32_t(tables.count()),
        };
        REQUIRE(prRenderCreate(device, &options, &render) == PrStatus_Ok);
        graph = prGpuGraphCreate(device, nullptr);
    }

    ~Fixture() {
        prGpuDeviceWaitIdle(device);
        prGpuGraphDestroy(graph);
        prRenderDestroy(render);
    }

    // Begins a frame that renders nothing, which applies what was set and committed.
    void frame() {
        prGpuGraphBegin(graph);
        prRenderFrameBegin(render, graph);
    }

    // Records the frame's graph and waits for its work.
    void record() {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        stats = prGpuGraphRecord(graph, commands);
        CHECK(prGpuGraphErrorCount(graph) == 0);
        gpuSubmitAndWait(commands);
    }

    // What a read of a texture of the frame needs while the graph records.
    struct Read {
        PrGpuGraph* graph;
        PrGpuGraphTexture texture;
        Image* image;
    };

    // Adds work to the frame's graph that reads a texture of the frame into `image`.
    void addRead(PrGpuGraphTexture texture, Image* image) {
        Read* read = static_cast<Read*>(prGpuGraphAlloc(graph, sizeof(Read)));
        *read = Read{graph, texture, image};
        PrGpuGraphPass pass = prGpuGraphAddWork(graph, "read", [](void* PR_NULLABLE context, PrGpuCommands* commands) {
            const Read& r = *static_cast<const Read*>(context);
            PrGpuTexture texture = prGpuGraphTexture(r.graph, r.texture);
            if (texture.id != 0)
                prGpuCommandsReadTexture(commands, texture, PrGpuAccessFlag_ShaderRead, readHalves, r.image);
        }, read);
        prGpuGraphUseTexture(graph, pass, texture, PrGpuAccessFlag_ShaderRead);
    }

    // Renders each view in one frame, and writes each view's color, as scene-referred linear sRGB, to `out`.
    void views(Span<const PrRenderViewDesc> descs, Array<Image>* out) {
        out->clear();
        out->resize(descs.count());
        prGpuGraphBegin(graph);
        prRenderFrameBegin(render, graph);
        for (uint32_t i = 0; i < descs.count(); ++i)
            addRead(prRenderViewAdd(render, &descs[i]).color, &(*out)[i]);
        record();
    }

    Image view(const PrRenderViewDesc& desc) {
        Array<Image> images;
        views(Span<const PrRenderViewDesc>(&desc, 1), &images);
        return images[0];
    }

    // What the display's pass needs while the graph records.
    struct Shown {
        PrRender* render;
        PrGpuGraph* graph;
        PrGpuGraphTexture color;
        PrRenderDisplayOptions display;
    };

    // Renders a view and draws its color as `display` says into a target of half floats, and returns the target.
    Image shown(const PrRenderViewDesc& desc, const PrRenderDisplayOptions& display) {
        Image image;
        prGpuGraphBegin(graph);
        prRenderFrameBegin(render, graph);
        Shown* shownAs = static_cast<Shown*>(prGpuGraphAlloc(graph, sizeof(Shown)));
        *shownAs = Shown{render, graph, prRenderViewAdd(render, &desc).color, display};
        PrGpuTextureDesc targetDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, desc.extent,
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        PrGpuGraphTexture target = prGpuGraphAddTexture(graph, &targetDesc);
        PrGpuGraphPassDesc passDesc{
            .name = "display",
            .viewCount = 1,
            .colorCount = 1,
            .color = {{.texture = target, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
        };
        PrGpuGraphPass pass = prGpuGraphAddPass(graph, &passDesc, [](void* PR_NULLABLE context, PrGpuPass* drawn) {
            const Shown& s = *static_cast<const Shown*>(context);
            prRenderDisplayDraw(s.render, drawn, prGpuGraphTexture(s.graph, s.color), &s.display);
        }, shownAs);
        prGpuGraphUseTexture(graph, pass, shownAs->color, PrGpuAccessFlag_FragmentRead);
        addRead(target, &image);
        record();
        return image;
    }
};

// Runs `test` on a fixture of its own for each of the shared devices, and checks that the device's validation found
// nothing wrong in it. Each fixture's renderer draws with `shaders`, or with the library's own set if that is null, and
// fills the set's own tables from `tables`.
template <typename Test>
void onEachDevice(Test test, const PrRenderShaderSet* PR_NULLABLE shaders = nullptr,
        Span<const PrRenderTableSource> tables = {}) {
    for (const TestDevice& device : testDevices()) {
        PrStringView backend = prBackendName(prGpuSelectInterface(), device.backend);
        CAPTURE(backend);
        CAPTURE(device.slotsOnly);
        uint32_t errors = validationErrors(device);
        {
            Fixture f(device, shaders, tables);
            test(f);
        }
        CHECK(validationErrors(device) == errors);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Scenes of quads

// Returns a transform that scales by `scale` and then moves by `offset`.
inline PrAffine3 placed(PrVec3 offset, PrVec3 scale = {1, 1, 1}) {
    return PrAffine3{{{scale.x, 0, 0, offset.x}, {0, scale.y, 0, offset.y}, {0, 0, scale.z, offset.z}}};
}

// Returns the transform that turns the quad's plane to face up: its normal, along z, to y, and its y to -z.
inline PrAffine3 facingUp(PrVec3 offset, float scale) {
    return PrAffine3{{{scale, 0, 0, offset.x}, {0, 0, scale, offset.y}, {0, -scale, 0, offset.z}}};
}

// What a vertex of the fixture's meshes holds: its geometry, and the attributes that a material reads of it, which the
// fixture converts into each layout of the renderer's shader set.
struct FixtureVertex {
    PrVec3 position;
    PrVec3 normal;
    PrVec3 tangent;
    float handedness = 1;
    PrVec2 uv0;
    PrVec2 uv1;
    PrColor color = {1, 1, 1, 1};
};

// Writes a stream of every layout of the renderer's shader set, each from the sources that match its attributes, into
// `bytes`, and the streams that name them into `streams`.
inline void fillStreams(Fixture& f, const PrRenderAttributeSource* sources, uint32_t sourceCount, uint32_t vertexCount,
        Array<Array<uint8_t>>* bytes, Array<PrRenderMeshStream>* streams) {
    const PrRenderShaderSet* set = prRenderShaderSet(f.render);
    bytes->resize(set->layoutCount);
    for (uint32_t i = 0; i < set->layoutCount; ++i) {
        const PrRenderVertexLayout* layout = &set->layouts[i];
        (*bytes)[i].resize(layout->stride * vertexCount);
        REQUIRE(prRenderVertexLayoutFill(layout, sources, sourceCount, vertexCount, (*bytes)[i].begin(), nullptr) ==
            PrStatus_Ok);
        streams->push(PrRenderMeshStream{layout, (*bytes)[i].begin()});
    }
}

// Sets a mesh's data to vertices and indices, with the parts that `parts` gives, and the attributes in every layout of
// the renderer's shader set, as a producer that knows nothing of its materials gives them.
inline void setMesh(Fixture& f, PrRenderMesh mesh, const Array<FixtureVertex>& vertices, const Array<uint32_t>& indices,
        const PrRenderMeshPart* parts, uint32_t partCount) {
    Array<PrRenderGeometryVertex> geometry;
    for (const FixtureVertex& v : vertices)
        geometry.push(prRenderGeometryVertexPack(v.position, v.normal, v.tangent, v.handedness));
    uint32_t stride = uint32_t(sizeof(FixtureVertex));
    const FixtureVertex* first = vertices.begin();
    const PrRenderAttributeSource sources[] = {
        {PrRenderSemantic_Uv, 0, {}, {&first->uv0, vertices.count(), stride, prMeshFormatFloat32(2)}},
        {PrRenderSemantic_Uv, 1, {}, {&first->uv1, vertices.count(), stride, prMeshFormatFloat32(2)}},
        {PrRenderSemantic_Color, 0, {}, {&first->color, vertices.count(), stride, prMeshFormatFloat32(4)}},
    };
    Array<Array<uint8_t>> bytes;
    Array<PrRenderMeshStream> streams;
    fillStreams(f, sources, 3, vertices.count(), &bytes, &streams);
    PrRenderMeshData data{
        .geometry = geometry.begin(),
        .vertexCount = vertices.count(),
        .streams = streams.begin(),
        .streamCount = streams.count(),
        .indices = indices.begin(),
        .indexCount = indices.count(),
        .parts = parts,
        .partCount = partCount,
    };
    REQUIRE(prRenderMeshSetData(f.render, mesh, &data) == PrStatus_Ok);
}

// Creates a square of side 2 about the origin in the plane z = 0, which faces along z, with its tangent along x. Its
// first texture coordinates run from (0, 0) at the top left to (1, 1) at the bottom right, and its second run the other
// way across. With `halves`, its left and right halves are two parts.
inline PrRenderMesh createQuad(Fixture& f, bool halves = false) {
    Array<FixtureVertex> vertices;
    Array<uint32_t> indices;
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
            FixtureVertex v;
            v.position = {float(column) - 1, 1 - float(row) * 2, 0};
            v.normal = {0, 0, 1};
            v.tangent = {1, 0, 0};
            v.uv0 = {float(column) * 0.5f, float(row)};
            v.uv1 = {1 - v.uv0.x, v.uv0.y};
            vertices.push(v);
        }
    }
    for (uint32_t column = 0; column < 2; ++column) {
        const uint32_t corners[6] = {column, column + 3, column + 4, column, column + 4, column + 1};
        for (uint32_t corner : corners)
            indices.push(corner);
    }
    PrRenderMeshPart parts[2] = {{.firstIndex = 0, .indexCount = 12}, {}};
    if (halves) {
        parts[0].indexCount = 6;
        parts[1] = {.firstIndex = 6, .indexCount = 6};
    }
    PrRenderMesh mesh = prRenderMeshCreate(f.render);
    setMesh(f, mesh, vertices, indices, parts, halves ? 2u : 1u);
    return mesh;
}

// Sets a material to a glTF material of the library's type that draws it, as a loader chooses: the layered type for a
// material with what only it draws, and the core type otherwise.
inline void setGltf(PrRender* render, PrRenderMaterial material, const PrRenderKhronosPbrMaterial* gltf) {
    if (prRenderKhronosPbrMaterialIsLayered(gltf))
        prRenderMaterialSetKhronosPbrLayered(render, material, gltf);
    else
        prRenderMaterialSetKhronosPbr(render, material, gltf);
}

// Create a material that is set: from a description, from a material of one of the library's types, and an unlit
// material of a color.
inline PrRenderMaterial createMaterial(Fixture& f, const PrRenderMaterialDesc& desc) {
    PrRenderMaterial material = prRenderMaterialCreate(f.render);
    prRenderMaterialSet(f.render, material, &desc);
    return material;
}
inline PrRenderMaterial createMaterial(Fixture& f, const PrRenderKhronosPbrMaterial& gltf) {
    PrRenderMaterial material = prRenderMaterialCreate(f.render);
    setGltf(f.render, material, &gltf);
    return material;
}
inline PrRenderMaterial createMaterial(Fixture& f, const PrRenderUnlitMaterial& unlit) {
    PrRenderMaterial material = prRenderMaterialCreate(f.render);
    prRenderMaterialSetUnlit(f.render, material, &unlit);
    return material;
}
inline PrRenderMaterial createUnlit(Fixture& f, PrColor color, PrRenderAlphaMode mode = PrRenderAlphaMode_Opaque) {
    PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
    unlit.color = color;
    unlit.alphaMode = mode;
    return createMaterial(f, unlit);
}
inline PrRenderMaterial createGltf(Fixture& f, const PrRenderKhronosPbrMaterial& material) {
    return createMaterial(f, material);
}

// How the texels of a texture of the fixture's encode their values: as they are, or as sRGB.
enum {
    TextureEncoding_Linear,
    TextureEncoding_Srgb,
};
typedef uint8_t TextureEncoding;

// Creates a texture of 8-bit RGBA texels. The fixture's uses of it sample it without filtering unless `filter` says
// otherwise (useOf).
inline PrRenderTexture createTexture(Fixture& f, PrUVec2 extent, const uint8_t* texels,
        TextureEncoding encoding = TextureEncoding_Linear, PrGpuFilter filter = PrGpuFilter_Nearest) {
    PrRenderTexture texture = prRenderTextureCreate(f.render);
    PrGpuFormat format = encoding == TextureEncoding_Srgb ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8;
    PrRenderTextureData data{.format = format, .extent = extent, .levelCount = 1, .levels = {{texels, 0}}};
    REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);
    f.samplers.set(texture.id, prGpuSamplerDescDefault(filter, PrGpuAddress_Repeat));
    return texture;
}

// Returns the use of a texture by the first set of coordinates, through the sampler that the fixture made the texture
// for: without filtering unless createTexture said otherwise, repeating, or the sampler that a test gave `samplers`. A
// texture that neither names is read through the library's default sampler, which filters.
inline PrRenderTextureUse useOf(Fixture& f, PrRenderTexture texture) {
    PrRenderTextureUse use = prRenderTextureUseDefault(texture);
    const PrGpuSamplerDesc* sampler = f.samplers.get(texture.id);
    if (sampler != nullptr)
        use.sampler = *sampler;
    return use;
}

// ---------------------------------------------------------------------------------------------------------------------
// The shading model's indirect light on the CPU

// The scale and the bias of the split-sum approximation.
struct SplitSum {
    double scale;
    double bias;
};

// Returns `value`, or the nearer of `low` and `high` if it lies outside them.
inline double clamped(double value, double low, double high) {
    if (value < low)
        return low;
    return value > high ? high : value;
}

// Returns the library's split sum, as pr/render_shading.slang's renderSplitSum reads it: the library's table as half
// floats, by the roughness and the square root of the cosine, interpolated between its four entries about them.
inline SplitSum referenceSplitSum(double roughness, double nv) {
    constexpr uint32_t kSide = 32;
    static float table[kSide * kSide * 4];
    static bool filled = false;
    if (!filled) {
        prRenderSplitSumTable(nullptr, PrUVec2{kSide, kSide}, table);
        filled = true;
    }
    double x = clamped(roughness, 0, 1) * (kSide - 1);
    double y = double(prF32Sqrt(float(clamped(nv, 0, 1)))) * (kSide - 1);
    uint32_t x0 = uint32_t(x) < kSide - 1 ? uint32_t(x) : kSide - 2;
    uint32_t y0 = uint32_t(y) < kSide - 1 ? uint32_t(y) : kSide - 2;
    double fx = x - x0;
    double fy = y - y0;
    double values[2];
    for (uint32_t channel = 0; channel < 2; ++channel) {
        auto at = [&](uint32_t column, uint32_t row) {
            return double(prFloat16ToFloat(prFloat16FromFloat(table[(row * kSide + column) * 4 + channel])));
        };
        double low = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
        double high = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
        values[channel] = low * (1 - fy) + high * fy;
    }
    return {values[0], values[1]};
}

// Returns the radiance that a gray surface of glTF's model leaves toward an eye at the cosine `nv` from its normal
// under an environment, as pr/render_brdf.slang's renderBrdfIndirect computes it. `radiance` is the environment's about
// the mirror direction, and `irradiance` is its diffuse light.
inline double referenceIndirect(double baseColor, double metallic, double roughness, double nv, double radiance,
        double irradiance) {
    SplitSum split = referenceSplitSum(roughness, nv);
    double lost = 1 - (split.scale + split.bias);
    double schlick = double(prF32Pow(float(1 - nv), 5));
    // The dielectric and the metal that the surface mixes: what each one's specular lobe returns after one bounce and
    // after more.
    double single[2];
    double multiple[2];
    const double f0s[2] = {0.04, baseColor};
    for (uint32_t i = 0; i < 2; ++i) {
        double f0 = f0s[i];
        double rise = (1 - roughness > f0 ? 1 - roughness : f0) - f0;
        double average = f0 + (1 - f0) / 21;
        single[i] = (f0 + rise * schlick) * split.scale + split.bias;
        multiple[i] = single[i] * average / (1 - average * lost) * lost;
    }
    double diffuse = baseColor * (1 - single[0] - multiple[0]);
    double mirrored = single[0] + (single[1] - single[0]) * metallic;
    double scattered = multiple[0] + diffuse + (multiple[1] - multiple[0] - diffuse) * metallic;
    return mirrored * radiance + scattered * irradiance;
}

// ---------------------------------------------------------------------------------------------------------------------
// Environments

// Returns the direction, not of unit length, through a point of a face of a cube, where `u` and `v` run from -1 to 1
// along the face's columns and rows, as lib/gpu lays out a cube's faces.
inline PrVec3 cubeDirection(uint32_t face, float u, float v) {
    switch (face) {
        case 0:
            return {1, -v, -u};
        case 1:
            return {-1, -v, u};
        case 2:
            return {u, 1, v};
        case 3:
            return {u, -1, -v};
        case 4:
            return {u, -v, 1};
        default:
            return {-u, -v, -1};
    }
}

// Returns the unit direction through the center of the texel (x, y) of a face of `size` texels.
inline PrVec3 cubeTexelDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size) {
    PrVec3 d = cubeDirection(face, (float(x) + 0.5f) / float(size) * 2 - 1, (float(y) + 0.5f) / float(size) * 2 - 1);
    return prVec3Normalize(d);
}

// Returns the solid angle of the texel (x, y) of a face of `size` texels, to the accuracy of its center's.
inline double cubeTexelSolidAngle(uint32_t x, uint32_t y, uint32_t size) {
    double u = (x + 0.5) / size * 2 - 1;
    double v = (y + 0.5) / size * 2 - 1;
    double length2 = 1 + u * u + v * v;
    return 4.0 / (double(size) * size) / (length2 * double(prF32Sqrt(float(length2))));
}

// The texels of one level of a cube: six faces of `size` by `size` texels of red, green and blue.
struct CubeTexels {
    uint32_t size = 0;
    Array<float> texels;

    float* at(uint32_t face, uint32_t x, uint32_t y) {
        return &texels[((face * size + y) * size + x) * 3];
    }
    const float* at(uint32_t face, uint32_t x, uint32_t y) const {
        return &texels[((face * size + y) * size + x) * 3];
    }
};

// Returns a cube of `size` whose texels hold `radiance` at their centers' directions.
template <typename Radiance>
CubeTexels cubeOf(uint32_t size, Radiance radiance) {
    CubeTexels cube;
    cube.size = size;
    cube.texels.resize(6 * size * size * 3);
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                PrVec3 value = radiance(cubeTexelDirection(face, x, y, size));
                float* texel = cube.at(face, x, y);
                texel[0] = value.x;
                texel[1] = value.y;
                texel[2] = value.z;
            }
        }
    }
    return cube;
}

// Creates an environment from a cube's texels.
inline PrRenderEnvironment createEnvironment(Fixture& f, const CubeTexels& cube) {
    PrRenderRadianceImage faces[6];
    size_t faceBytes = size_t(cube.size) * cube.size * 3 * sizeof(float);
    for (uint32_t face = 0; face < 6; ++face) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(cube.texels.begin()) + face * faceBytes;
        faces[face] = {.pixels = {data, size_t(cube.size) * 3 * sizeof(float), {cube.size, cube.size}}, .channels = 3};
    }
    PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
    REQUIRE(prRenderEnvironmentSetCube(f.render, environment, faces) == PrStatus_Ok);
    return environment;
}

// Creates an environment of one radiance from every direction.
inline PrRenderEnvironment createEvenEnvironment(Fixture& f, PrVec3 radiance) {
    return createEnvironment(f, cubeOf(1, [&](PrVec3) { return radiance; }));
}

// Makes `environment` the environment of a scene, turned by `rotation` and scaled by `intensity`.
inline void setEnvironment(Fixture& f, PrRenderScene scene, PrRenderEnvironment environment, float intensity = 1,
        PrAffine3 rotation = prAffine3Identity()) {
    PrRenderSceneEnvironment use = prRenderSceneEnvironmentDefault(environment);
    use.intensity = intensity;
    use.rotation = rotation;
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderSceneEnvironmentSet(update, &use);
    prRenderUpdateCommit(update);
}

// Returns the texels of one level of an environment's cube, read back from the device. The frame that began last shows
// the environment.
inline CubeTexels readEnvironment(Fixture& f, PrRenderEnvironment environment, uint32_t level) {
    PrRenderEnvironmentInfo info = prRenderEnvironmentInfo(f.render, environment);
    REQUIRE(info.texture.id != 0);
    REQUIRE(level < info.levels);
    CubeTexels cube;
    cube.size = info.faceSize >> level;
    uint32_t rowBytes = cube.size * 8;
    PrGpuBufferDesc desc{.size = uint64_t(rowBytes) * cube.size * 6, .memory = PrGpuMemory_Readback};
    PrGpuBuffer readback{};
    REQUIRE(prGpuBufferCreate(f.device, &desc, &readback) == PrStatus_Ok);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    PrGpuSubresources one{.firstLevel = uint8_t(level), .levels = 1};
    prGpuCommandsTextureBarrier(commands, info.texture, one, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
    PrGpuTextureRegion region{.level = uint8_t(level), .extent = {cube.size, cube.size, 6}};
    prGpuCommandsCopyTextureToBuffer(commands, info.texture, &region, readback, 0, rowBytes, rowBytes * cube.size);
    prGpuCommandsTextureBarrier(commands, info.texture, one, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ShaderRead);
    gpuSubmitAndWait(commands);
    const uint8_t* bytes = static_cast<const uint8_t*>(prGpuBufferMap(f.device, readback));
    cube.texels.resize(6 * cube.size * cube.size * 3);
    for (uint32_t i = 0; i < 6 * cube.size * cube.size; ++i) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            PrFloat16 half;
            prMemCopy(&half, bytes + size_t(i) * 8 + channel * 2, 2);
            cube.texels[i * 3 + channel] = prFloat16ToFloat(half);
        }
    }
    prGpuBufferRelease(f.device, readback);
    return cube;
}

// Creates a sphere of radius 1 about the origin, of `rings` bands of quads from pole to pole. Its normals are the
// sphere's own at each vertex.
inline PrRenderMesh createSphere(Fixture& f, uint32_t rings = 48) {
    Array<FixtureVertex> vertices;
    Array<uint32_t> indices;
    uint32_t segments = rings * 2;
    for (uint32_t ring = 0; ring <= rings; ++ring) {
        float polar = 3.14159265f * float(ring) / float(rings);
        for (uint32_t segment = 0; segment <= segments; ++segment) {
            float around = 2 * 3.14159265f * float(segment) / float(segments);
            FixtureVertex v;
            v.position = {prF32Sin(polar) * prF32Cos(around), prF32Cos(polar), prF32Sin(polar) * prF32Sin(around)};
            v.normal = v.position;
            v.uv0 = {float(segment) / float(segments), float(ring) / float(rings)};
            vertices.push(v);
        }
    }
    for (uint32_t ring = 0; ring < rings; ++ring) {
        for (uint32_t segment = 0; segment < segments; ++segment) {
            uint32_t a = ring * (segments + 1) + segment;
            uint32_t b = a + segments + 1;
            // Counterclockwise seen from outside the sphere.
            const uint32_t corners[6] = {a, a + 1, b, a + 1, b + 1, b};
            for (uint32_t corner : corners)
                indices.push(corner);
        }
    }
    PrRenderMeshPart part{.firstIndex = 0, .indexCount = indices.count()};
    PrRenderMesh mesh = prRenderMeshCreate(f.render);
    setMesh(f, mesh, vertices, indices, &part, 1);
    return mesh;
}

// Adds an instance of `mesh` with one material to an update.
inline PrRenderInstance addInstance(PrRenderUpdate* update, PrRenderMesh mesh, PrRenderMaterial material,
        const PrAffine3& world) {
    PrRenderInstanceDesc desc = prRenderInstanceDescDefault(mesh);
    desc.materials = &material;
    desc.materialCount = 1;
    desc.world = world;
    return prRenderInstanceAdd(update, &desc);
}

// The distance from which the tests' camera looks at the origin.
constexpr float kCameraDistance = 4;

// Returns a view of `scene` at kExtent with one sample, from a camera on the z axis that looks at the origin. The
// square of side 2 about the origin in the plane z = 0 exactly fills the view, and the pixel at kCenter shows the
// origin.
inline PrRenderViewDesc frontView(PrRenderScene scene) {
    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = prRenderCameraLookAt({0, 0, kCameraDistance}, {0, 0, 0}, 2 * prF32Atan(1 / kCameraDistance), 1, 0.1f);
    view.extent = kExtent;
    view.samples = 1;
    return view;
}

// Returns the pixel of a view that shows a point of the world.
inline PrUVec2 pixelOf(const PrRenderViewDesc& view, PrVec3 point) {
    PrMatrix4 m = prMatrix4Concat(&view.camera.view, &view.camera.projection);
    float clip[4];
    for (uint32_t row = 0; row < 4; ++row)
        clip[row] = m.m[row][0] * point.x + m.m[row][1] * point.y + m.m[row][2] * point.z + m.m[row][3];
    float x = (clip[0] / clip[3] * 0.5f + 0.5f) * float(view.extent.x);
    float y = (0.5f - clip[1] / clip[3] * 0.5f) * float(view.extent.y);
    return PrUVec2{uint32_t(x), uint32_t(y)};
}

} // namespace harness

PR_ASSUME_NONNULL_END
