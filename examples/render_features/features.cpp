#include "examples/render_features/features.h"

#include "pr/color.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/mesh.h"
#include "pr/render_shaders.h"
#include "pr/vec.h"

#include "examples/render_features/features_materials.h"
#include "render_shaders/RenderFeaturesShaders.h"

namespace render_features {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// The direction toward the sun of the pages that have one.
constexpr PrVec3 kSunDirection{0.45f, 0.75f, 0.5f};

// ---------------------------------------------------------------------------------------------------------------------
// The stage: what the pages build their worlds from

// A world while a page fills it: the renderer, the scene's open update, and the meshes and textures that pages share.
struct Stage {
    PrRender* render;
    World* world;
    PrRenderUpdate* update;
    PrRenderMesh sphere;        // of radius 0.5 about the origin
    PrRenderMesh slab;          // a cube of side 1 about the origin
    PrRenderMesh quad;          // of side 1 about the origin in the plane z = 0, facing +z
    PrRenderTexture dimples;    // a normal map of bumps
    PrRenderTexture checker;    // a checker of two grays, sRGB
};

// A mesh's vertices as the pages make them: the geometry, and the attributes of the vertices that the shader set's
// layouts are filled from.
struct Vertex {
    PrVec2 uv0;
    PrVec2 uv1;
};

struct Geometry {
    Array<PrRenderGeometryVertex> geometry;
    Array<Vertex> vertices;
    Array<uint32_t> indices;
    Array<PrRenderSkinVertex> skin;     // the skin of each vertex, or none
    uint32_t jointCount = 0;
    Array<PrRenderMorphDelta> morphs;   // each vertex's delta of each target, or none
    uint32_t morphTargetCount = 0;
};

void pushVertex(Geometry* g, PrVec3 position, PrVec3 normal, PrVec3 tangent, PrVec2 uv) {
    g->geometry.push(prRenderGeometryVertexPack(position, normal, tangent, 1));
    // The second set of coordinates runs the other way, so that a texture that reads it shows which set it reads.
    g->vertices.push(Vertex{uv, {1 - uv.x, uv.y}});
}

PrRenderMesh addMesh(Stage& s, const Geometry& g, float radius) {
    // The mesh has the attributes of every layout of the set's types, converted from the vertices' own; a layout's
    // attribute that the vertices lack, as the color, is its fallback.
    const PrRenderShaderSet* set = prRenderShaderSet(s.render);
    uint32_t count = g.vertices.count();
    uint32_t stride = uint32_t(sizeof(Vertex));
    const PrRenderAttributeSource sources[] = {
        {PrRenderSemantic_Uv, 0, {}, {&g.vertices.begin()->uv0, count, stride, prMeshFormatFloat32(2)}},
        {PrRenderSemantic_Uv, 1, {}, {&g.vertices.begin()->uv1, count, stride, prMeshFormatFloat32(2)}},
    };
    Array<Array<uint8_t>> bytes;
    Array<PrRenderMeshStream> streams;
    for (uint32_t i = 0; i < set->layoutCount; ++i) {
        const PrRenderVertexLayout* layout = &set->layouts[i];
        bytes.push(Array<uint8_t>(size_t(layout->stride) * count));
        prRenderVertexLayoutFill(layout, sources, 2, count, bytes[i].begin(), nullptr);
        streams.push(PrRenderMeshStream{layout, bytes[i].begin()});
    }
    PrRenderMeshPart part{.firstIndex = 0, .indexCount = g.indices.count(), .bounds = {.radius = radius}};
    PrRenderMeshData data{
        .geometry = g.geometry.begin(),
        .vertexCount = count,
        .streams = streams.begin(),
        .streamCount = streams.count(),
        .indices = g.indices.begin(),
        .indexCount = g.indices.count(),
        .parts = &part,
        .partCount = 1,
        .skin = g.skin.isEmpty() ? nullptr : g.skin.begin(),
        .jointCount = g.jointCount,
        .morphs = g.morphs.isEmpty() ? nullptr : g.morphs.begin(),
        .morphTargetCount = g.morphTargetCount,
    };
    PrRenderMesh mesh = prRenderMeshCreate(s.render);
    prRenderMeshSetData(s.render, mesh, &data);
    s.world->meshes.push(mesh);
    return mesh;
}

// The sphere's rings of latitude and its segments of longitude.
constexpr uint32_t kSphereRings = 48;
constexpr uint32_t kSphereSegments = 96;

// Returns the unit normal of the sphere's vertex at a ring, from its top, and a segment.
PrVec3 sphereNormal(uint32_t ring, uint32_t segment) {
    float polar = kPi * float(ring) / kSphereRings;
    float around = 2 * kPi * float(segment) / kSphereSegments;
    return PrVec3{prF32Sin(polar) * prF32Cos(around), prF32Cos(polar), prF32Sin(polar) * prF32Sin(around)};
}

// A sphere of radius 0.5 about the origin, as rings of latitude from its top, with tangents along its parallels.
Geometry sphereGeometry() {
    Geometry g;
    for (uint32_t ring = 0; ring <= kSphereRings; ++ring) {
        for (uint32_t segment = 0; segment <= kSphereSegments; ++segment) {
            float around = 2 * kPi * float(segment) / kSphereSegments;
            PrVec3 n = sphereNormal(ring, segment);
            PrVec3 tangent{-prF32Sin(around), 0, prF32Cos(around)};
            pushVertex(&g, n * 0.5f, n, tangent, {float(segment) / kSphereSegments, float(ring) / kSphereRings});
        }
    }
    for (uint32_t ring = 0; ring < kSphereRings; ++ring) {
        for (uint32_t segment = 0; segment < kSphereSegments; ++segment) {
            uint32_t a = ring * (kSphereSegments + 1) + segment;
            uint32_t d = a + kSphereSegments + 1;
            const uint32_t corners[6] = {a, a + 1, d + 1, a, d + 1, d};
            for (uint32_t index : corners)
                g.indices.push(index);
        }
    }
    return g;
}

PrRenderMesh addSphere(Stage& s) {
    return addMesh(s, sphereGeometry(), 0.5f);
}

// The faces of a cube of side 1 about the origin, the first `faces` of them: the one that faces +z first.
PrRenderMesh addFaces(Stage& s, uint32_t faces) {
    static const PrVec3 kFaces[6][2] = {
        {{0, 0, 1}, {1, 0, 0}},  {{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}},
        {{0, 1, 0}, {0, 0, 1}},  {{0, -1, 0}, {1, 0, 0}}, {{0, 0, -1}, {0, 1, 0}},
    };
    Geometry g;
    for (uint32_t face = 0; face < faces; ++face) {
        PrVec3 n = kFaces[face][0];
        PrVec3 u = kFaces[face][1];
        PrVec3 v = prVec3Cross(n, u);
        // One face alone lies in the plane through the origin.
        PrVec3 center = faces == 1 ? PrVec3{} : n * 0.5f;
        uint32_t first = g.vertices.count();
        pushVertex(&g, center + (u * -1.0f - v) * 0.5f, n, u, {0, 1});
        pushVertex(&g, center + (u - v) * 0.5f, n, u, {1, 1});
        pushVertex(&g, center + (u + v) * 0.5f, n, u, {1, 0});
        pushVertex(&g, center + (v - u) * 0.5f, n, u, {0, 0});
        const uint32_t corners[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t corner : corners)
            g.indices.push(first + corner);
    }
    return addMesh(s, g, prF32Sqrt(0.75f));
}

// Creates a texture of 8-bit RGBA texels, `side` by `side`, whose texel (x, y) is `texel(x, y)`, which a shader reads
// as color encoded as sRGB if `srgb`, and as they are stored otherwise.
template <typename Texel>
PrRenderTexture addTexture(Stage& s, uint32_t side, bool srgb, Texel texel) {
    Array<uint8_t> texels(size_t(side) * side * 4);
    for (uint32_t y = 0; y < side; ++y) {
        for (uint32_t x = 0; x < side; ++x) {
            PrColor value = texel(x, y);
            uint8_t* to = texels.begin() + (size_t(y) * side + x) * 4;
            to[0] = uint8_t(prF32Clamp(value.r, 0, 1) * 255 + 0.5f);
            to[1] = uint8_t(prF32Clamp(value.g, 0, 1) * 255 + 0.5f);
            to[2] = uint8_t(prF32Clamp(value.b, 0, 1) * 255 + 0.5f);
            to[3] = uint8_t(prF32Clamp(value.a, 0, 1) * 255 + 0.5f);
        }
    }
    PrRenderTexture texture = prRenderTextureCreate(s.render);
    PrRenderTextureData data{
        .format = PrGpuFormat(srgb ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8),
        .extent = {side, side},
        .levelCount = 1,
        .levels = {{texels.begin(), 0}},
    };
    prRenderTextureSetData(s.render, texture, &data);
    s.world->textures.push(texture);
    return texture;
}

PrRenderMaterial addGltf(Stage& s, const PrRenderKhronosPbrMaterial& material) {
    PrRenderMaterial id = prRenderMaterialCreate(s.render);
    prRenderMaterialSetKhronosPbr(s.render, id, &material);
    s.world->materials.push(id);
    return id;
}

// An OpenPBR material with its textures by their indices, or without any.
PrRenderMaterial addOpenPbr(Stage& s, const PrRenderParameterRecord& parameters,
        const PrRenderTextureUse* PR_NULLABLE textures = nullptr, PrRenderAlphaMode mode = PrRenderAlphaMode_Opaque) {
    PrRenderMaterialDesc desc =
        prRenderMaterialDescDefault(kRenderFeaturesMaterialOpenPbr, parameters.bytes, parameters.type->parameterBytes);
    desc.alphaMode = mode;
    if (parameters.value("transmissionWeight") > 0)
        desc.flags |= PrRenderMaterialFlag_ReadsBehind;
    if (parameters.value("geometryThinWalled") != 0)
        desc.flags |= PrRenderMaterialFlag_DoubleSided;
    desc.textures = textures;
    desc.textureCount = textures != nullptr ? kRenderFeaturesOpenPbrTextureCount : 0;
    PrRenderMaterial id = prRenderMaterialCreate(s.render);
    prRenderMaterialSet(s.render, id, &desc);
    s.world->materials.push(id);
    return id;
}

PrAffine3 placed(PrVec3 position, PrVec3 scale = {1, 1, 1}) {
    return PrAffine3{{{scale.x, 0, 0, position.x}, {0, scale.y, 0, position.y}, {0, 0, scale.z, position.z}}};
}

PrRenderInstance addInstance(Stage& s, PrRenderMesh mesh, PrRenderMaterial material, const PrAffine3& world,
        PrRenderMobilityFlags mobility = PrRenderMobilityFlag_Static) {
    PrRenderInstanceDesc desc = prRenderInstanceDescDefault(mesh);
    desc.materials = &material;
    desc.materialCount = 1;
    desc.world = world;
    desc.mobility = mobility;
    return prRenderInstanceAdd(s.update, &desc);
}

// Adds an instance that the page moves with time.
void addMoving(Stage& s, PrRenderMesh mesh, PrRenderMaterial material, const PrAffine3& world) {
    s.world->moving.push(addInstance(s, mesh, material, world, PrRenderMobilityFlag_Dynamic));
}

// A material of a type of the example's own shader set, of its parameter record.
template <typename Parameters>
PrRenderMaterial addCustom(Stage& s, uint32_t type, const Parameters& parameters) {
    PrRenderMaterialDesc desc = prRenderMaterialDescDefault(type, &parameters, sizeof(parameters));
    PrRenderMaterial id = prRenderMaterialCreate(s.render);
    prRenderMaterialSet(s.render, id, &desc);
    s.world->materials.push(id);
    return id;
}

// The ground: a wide slab under the origin with a checker, which a surface that transmits shows bent.
void addGround(Stage& s) {
    PrRenderKhronosPbrMaterial ground = prRenderKhronosPbrMaterialDefault();
    ground.metallic = 0;
    ground.roughness = 0.8f;
    ground.baseColorTexture = prRenderTextureUseDefault(s.checker);
    ground.baseColorTexture.transform = prAffine2Scale({20, 20});
    addInstance(s, s.slab, addGltf(s, ground), placed({0, -0.25f, 0}, {40, 0.5f, 40}));
}

// A clear day: the sky as the scene's environment, and its sun toward `sunDirection` as a light that casts shadows,
// whose source has the angular radius `sunRadius`.
void addDaylight(Stage& s, float sunRadius = kPrRenderSunAngularRadius, PrVec3 sunDirection = kSunDirection) {
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = sunDirection;
    sky.sunIrradiance = 4.5f;
    sky.groundAlbedo = {0.25f, 0.24f, 0.22f};
    s.world->environment = prRenderEnvironmentCreate(s.render);
    prRenderEnvironmentSetSky(s.render, s.world->environment, &sky);
    PrRenderSceneEnvironment environment = prRenderSceneEnvironmentDefault(s.world->environment);
    prRenderSceneEnvironmentSet(s.update, &environment);
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.color = prRenderSkySunIrradiance(&sky);
    sun.direction = -sunDirection;
    sun.castsShadows = true;
    sun.shadowBounds.radius = 7;
    sun.angularRadius = sunRadius;
    prRenderLightAdd(s.update, &sun);
}

// A grid of spheres over the ground under daylight: `cell` returns the material of the sphere at a row and a column,
// with the column's place along its row from 0 to 1. The rows run from the back of the world to its front.
template <typename Cell>
void addGrid(Stage& s, uint32_t rows, uint32_t columns, Cell cell) {
    addGround(s);
    addDaylight(s);
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t column = 0; column < columns; ++column) {
            float along = columns > 1 ? float(column) / float(columns - 1) : 0;
            PrVec3 position{
                (float(column) - 0.5f * float(columns - 1)) * 1.3f,
                0.5f,
                (float(row) - 0.5f * float(rows - 1)) * 1.5f,
            };
            addInstance(s, s.sphere, cell(row, column, along), placed(position));
        }
    }
    s.world->distance = 3.2f + 0.75f * float(columns > rows * 2 ? columns : rows * 2);
}

PrRenderKhronosPbrMaterial gltfOf(PrColor color, float metallic, float roughness) {
    PrRenderKhronosPbrMaterial m = prRenderKhronosPbrMaterialDefault();
    m.baseColor = color;
    m.metallic = metallic;
    m.roughness = roughness;
    return m;
}

PrRenderParameterRecord openPbrOf(PrVec3 color, float metalness, float roughness) {
    PrRenderParameterRecord p(&renderFeaturesShaderSet()->materialTypes[kRenderFeaturesMaterialOpenPbr]);
    p.set("baseColor", {color.x, color.y, color.z}).set("baseMetalness", {metalness});
    p.set("specularRoughness", {roughness});
    return p;
}

constexpr PrColor kRed{0.7f, 0.05f, 0.04f, 1};
constexpr PrColor kBlue{0.05f, 0.12f, 0.5f, 1};
constexpr PrColor kBlack{0.01f, 0.01f, 0.01f, 1};
constexpr PrColor kWhite{0.9f, 0.9f, 0.9f, 1};
constexpr PrColor kGold{1.0f, 0.77f, 0.34f, 1};

// ---------------------------------------------------------------------------------------------------------------------
// The Khronos PBR model: glTF's core material and each of its extensions

void gltfCore(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        return addGltf(s, gltfOf(row == 0 ? kRed : kGold, float(row), along));
    });
}

void gltfIor(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t column, float) {
        const float iors[6] = {1.0f, 1.33f, 1.5f, 2.0f, 2.42f, 0};
        PrRenderKhronosPbrMaterial m = gltfOf(kBlack, 0, row == 0 ? 0.05f : 0.4f);
        m.ior = iors[column];
        return addGltf(s, m);
    });
}

void gltfSpecular(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(row == 0 ? kRed : kBlack, 0, 0.2f);
        if (row == 0)
            m.specular = along;
        else
            m.specularColor = {1, 1 - along, along};
        return addGltf(s, m);
    });
}

void gltfClearcoat(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t column, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(kRed, 0, 0.6f);
        m.clearcoat = row == 0 ? along : 1.0f;
        m.clearcoatRoughness = row == 1 ? along : 0.0f;
        if (row == 2) {
            // A bumped base under a smooth coat, a coat with bumps of its own over a smooth base, and both.
            m.clearcoat = column < 3 ? 1.0f : 0.0f;
            PrRenderTextureUse bumps = prRenderTextureUseDefault(s.dimples);
            bumps.transform = prAffine2Scale({6, 3});
            if (column % 3 != 1)
                m.normalTexture = bumps;
            if (column % 3 != 0)
                m.clearcoatNormalTexture = bumps;
        }
        return addGltf(s, m);
    });
}

void gltfSheen(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(kBlue, 0, 0.8f);
        m.sheenColor = row == 0 ? PrVec3{1, 1, 1} : PrVec3{along, along * 0.6f, along * 0.3f};
        m.sheenRoughness = row == 0 ? 0.05f + 0.95f * along : 0.5f;
        return addGltf(s, m);
    });
}

void gltfIridescence(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(row == 1 ? kWhite : kBlack, row == 1 ? 1.0f : 0.0f, 0.15f);
        m.iridescence = row == 2 ? along : 1.0f;
        m.iridescenceThicknessMaximum = row == 2 ? 400.0f : 100 + 700 * along;
        return addGltf(s, m);
    });
}

void gltfAnisotropy(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(kGold, 1, 0.3f);
        m.anisotropy = row == 0 ? along : 0.9f;
        m.anisotropyRotation = row == 0 ? 0.0f : along * kPi;
        return addGltf(s, m);
    });
}

void gltfTransmission(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf({0.9f, 0.95f, 1, 1}, 0, row == 0 ? 0.0f : along);
        m.transmission = row == 0 ? along : 1.0f;
        return addGltf(s, m);
    });
}

void gltfVolume(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf({1, 1, 1, 1}, 0, 0);
        m.transmission = 1;
        m.thickness = row == 0 ? 1.5f * along : 1.0f;
        m.attenuationColor = {0.2f, 0.7f, 0.4f};
        m.attenuationDistance = row == 0 ? 1.0f : 0.1f + 3 * along;
        return addGltf(s, m);
    });
}

void gltfDispersion(Stage& s) {
    addGrid(s, 1, 6, [&](uint32_t, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf({1, 1, 1, 1}, 0, 0);
        m.transmission = 1;
        m.thickness = 1;
        m.ior = 1.6f;
        m.dispersion = 8 * along;
        return addGltf(s, m);
    });
}

void gltfDiffuseTransmission(Stage& s) {
    addGround(s);
    // Thin sheets that stand against a low sun behind them, so that the light that passes through them reaches the eye.
    addDaylight(s, kPrRenderSunAngularRadius, PrVec3{0.3f, 0.45f, -0.85f});
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t column = 0; column < 6; ++column) {
            float along = float(column) / 5;
            PrRenderKhronosPbrMaterial m = gltfOf({0.3f, 0.6f, 0.2f, 1}, 0, 0.7f);
            m.flags |= PrRenderMaterialFlag_DoubleSided;
            m.diffuseTransmission = row == 0 ? along : 1.0f;
            m.diffuseTransmissionColor = row == 0 ? PrVec3{0.5f, 0.9f, 0.2f} : PrVec3{1, 1 - along, along};
            PrVec3 position{(float(column) - 2.5f) * 1.3f, 0.5f, (float(row) - 0.5f) * 1.5f};
            addInstance(s, s.quad, addGltf(s, m), placed(position));
        }
    }
    s.world->distance = 6.5f;
}

void gltfEmissive(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t column, float along) {
        if (row == 1) {
            // KHR_materials_unlit: the color alone, which no light changes.
            PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
            unlit.color = {along, 0.5f, 1 - along, 1};
            PrRenderMaterial id = prRenderMaterialCreate(s.render);
            prRenderMaterialSetUnlit(s.render, id, &unlit);
            s.world->materials.push(id);
            return id;
        }
        PrRenderKhronosPbrMaterial m = gltfOf(kBlack, 0, 0.5f);
        m.emissive = {1, 0.45f, 0.1f};
        m.emissiveStrength = float(1u << column) * 0.25f;
        return addGltf(s, m);
    });
}

void gltfSpecularGlossiness(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderKhronosPbrMaterial m = gltfOf(row == 0 ? kRed : kBlack, 0, 0);
        m.specularGlossiness = true;
        m.specularColor = row == 0 ? PrVec3{0.04f, 0.04f, 0.04f} : PrVec3{1.0f, 0.77f * along, 0.34f * along};
        m.glossiness = row == 0 ? along : 0.7f;
        return addGltf(s, m);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// OpenPBR Surface, group by group

void openPbrBase(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderParameterRecord p = openPbrOf({0.7f, 0.05f, 0.04f}, 0, 0.3f);
        if (row == 0) {
            p.set("specularRoughness", {along});
        } else if (row == 1) {
            p = openPbrOf({1.0f, 0.77f, 0.34f}, 1, along);
        } else {
            // The rough diffuse lobe alone, without the specular layer.
            p.set("specularWeight", {0});
            p.set("baseDiffuseRoughness", {along});
        }
        return addOpenPbr(s, p);
    });
}

void openPbrSpecular(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderParameterRecord p = openPbrOf({0.01f, 0.01f, 0.01f}, 0, 0.15f);
        if (row == 0) {
            p.set("specularIor", {1 + 1.5f * along});
        } else if (row == 1) {
            p.set("specularColor", {1, 1 - along, along}).set("specularWeight", {0.25f + 0.75f * along});
        } else {
            p = openPbrOf({1.0f, 0.77f, 0.34f}, 1, 0.35f);
            p.set("specularRoughnessAnisotropy", {along});
            p.set("geometryTangentRotation", {along * kPi * 0.5f});
        }
        return addOpenPbr(s, p);
    });
}

void openPbrCoat(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t column, float along) {
        PrRenderParameterRecord p = openPbrOf({0.7f, 0.05f, 0.04f}, 0, 0.6f);
        p.set("coatWeight", {1});
        PrRenderTextureUse textures[kRenderFeaturesOpenPbrTextureCount];
        for (PrRenderTextureUse& use : textures)
            use = prRenderTextureUseDefault(PrRenderTexture{});
        if (row == 0) {
            p.set("coatWeight", {along});
        } else if (row == 1) {
            // The coat's roughness, and then its color and its darkening of the base.
            p.set("coatRoughness", {column < 3 ? float(column) * 0.3f : 0.0f});
            if (column >= 3)
                p.set("coatColor", {1, 0.75f, 0.3f}).set("coatWeight", {1});
            p.set("coatDarkening", {column == 5 ? 0.0f : 1.0f});
        } else {
            // The coat's anisotropy, and then the base's bumps under a smooth coat and the coat's own bumps.
            p.set("coatRoughness", {0.3f});
            p.set("coatRoughnessAnisotropy", {column < 3 ? float(column) * 0.45f : 0.0f});
            PrRenderTextureUse bumps = prRenderTextureUseDefault(s.dimples);
            bumps.transform = prAffine2Scale({6, 3});
            if (column == 3 || column == 5)
                textures[kRenderFeaturesOpenPbrTextureGeometryNormal] = bumps;
            if (column >= 4)
                textures[kRenderFeaturesOpenPbrTextureGeometryCoatNormal] = bumps;
        }
        return addOpenPbr(s, p, textures);
    });
}

void openPbrFuzz(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderParameterRecord p = openPbrOf({0.05f, 0.12f, 0.5f}, 0, 0.7f);
        p.set("fuzzWeight", {row == 0 ? along : 1.0f});
        p.set("fuzzRoughness", {row == 0 ? 0.5f : 0.05f + 0.95f * along});
        return addOpenPbr(s, p);
    });
}

void openPbrThinFilm(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t, float along) {
        PrRenderParameterRecord p =
            row == 0 ? openPbrOf({0.01f, 0.01f, 0.01f}, 0, 0.15f) : openPbrOf({0.92f, 0.92f, 0.92f}, 1, 0.2f);
        p.set("thinFilmWeight", {1});
        p.set("thinFilmThickness", {0.1f + 0.7f * along});
        return addOpenPbr(s, p);
    });
}

void openPbrTransmission(Stage& s) {
    addGrid(s, 3, 6, [&](uint32_t row, uint32_t column, float along) {
        PrRenderParameterRecord p = openPbrOf({0.8f, 0.8f, 0.8f}, 0, 0);
        p.set("transmissionWeight", {1});
        p.set("thickness", {1});
        if (row == 0) {
            p.set("transmissionWeight", {along});
        } else if (row == 1) {
            // A medium that absorbs over a shorter and shorter depth, and then one that disperses.
            p.set("transmissionColor", {0.2f, 0.7f, 0.4f}).set("transmissionWeight", {1});
            p.set("transmissionDepth", {column < 4 ? 2.0f / float(1u << column) : 0.0f});
            if (column >= 4) {
                p.set("transmissionColor", {1, 1, 1}).set("transmissionWeight", {1});
                p.set("transmissionDispersionScale", {column == 4 ? 1.0f : 3.0f});
            }
        } else {
            // A rougher and rougher interface, and then a thin wall, which does not bend what it shows.
            p.set("specularRoughness", {column < 4 ? float(column) * 0.2f : 0.0f});
            p.set("geometryThinWalled", {column >= 4 ? 1.0f : 0.0f});
            if (column == 5)
                p.set("transmissionColor", {0.9f, 0.4f, 0.3f}).set("transmissionWeight", {1});
        }
        return addOpenPbr(s, p);
    });
}

void openPbrSubsurface(Stage& s) {
    addGround(s);
    addDaylight(s);
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t column = 0; column < 6; ++column) {
            float along = float(column) / 5;
            PrRenderParameterRecord p = openPbrOf({0.8f, 0.8f, 0.8f}, 0, 0.5f);
            p.set("subsurfaceColor", {0.9f, 0.45f, 0.35f}).set("subsurfaceWeight", {along});
            // The front row is thin walls against the sun, which pass half of the subsurface's light.
            p.set("geometryThinWalled", {row == 1 ? 1.0f : 0.0f});
            PrVec3 position{(float(column) - 2.5f) * 1.3f, 0.5f, (float(row) - 0.5f) * 1.5f};
            addInstance(s, row == 1 ? s.quad : s.sphere, addOpenPbr(s, p), placed(position));
        }
    }
    s.world->distance = 6.5f;
}

void openPbrEmission(Stage& s) {
    addGrid(s, 2, 6, [&](uint32_t row, uint32_t column, float along) {
        PrRenderParameterRecord p = openPbrOf({0.05f, 0.05f, 0.05f}, 0, 0.4f);
        if (row == 0) {
            // Emission, bare and then under a coat, which tints it and dims it toward the sphere's edge.
            p.set("emissionColor", {1, 0.45f, 0.1f}).set("emissionLuminance", {0.3f * float(1u << (column % 3))});
            p.set("coatColor", {1, 0.9f, 0.6f}).set("coatWeight", {column >= 3 ? 1.0f : 0.0f});
            return addOpenPbr(s, p);
        }
        p = openPbrOf({0.9f, 0.9f, 0.95f}, 0, 0.05f);
        p.set("geometryOpacity", {1 - 0.8f * along});
        return addOpenPbr(s, p, nullptr, PrRenderAlphaMode_Blend);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The renderer

// A texture whose alpha falls from its left edge to its right, over stripes of color.
PrRenderTexture addFade(Stage& s) {
    return addTexture(s, 64, true, [](uint32_t x, uint32_t y) {
        float alpha = 1 - (float(x) + 0.5f) / 64;
        return (y / 8) % 2 == 0 ? PrColor{0.9f, 0.3f, 0.1f, alpha} : PrColor{0.1f, 0.4f, 0.9f, alpha};
    });
}

void alphaModes(Stage& s) {
    addGround(s);
    addDaylight(s);
    PrRenderTexture fade = addFade(s);
    // Opaque, masked at three cutoffs, and blended, single sided and then double sided.
    const PrRenderAlphaMode modes[6] = {
        PrRenderAlphaMode_Opaque, PrRenderAlphaMode_Mask, PrRenderAlphaMode_Mask,
        PrRenderAlphaMode_Mask, PrRenderAlphaMode_Blend, PrRenderAlphaMode_Blend,
    };
    const float cutoffs[6] = {0.5f, 0.25f, 0.5f, 0.75f, 0.5f, 0.5f};
    for (uint32_t column = 0; column < 6; ++column) {
        PrRenderKhronosPbrMaterial m = gltfOf({1, 1, 1, 1}, 0, 0.5f);
        m.baseColorTexture = prRenderTextureUseDefault(fade);
        m.alphaMode = modes[column];
        m.alphaCutoff = cutoffs[column];
        m.flags = column == 5 ? PrRenderMaterialFlag_DoubleSided : 0;
        addInstance(s, s.slab, addGltf(s, m), placed({(float(column) - 2.5f) * 1.3f, 0.5f, 0}));
    }
    s.world->distance = 6.5f;
}

void textureTransforms(Stage& s) {
    addGround(s);
    addDaylight(s);
    // A texture that shows which way is up and which is right: a red corner at its start and a green bar along x.
    PrRenderTexture arrow = addTexture(s, 64, true, [](uint32_t x, uint32_t y) {
        if (x < 16 && y < 16)
            return PrColor{0.9f, 0.1f, 0.1f, 1};
        if (y >= 28 && y < 36)
            return PrColor{0.1f, 0.7f, 0.2f, 1};
        return ((x / 8) + (y / 8)) % 2 == 0 ? PrColor{0.85f, 0.85f, 0.85f, 1} : PrColor{0.55f, 0.55f, 0.6f, 1};
    });
    // As it is, moved, turned, scaled, all three, and through the second set of coordinates, which runs the other way.
    for (uint32_t column = 0; column < 6; ++column) {
        PrRenderKhronosPbrMaterial m = gltfOf({1, 1, 1, 1}, 0, 0.6f);
        PrRenderTextureUse use = prRenderTextureUseDefault(arrow);
        PrVec2 offset = column == 1 || column == 4 ? PrVec2{0.25f, 0.25f} : PrVec2{0, 0};
        float rotation = column == 2 || column == 4 ? kPi / 6 : 0;
        PrVec2 scale = column == 3 || column == 4 ? PrVec2{2, 2} : PrVec2{1, 1};
        use.transform = prRenderTextureTransform(offset, rotation, scale);
        if (column == 5)
            use.texCoord = 1;
        m.baseColorTexture = use;
        addInstance(s, s.quad, addGltf(s, m), placed({(float(column) - 2.5f) * 1.3f, 0.6f, 0}));
    }
    s.world->distance = 6.5f;
}

// Spheres on the ground for the pages that light them.
void addSubjects(Stage& s) {
    addGround(s);
    for (uint32_t i = 0; i < 5; ++i) {
        PrRenderKhronosPbrMaterial m = gltfOf(kWhite, i % 2 == 0 ? 0.0f : 1.0f, 0.15f + 0.15f * float(i));
        addInstance(s, s.sphere, addGltf(s, m), placed({(float(i) - 2) * 1.6f, 0.5f, 0}));
    }
    s.world->distance = 7.5f;
}

void lights(Stage& s) {
    addSubjects(s);
    // A point light with a range and one without, a spot with a narrow cone and one with a wide one, and a dim
    // directional light, in the units of glTF's KHR_lights_punctual.
    PrRenderLightDesc light = prRenderLightDescDefault();
    light.kind = PrRenderLightKind_Point;
    light.color = {1, 0.3f, 0.2f};
    light.intensity = 6;
    light.position = {-3.2f, 1.2f, 1.2f};
    light.range = 3;
    prRenderLightAdd(s.update, &light);
    light.color = {0.2f, 0.4f, 1};
    light.position = {-0.8f, 1.2f, 1.2f};
    light.range = 0;
    prRenderLightAdd(s.update, &light);
    light.kind = PrRenderLightKind_Spot;
    light.color = {0.3f, 1, 0.3f};
    light.intensity = 30;
    light.position = {1.6f, 3, 0.5f};
    light.direction = {0, -1, 0};
    light.innerConeAngle = 0.15f;
    light.outerConeAngle = 0.3f;
    prRenderLightAdd(s.update, &light);
    light.color = {1, 1, 0.6f};
    light.position = {3.6f, 3, 0.5f};
    light.innerConeAngle = 0.1f;
    light.outerConeAngle = 0.8f;
    prRenderLightAdd(s.update, &light);
    PrRenderLightDesc dim = prRenderLightDescDefault();
    dim.direction = {0.3f, -1, -0.4f};
    dim.intensity = 0.15f;
    prRenderLightAdd(s.update, &dim);
}

void shadows(Stage& s) {
    addGround(s);
    // A sun eight times the size of the real one, so that a shadow's edge widens visibly with the height of what casts
    // it: posts of rising height, and slabs that hover higher and higher.
    addDaylight(s, 8 * kPrRenderSunAngularRadius);
    PrRenderMaterial material = addGltf(s, gltfOf(kWhite, 0, 0.7f));
    for (uint32_t i = 0; i < 6; ++i) {
        float height = 0.4f + 0.5f * float(i);
        float x = (float(i) - 2.5f) * 1.4f;
        addInstance(s, s.slab, material, placed({x, height * 0.5f, -1}, {0.3f, height, 0.3f}));
        addInstance(s, s.slab, material, placed({x, 0.3f + 0.5f * float(i), 1.2f}, {0.8f, 0.05f, 0.8f}));
    }
    s.world->distance = 9;
}

// A mirror, a rough metal and a rough dielectric, which show an environment's radiance and its filtered light.
void addProbes(Stage& s) {
    addGround(s);
    const float roughness[5] = {0, 0.25f, 0.5f, 0.8f, 1};
    for (uint32_t i = 0; i < 5; ++i) {
        PrRenderKhronosPbrMaterial m = gltfOf(kWhite, i < 4 ? 1.0f : 0.0f, roughness[i]);
        addInstance(s, s.sphere, addGltf(s, m), placed({(float(i) - 2) * 1.4f, 0.5f, 0}));
    }
    s.world->distance = 6.5f;
}

void environmentSky(Stage& s) {
    addProbes(s);
    addDaylight(s);
}

// Returns the radiance of a made-up environment along a direction: a warm light above, a cool one to the side, over a
// dark floor.
PrVec3 studioRadiance(PrVec3 d) {
    float key = prF32Max(d.y * 0.8f + d.x * 0.6f, 0);
    float fill = prF32Max(-d.x * 0.7f + d.z * 0.7f, 0);
    PrVec3 base = d.y > 0 ? PrVec3{0.25f, 0.3f, 0.4f} : PrVec3{0.05f, 0.045f, 0.04f};
    return base + PrVec3{4, 3.2f, 2.4f} * prF32Pow(key, 16) + PrVec3{0.6f, 0.9f, 1.6f} * prF32Pow(fill, 6);
}

void setEnvironment(Stage& s) {
    PrRenderSceneEnvironment environment = prRenderSceneEnvironmentDefault(s.world->environment);
    prRenderSceneEnvironmentSet(s.update, &environment);
}

void environmentCube(Stage& s) {
    addProbes(s);
    // Six square images of floats, in lib/gpu's order of a cube's faces: +x, -x, +y, -y, +z, -z.
    constexpr uint32_t kSide = 64;
    Array<float> texels(size_t(kSide) * kSide * 3 * 6);
    PrRenderRadianceImage faces[6];
    static const PrVec3 kAxes[6][3] = {
        {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},
    };
    for (uint32_t face = 0; face < 6; ++face) {
        float* to = texels.begin() + size_t(face) * kSide * kSide * 3;
        for (uint32_t y = 0; y < kSide; ++y) {
            for (uint32_t x = 0; x < kSide; ++x) {
                float u = (float(x) + 0.5f) / kSide * 2 - 1;
                float v = (float(y) + 0.5f) / kSide * 2 - 1;
                PrVec3 radiance = studioRadiance(prVec3Normalize(kAxes[face][0] + kAxes[face][1] * u + kAxes[face][2] * v));
                float* texel = to + (size_t(y) * kSide + x) * 3;
                texel[0] = radiance.x;
                texel[1] = radiance.y;
                texel[2] = radiance.z;
            }
        }
        faces[face] = PrRenderRadianceImage{
            .pixels = {reinterpret_cast<const uint8_t*>(to), size_t(kSide) * 3 * sizeof(float), {kSide, kSide}},
            .channels = 3,
        };
    }
    s.world->environment = prRenderEnvironmentCreate(s.render);
    prRenderEnvironmentSetCube(s.render, s.world->environment, faces);
    setEnvironment(s);
}

void environmentEquirect(Stage& s) {
    addProbes(s);
    // One image of floats, whose columns are longitude and whose rows are latitude from the top.
    constexpr uint32_t kWidth = 256;
    constexpr uint32_t kHeight = 128;
    Array<float> texels(size_t(kWidth) * kHeight * 3);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            float polar = (float(y) + 0.5f) / kHeight * kPi;
            // The image's middle column is the direction toward +x, and the columns right of it turn toward +z.
            float around = ((float(x) + 0.5f) / kWidth - 0.5f) * 2 * kPi;
            PrVec3 d{prF32Sin(polar) * prF32Cos(around), prF32Cos(polar), prF32Sin(polar) * prF32Sin(around)};
            PrVec3 radiance = studioRadiance(d);
            float* texel = texels.begin() + (size_t(y) * kWidth + x) * 3;
            texel[0] = radiance.x;
            texel[1] = radiance.y;
            texel[2] = radiance.z;
        }
    }
    PrRenderRadianceImage image{
        .pixels = {
            reinterpret_cast<const uint8_t*>(texels.begin()), size_t(kWidth) * 3 * sizeof(float),
            {kWidth, kHeight},
        },
        .channels = 3,
    };
    s.world->environment = prRenderEnvironmentCreate(s.render);
    prRenderEnvironmentSetEquirect(s.render, s.world->environment, &image);
    setEnvironment(s);
}

void toneMaps(Stage& s) {
    // Emitters that double in radiance from one to the next, in a neutral and in three saturated colors, which each
    // tone map of the display shows its own way (--tone-map, or T in the window).
    addGrid(s, 4, 6, [&](uint32_t row, uint32_t column, float) {
        const PrVec3 colors[4] = {{1, 1, 1}, {1, 0.25f, 0.05f}, {0.1f, 1, 0.2f}, {0.1f, 0.2f, 1}};
        PrRenderKhronosPbrMaterial m = gltfOf(kBlack, 0, 1);
        m.emissive = colors[row];
        m.emissiveStrength = 0.125f * float(1u << (column * 2));
        return addGltf(s, m);
    });
}

void instancing(Stage& s) {
    addGround(s);
    addDaylight(s);
    // Four materials and one mesh, in six hundred instances.
    PrRenderMaterial materials[4] = {
        addGltf(s, gltfOf(kRed, 0, 0.4f)), addGltf(s, gltfOf(kGold, 1, 0.3f)),
        addGltf(s, gltfOf(kBlue, 0, 0.7f)), addGltf(s, gltfOf(kWhite, 1, 0.1f)),
    };
    for (uint32_t z = 0; z < 20; ++z) {
        for (uint32_t x = 0; x < 30; ++x) {
            float size = 0.15f + 0.1f * float((x * 7 + z * 13) % 4);
            PrVec3 position{(float(x) - 14.5f) * 0.45f, size * 0.5f, (float(z) - 9.5f) * 0.45f};
            addInstance(s, s.sphere, materials[(x + z * 3) % 4], placed(position, {size, size, size}));
        }
    }
    s.world->distance = 12;
}

// ---------------------------------------------------------------------------------------------------------------------
// Deformation

// The joint that bends a bar of `bend` radians about z, at the bar's middle, from where the mesh was bound.
PrAffine3 bentJoint(float bend) {
    float c = prF32Cos(bend);
    float s = prF32Sin(bend);
    // A turn about the point (0, 1, 0): p' = R (p - pivot) + pivot.
    return PrAffine3{{{c, -s, 0, s}, {s, c, 0, 1 - c}, {0, 0, 1, 0}}};
}

// A tube of radius 0.15 from y = 0 to y = 2 about the y axis, bound to two joints: the first holds its lower half, and
// the second its upper half, with a blend about its middle.
Geometry barGeometry() {
    constexpr uint32_t kRings = 32;
    constexpr uint32_t kSegments = 24;
    Geometry g;
    for (uint32_t ring = 0; ring <= kRings; ++ring) {
        float y = 2 * float(ring) / kRings;
        float upper = prF32Clamp((y - 0.7f) / 0.6f, 0, 1);
        upper = upper * upper * (3 - 2 * upper);
        const uint16_t joints[kPrRenderSkinInfluences] = {0, 1, 0, 0};
        const float weights[kPrRenderSkinInfluences] = {1 - upper, upper, 0, 0};
        for (uint32_t segment = 0; segment <= kSegments; ++segment) {
            float around = 2 * kPi * float(segment) / kSegments;
            PrVec3 n{prF32Cos(around), 0, prF32Sin(around)};
            pushVertex(&g, PrVec3{n.x * 0.15f, y, n.z * 0.15f}, n, {0, 1, 0}, {float(segment) / kSegments, y / 2});
            g.skin.push(prRenderSkinVertexPack(joints, weights));
        }
    }
    for (uint32_t ring = 0; ring < kRings; ++ring) {
        for (uint32_t segment = 0; segment < kSegments; ++segment) {
            uint32_t a = ring * (kSegments + 1) + segment;
            uint32_t d = a + kSegments + 1;
            const uint32_t corners[6] = {a, d, d + 1, a, d + 1, a + 1};
            for (uint32_t index : corners)
                g.indices.push(index);
        }
    }
    g.jointCount = 2;
    return g;
}

// Returns the change of a unit normal `n` of a surface that a scale stretches.
PrVec3 stretchedNormal(PrVec3 n, PrVec3 scale) {
    return prVec3Normalize(PrVec3{n.x / scale.x, n.y / scale.y, n.z / scale.z}) - n;
}

// The sphere with two morph targets: one stretches it along x, and the other flattens it along y.
Geometry morphedSphereGeometry() {
    Geometry g = sphereGeometry();
    const PrVec3 scales[2] = {{1.5f, 1, 1}, {1.15f, 0.55f, 1.15f}};
    g.morphTargetCount = 2;
    for (uint32_t ring = 0; ring <= kSphereRings; ++ring) {
        for (uint32_t segment = 0; segment <= kSphereSegments; ++segment) {
            PrVec3 n = sphereNormal(ring, segment);
            for (PrVec3 scale : scales) {
                PrVec3 moved = PrVec3{n.x * (scale.x - 1), n.y * (scale.y - 1), n.z * (scale.z - 1)} * 0.5f;
                PrVec3 turned = stretchedNormal(n, scale);
                g.morphs.push(PrRenderMorphDelta{
                    .position = {moved.x, moved.y, moved.z},
                    .normal = {turned.x, turned.y, turned.z},
                    .tangent = {0, 0, 0},
                });
            }
        }
    }
    return g;
}

// Three bars that bend by their joints, each at its own pace, and two spheres between their morph targets.
void moveDeformation(PrRenderUpdate* update, const World& world, double time) {
    float t = float(time);
    for (uint32_t i = 0; i < 3; ++i) {
        const PrAffine3 joints[2] = {placed({}), bentJoint(0.9f * prF32Sin(t * (1.1f + 0.3f * float(i)) + float(i)))};
        prRenderInstanceSetJoints(update, world.moving[i], joints, 2);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        float phase = t * 1.4f + 1.7f * float(i);
        const float weights[2] = {0.5f + 0.5f * prF32Sin(phase), 0.5f + 0.5f * prF32Cos(phase * 0.8f)};
        prRenderInstanceSetMorphWeights(update, world.moving[3 + i], weights, 2);
    }
}

void deformation(Stage& s) {
    addGround(s);
    addDaylight(s);
    PrRenderMesh bar = addMesh(s, barGeometry(), 1.2f);
    PrRenderMesh blob = addMesh(s, morphedSphereGeometry(), 0.5f);
    const PrColor colors[3] = {kRed, kGold, kBlue};
    for (uint32_t i = 0; i < 3; ++i)
        addMoving(s, bar, addGltf(s, gltfOf(colors[i], i == 1 ? 1.0f : 0.0f, 0.4f)), placed({float(i) - 3.2f, 0, 0}));
    for (uint32_t i = 0; i < 2; ++i) {
        PrRenderMaterial material = addGltf(s, gltfOf(i == 0 ? kWhite : kGold, float(i), 0.3f));
        addMoving(s, blob, material, placed({1.2f + 1.9f * float(i), 0.6f, 0}, {1.2f, 1.2f, 1.2f}));
    }
    s.world->move = moveDeformation;
    s.world->distance = 7.5f;
}

// ---------------------------------------------------------------------------------------------------------------------
// The example's own material types and shading model

// A checker of the example's type, for the ground and the pages' cubes.
FeaturesCheckerParameters checkerOf(float cells) {
    return FeaturesCheckerParameters{
        .even = {0.62f, 0.62f, 0.6f, 1},
        .odd = {0.28f, 0.28f, 0.27f, 1},
        .cells = cells,
        .evenRoughness = 0.4f,
        .oddRoughness = 0.8f,
    };
}

void customMaterial(Stage& s) {
    addDaylight(s);
    // A ground of the checker type, whose cells the material computes and filters by the technique's derivatives, and
    // cubes of the same type with more and more cells to a face.
    PrRenderMaterial ground = addCustom(s, kRenderFeaturesMaterialChecker, checkerOf(40));
    addInstance(s, s.slab, ground, placed({0, -0.25f, 0}, {40, 0.5f, 40}));
    for (uint32_t i = 0; i < 6; ++i) {
        PrRenderMaterial material = addCustom(s, kRenderFeaturesMaterialChecker, checkerOf(float(1u << i)));
        addInstance(s, s.slab, material, placed({(float(i) - 2.5f) * 1.3f, 0.5f, 0}, {0.9f, 0.9f, 0.9f}));
    }
    s.world->distance = 7;
}

constexpr PrColor kCubeColors[8] = {
    {0.1f, 0.45f, 0.85f, 1},  {0.15f, 0.7f, 0.4f, 1},  {0.9f, 0.6f, 0.1f, 1}, {0.75f, 0.2f, 0.6f, 1},
    {0.85f, 0.85f, 0.85f, 1}, {0.2f, 0.75f, 0.75f, 1}, {0.9f, 0.3f, 0.2f, 1}, {0.45f, 0.35f, 0.85f, 1},
};

// Returns a transform that scales by `scale`, then turns by `turn` radians about y, then moves to `position`.
PrAffine3 turnedAt(PrVec3 position, float turn, float scale) {
    float c = prF32Cos(turn) * scale;
    float s = prF32Sin(turn) * scale;
    return PrAffine3{{{c, 0, s, position.x}, {0, scale, 0, position.y}, {-s, 0, c, position.z}}};
}

// Where the cube `i` of the circle is at `time`: turning and rising as it goes about the circle's middle.
PrAffine3 circlingCube(uint32_t i, float time) {
    float around = 2 * kPi * float(i) / 8 + time * 0.25f;
    PrVec3 position{prF32Cos(around) * 3, 0.9f + 0.4f * prF32Sin(time * 1.3f + float(i)), prF32Sin(around) * 3};
    return turnedAt(position, time * 0.8f + float(i), 0.8f);
}

void moveCircle(PrRenderUpdate* update, const World& world, double time) {
    PrAffine3 transforms[8];
    for (uint32_t i = 0; i < 8; ++i)
        transforms[i] = circlingCube(i, float(time));
    prRenderInstanceSetTransforms(update, world.moving.begin(), transforms, 8);
}

void sceneTime(Stage& s) {
    addGround(s);
    addDaylight(s);
    // Cubes of the pulse type, up which bands of their own color's light run by the scene's time, at a speed of each
    // material's own, while the same time carries them about a circle. Every speed is a whole number of tenths of a
    // band a second, so the materials repeat within the period at which the example wraps the time.
    for (uint32_t i = 0; i < 8; ++i) {
        PrColor color = kCubeColors[i];
        FeaturesPulseParameters pulse{
            .color = {color.r * 0.5f, color.g * 0.5f, color.b * 0.5f, 1},
            .glow = {color.r * 3, color.g * 3, color.b * 3, 1},
            .bands = 2,
            .speed = 0.4f + 0.1f * float(i),
            .roughness = 0.45f,
        };
        addMoving(s, s.slab, addCustom(s, kRenderFeaturesMaterialPulse, pulse), circlingCube(i, 0));
    }
    s.world->move = moveCircle;
    s.world->distance = 8;
}

void toon(Stage& s) {
    addGround(s);
    addDaylight(s);
    // Spheres and cubes of the toon type, which the example's toon model lights: steps of the light's angle, a shadow
    // without a penumbra, and a highlight with a hard edge, beside spheres of the library's model.
    for (uint32_t i = 0; i < 6; ++i) {
        PrColor color = kCubeColors[i];
        FeaturesToonParameters paint{
            .color = {color.r, color.g, color.b, 1},
            .bands = float(2 + i / 2),
            .highlight = 0.5f,
            .highlightEdge = 0.97f,
        };
        PrRenderMaterial material = addCustom(s, kRenderFeaturesMaterialToon, paint);
        float x = (float(i) - 2.5f) * 1.3f;
        addInstance(s, s.sphere, material, placed({x, 0.5f, -0.75f}));
        addInstance(s, s.slab, material, turnedAt({x, 0.35f, 0.75f}, 0.5f + float(i), 0.7f));
    }
    s.world->distance = 7;
}

template <void (*Build)(Stage&)>
void build(PrRender* render, World* world) {
    Stage s{.render = render, .world = world};
    world->scene = prRenderSceneCreate(render);
    s.update = prRenderUpdateBegin(render, world->scene);
    s.sphere = addSphere(s);
    s.slab = addFaces(s, 6);
    s.quad = addFaces(s, 1);
    // Dimples for a normal texture: a tangent-space normal of a height field of bumps, encoded from [-1, 1] to [0, 1].
    s.dimples = addTexture(s, 64, false, [](uint32_t x, uint32_t y) {
        float u = (float(x) + 0.5f) / 64 * 2 * kPi;
        float v = (float(y) + 0.5f) / 64 * 2 * kPi;
        PrVec3 n = prVec3Normalize({-0.6f * prF32Cos(u) * prF32Sin(v), -0.6f * prF32Sin(u) * prF32Cos(v), 1});
        return PrColor{n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f, 1};
    });
    s.checker = addTexture(s, 64, true, [](uint32_t x, uint32_t y) {
        return ((x / 32) + (y / 32)) % 2 == 0 ? PrColor{0.82f, 0.8f, 0.76f, 1} : PrColor{0.3f, 0.32f, 0.36f, 1};
    });
    Build(s);
    prRenderUpdateCommit(s.update);
}

constexpr Page kPages[] = {
    {"gltf-core", "glTF's core material: a dielectric and a metal, from smooth to rough", build<gltfCore>},
    {"gltf-ior", "KHR_materials_ior: indices of 1, 1.33, 1.5, 2, 2.42 and infinity, smooth and rough", build<gltfIor>},
    {"gltf-specular", "KHR_materials_specular: the strength from 0 to 1, and the color", build<gltfSpecular>},
    {
        "gltf-clearcoat", "KHR_materials_clearcoat: the strength, the roughness, and the two normal maps",
        build<gltfClearcoat>,
    },
    {"gltf-sheen", "KHR_materials_sheen: the roughness of a white sheen, and the color", build<gltfSheen>},
    {
        "gltf-iridescence", "KHR_materials_iridescence: thickness over a dielectric and a metal, and the strength",
        build<gltfIridescence>,
    },
    {"gltf-anisotropy", "KHR_materials_anisotropy: the strength, and the rotation", build<gltfAnisotropy>},
    {"gltf-transmission", "KHR_materials_transmission: the factor, and the roughness", build<gltfTransmission>},
    {"gltf-volume", "KHR_materials_volume: the thickness, and the attenuation distance", build<gltfVolume>},
    {"gltf-dispersion", "KHR_materials_dispersion: from none to an Abbe number of 2.5", build<gltfDispersion>},
    {
        "gltf-diffuse-transmission", "KHR_materials_diffuse_transmission: the factor, and the color, on sheets",
        build<gltfDiffuseTransmission>,
    },
    {"gltf-emissive", "KHR_materials_emissive_strength from 0.25 to 8, and KHR_materials_unlit", build<gltfEmissive>},
    {
        "gltf-specular-glossiness", "KHR_materials_pbrSpecularGlossiness: the glossiness, and a colored specular",
        build<gltfSpecularGlossiness>,
    },
    {
        "openpbr-base", "OpenPBR base: roughness of a dielectric and of a metal, and the diffuse roughness",
        build<openPbrBase>,
    },
    {
        "openpbr-specular", "OpenPBR specular: the index, the color and weight, and the anisotropy with its tangent",
        build<openPbrSpecular>,
    },
    {
        "openpbr-coat", "OpenPBR coat: the weight; roughness, color and darkening; anisotropy and the two normals",
        build<openPbrCoat>,
    },
    {"openpbr-fuzz", "OpenPBR fuzz: the weight, and the roughness", build<openPbrFuzz>},
    {
        "openpbr-thin-film", "OpenPBR thin film: the thickness over a dielectric and over a metal",
        build<openPbrThinFilm>,
    },
    {
        "openpbr-transmission", "OpenPBR transmission: the weight; depth and dispersion; roughness and thin walls",
        build<openPbrTransmission>,
    },
    {
        "openpbr-subsurface", "OpenPBR subsurface: the weight on spheres, and on thin walls against the sun",
        build<openPbrSubsurface>,
    },
    {"openpbr-emission", "OpenPBR emission bare and under a coat, and the opacity", build<openPbrEmission>},
    {
        "alpha-modes", "Alpha modes: opaque, masked at 0.25, 0.5 and 0.75, blended, and blended double sided",
        build<alphaModes>,
    },
    {
        "texture-transforms", "Texture transforms: none, offset, rotation, scale, all three, and the second UV set",
        build<textureTransforms>,
    },
    {"lights", "Lights: two point lights, with a range and without, two spots, and a directional light", build<lights>},
    {"shadows", "Shadows: the penumbra of a wide sun, by the height of what casts the shadow", build<shadows>},
    {"environment-sky", "An environment from the analytic sky", build<environmentSky>},
    {"environment-cube", "An environment from six images of a cube's faces", build<environmentCube>},
    {"environment-equirect", "An environment from an equirectangular image", build<environmentEquirect>},
    {"tone-maps", "Tone maps: emitters that quadruple in radiance, neutral and saturated", build<toneMaps>},
    {"instancing", "Instancing: six hundred instances of one mesh", build<instancing>},
    {
        "deformation", "Skinning and morphing: bars that two joints bend, and spheres between two morph targets",
        build<deformation>,
    },
    {
        "custom-material", "A material type of the example's: a checker that it computes and filters, on the ground "
            "and on cubes", build<customMaterial>,
    },
    {
        "scene-time", "The scene's time: bands of light that a material type moves, on cubes that the time carries",
        build<sceneTime>,
    },
    {"toon", "A shading model of the example's: toon steps of light, a hard shadow and a hard highlight", build<toon>},
};

} // namespace

Span<const Page> pages() {
    return Span<const Page>(kPages);
}

uint32_t pageNamed(PrStringView name) {
    for (uint32_t i = 0; i < pages().count(); ++i) {
        if (kPages[i].name == name)
            return i;
    }
    return uint32_t(pages().count());
}

void worldCreate(PrRender* render, const Page& page, World* out) {
    *out = World{};
    page.build(render, out);
}

void worldSetTime(PrRender* render, const World& world, double time) {
    // A float loses a frame's step as it grows, so the time wraps at a period of every page's materials: a whole number
    // of the periods of the pulse type's speeds, which are tenths of a band a second.
    constexpr double kTimePeriod = 600;
    PrRenderUpdate* update = prRenderUpdateBegin(render, world.scene);
    prRenderSceneTimeSet(update, float(prF64FloorMod(time, kTimePeriod)));
    if (world.move != nullptr)
        world.move(update, world, time);
    prRenderUpdateCommit(update);
}

void worldDestroy(PrRender* render, World* world) {
    prRenderSceneDestroy(render, world->scene);
    for (PrRenderMaterial material : world->materials)
        prRenderMaterialDestroy(render, material);
    for (PrRenderTexture texture : world->textures)
        prRenderTextureDestroy(render, texture);
    for (PrRenderMesh mesh : world->meshes)
        prRenderMeshDestroy(render, mesh);
    prRenderEnvironmentDestroy(render, world->environment);
    *world = World{};
}

} // namespace render_features
