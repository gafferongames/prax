// A renderer's views: the passes that draw one view of a scene, which each technique adds to the frame's graph, the
// passes that the views of a scene share, and the draw that shows a view's color.

#include "pr/render_view.h"
#include "render/src/render_view.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/sort.h"
#include "pr/span.h"

#include "render/src/render.h"
#include "render/src/render_deform.h"
#include "render/src/render_environment.h"
#include "render/src/render_meter.h"
#include "render/src/render_pipeline.h"
#include "render/src/render_resource.h"
#include "render/src/render_scene.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Views

// Writes a view's matrix from the world to clip space into the uniforms, and the planes of its frustum. Clip space
// holds a point if its x and y lie within its w of zero, and its z between zero and its w.
void setView(PrRenderFrameUniforms* uniforms, uint32_t view, const PrMatrix4& matrix) {
    const float (*m)[4] = matrix.m;
    for (uint32_t row = 0; row < 4; ++row)
        uniforms->viewProjection[view * 4 + row] = {m[row][0], m[row][1], m[row][2], m[row][3]};
    float planes[kPrRenderViewPlanes][4];
    for (uint32_t i = 0; i < 4; ++i) {
        planes[0][i] = m[3][i] + m[0][i];
        planes[1][i] = m[3][i] - m[0][i];
        planes[2][i] = m[3][i] + m[1][i];
        planes[3][i] = m[3][i] - m[1][i];
        planes[4][i] = m[2][i];
        planes[5][i] = m[3][i] - m[2][i];
    }
    for (uint32_t i = 0; i < kPrRenderViewPlanes; ++i) {
        const float* p = planes[i];
        float length = prF32Sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        // A plane without a normal bounds nothing. The far plane of a view that has none is such a plane.
        PrRenderVec4 plane{0, 0, 0, 1};
        if (length >= 1e-12f)
            plane = {p[0] / length, p[1] / length, p[2] / length, p[3] / length};
        uniforms->planes[view * kPrRenderViewPlanes + i] = plane;
    }
}

// Returns the point of the world that a camera's matrix from the world to clip space maps to a point of clip space with
// a w of 1. `inverse` is the inverse of that matrix.
PrVec3 unproject(const PrMatrix4& inverse, float x, float y, float z) {
    const float (*m)[4] = inverse.m;
    float w = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3];
    return PrVec3{
        (m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3]) / w,
        (m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3]) / w,
        (m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]) / w,
    };
}

// Writes the matrix that maps a point of a camera's target to the direction that the camera sees there. The direction
// at a point runs from the point's place at a depth of one half toward its place at a depth of 1, the near plane. It is
// linear in the point for a perspective and constant for an orthographic camera, so its values at three points give the
// matrix. A camera whose matrix has no inverse sees along -z everywhere.
void setBackgroundRay(PrRenderFrameUniforms* uniforms, const PrRenderCamera& camera) {
    PrMatrix4 viewProjection = prMatrix4Concat(&camera.view, &camera.projection);
    PrMatrix4 inverse{};
    PrVec3 middle{0, 0, -1};
    PrVec3 right{};
    PrVec3 up{};
    if (prMatrix4Invert(&viewProjection, &inverse)) {
        middle = unproject(inverse, 0, 0, 0.5f) - unproject(inverse, 0, 0, 1);
        right = unproject(inverse, 1, 0, 0.5f) - unproject(inverse, 1, 0, 1) - middle;
        up = unproject(inverse, 0, 1, 0.5f) - unproject(inverse, 0, 1, 1) - middle;
    }
    uniforms->backgroundRay[0] = {right.x, up.x, middle.x, 0};
    uniforms->backgroundRay[1] = {right.y, up.y, middle.y, 0};
    uniforms->backgroundRay[2] = {right.z, up.z, middle.z, 0};
}

PrVec3 worldPoint(const PrRenderInstanceRecord& instance, PrVec3 point) {
    const PrRenderVec4* w = instance.world;
    return PrVec3{
        w[0].x * point.x + w[0].y * point.y + w[0].z * point.z + w[0].w,
        w[1].x * point.x + w[1].y * point.y + w[1].z * point.z + w[1].w,
        w[2].x * point.x + w[2].y * point.y + w[2].z * point.z + w[2].w,
    };
}

// Returns the largest factor by which an instance scales a length along one of its mesh's axes.
float worldScale(const PrRenderInstanceRecord& instance) {
    const PrRenderVec4* w = instance.world;
    float x = w[0].x * w[0].x + w[1].x * w[1].x + w[2].x * w[2].x;
    float y = w[0].y * w[0].y + w[1].y * w[1].y + w[2].y * w[2].y;
    float z = w[0].z * w[0].z + w[1].z * w[1].z + w[2].z * w[2].z;
    return prF32Sqrt(prF32Max(x, prF32Max(y, z)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Draws

// What the passes of one view of a scene read while the graph records them. It lives in the graph's memory. The light's
// view has one too, for the passes that the views of a scene share.
struct ViewFrame {
    Render* render;
    RenderSceneState* scene;
    PrGpuGraph* graph;
    PrRenderFrameUniforms uniforms;
    uint32_t view;                  // kPrRenderViewCamera or kPrRenderViewLight
    PrGpuBuffer arguments;          // the indirect arguments that the view's cull writes, by draw
    uint8_t samples;
    PrGpuTexture shadowMap;         // the texture that shading reads as the shadow map
    PrGpuTexture environment;       // the cube that shading and the background read as the environment

    // The view's blended draws, from the farthest to the nearest.
    const uint32_t* PR_NULLABLE blendOrder;
    uint32_t blendCount;

    PrGpuGraphTexture visibility;   // the visibility technique's buffer

    // The scene behind the surfaces that read it: the view's color before its blended draws, with a level for each
    // blur. None for a view of a scene without such a surface. `behindSource` is the texture that is copied into its
    // first level, or none where a pass resolves into it.
    PrGpuGraphTexture behind;
    PrGpuGraphTexture behindSource;
    PrUVec2 extent;
};

// What a pass has bound for its draws, so that a draw binds only what differs from the draw before it.
struct DrawBindings {
    PrGpuPipeline pipeline;
    RenderLibrary library;          // the library of the pipeline, whose slots the pass binds
    uint32_t geometry;
    uint32_t firstVertex;           // the vertex of `geometry` that the vertex buffer starts at
    uint32_t attributes;
    uint32_t firstAttribute;        // the vertex of `attributes` that the second vertex buffer starts at
    uint32_t indices;
    uint32_t material;              // the material whose textures are bound, or kNoMaterial
};

constexpr uint32_t kNoMaterial = 0xffffffffu;

// How a pass issues its draws.
enum {
    DrawFlag_Indexed = 1u << 0,     // fetches vertices through the vertex layout; otherwise the shader pulls them
    DrawFlag_Materials = 1u << 1,   // the shaders sample the draw's material
    DrawFlag_Shadows = 1u << 2,     // the shaders read the shadow map
    DrawFlag_Environment = 1u << 3, // the shaders read the environment
    DrawFlag_Tables = 1u << 4,      // the shaders read the shader set's tables
    DrawFlag_Shading = DrawFlag_Shadows | DrawFlag_Environment | DrawFlag_Tables,
    DrawFlag_ByType = 1u << 5,      // each material type has a pipeline of its own
    DrawFlag_ByLayout = 1u << 6,    // each layout of the types' attributes has a pipeline of its own
};
typedef uint32_t DrawFlags;

// Returns the library of a pipeline of `kind`.
RenderLibrary libraryOf(RenderPipelineKind kind) {
    switch (kind) {
        case RenderPipelineKind_Depth:
            return RenderLibrary_Depth;
        case RenderPipelineKind_DepthMask:
            return RenderLibrary_DepthMask;
        case RenderPipelineKind_Background:
            return RenderLibrary_Background;
        case RenderPipelineKind_Visibility:
        case RenderPipelineKind_Shade:
            return RenderLibrary_Visibility;
        default:
            return RenderLibrary_Forward;
    }
}

// Binds a texture to a slot that a library's source declares, if it declares it.
void bindTexture(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler) {
    if (slot != kRenderSlotNone)
        prGpuCommandsSetTexture(commands, slot, texture, sampler);
}

// Binds a buffer to a slot that a library's source declares, if it declares it.
void bindBuffer(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer) {
    if (slot != kRenderSlotNone)
        prGpuCommandsSetBuffer(commands, slot, buffer, 0);
}

// Binds the scene's records to the slots of a library that reads them.
void bindRecords(const ViewFrame& v, PrGpuCommands* commands, const RenderBindings& b) {
    const Render* r = v.render;
    bindBuffer(commands, b.instances, v.scene->instanceBuffer.buffer);
    bindBuffer(commands, b.draws, v.scene->drawBuffer.buffer);
    bindBuffer(commands, b.parts, r->partBuffer.buffer);
    bindBuffer(commands, b.materials, r->materialBuffer.buffer);
}

// Sets the pipeline of the draws that follow, with the view's uniforms, and through slots its records.
void usePipeline(const ViewFrame& v, PrGpuPass* pass, DrawBindings* bound, PrGpuPipeline pipeline,
        RenderPipelineKind kind, DrawFlags flags) {
    if (bound->pipeline.id == pipeline.id)
        return;
    const Render* r = v.render;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    *bound = DrawBindings{.pipeline = pipeline, .library = libraryOf(kind), .material = kNoMaterial};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &v.uniforms, sizeof(v.uniforms));
    if (r->bindless)
        return;
    const RenderBindings& b = r->bindings[bound->library];
    bindRecords(v, commands, b);
    if ((flags & DrawFlag_Shadows) != 0)
        bindTexture(commands, b.shadowMap, v.shadowMap, PrGpuSampler{});
    if ((flags & DrawFlag_Environment) != 0)
        bindTexture(commands, b.environment, v.environment, r->environmentSampler);
    if ((flags & DrawFlag_Tables) != 0)
        bindTexture(commands, b.tables, r->tables, PrGpuSampler{});
}

// Unbinds the textures that a pass bound through slots of `library`, so that a later pass may write them.
void unbindTextures(const ViewFrame& v, PrGpuPass* pass, RenderLibrary library, DrawFlags flags) {
    if (v.render->bindless)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    const RenderBindings& b = v.render->bindings[library];
    if ((flags & DrawFlag_Shadows) != 0)
        bindTexture(commands, b.shadowMap, PrGpuTexture{}, PrGpuSampler{});
    if ((flags & DrawFlag_Environment) != 0)
        bindTexture(commands, b.environment, PrGpuTexture{}, PrGpuSampler{});
    if ((flags & DrawFlag_Tables) != 0)
        bindTexture(commands, b.tables, PrGpuTexture{}, PrGpuSampler{});
    if ((flags & DrawFlag_Materials) != 0) {
        for (uint32_t i = 0; i < b.materialTextureCount; ++i)
            bindTexture(commands, b.materialTextures[i], PrGpuTexture{}, PrGpuSampler{});
    }
}

// Records one of the scene's draws with the pass's pipeline, if the view's cull kept it.
void drawOne(const ViewFrame& v, PrGpuPass* pass, DrawBindings* bound, uint32_t index, DrawFlags flags) {
    const Render* r = v.render;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    const PrRenderDrawRecord& draw = v.scene->draws.data[index];
    if ((flags & DrawFlag_Indexed) != 0) {
        const PrRenderPartRecord& part = r->parts.data[draw.part];
        // Each vertex buffer starts at the part's first vertex, so that the part's indices name its vertices from zero
        // without a base vertex. The geometry is the part's, and the attributes the draw's.
        if (bound->geometry != part.geometry || bound->firstVertex != part.firstVertex) {
            prGpuPassSetVertexBuffer(pass, 0, PrGpuBuffer{part.geometry},
                uint64_t(part.firstVertex) * kPrRenderGeometryBytes);
            bound->geometry = part.geometry;
            bound->firstVertex = part.firstVertex;
        }
        if (draw.attributes != 0 &&
                (bound->attributes != draw.attributes || bound->firstAttribute != draw.firstAttribute)) {
            uint32_t layout = r->shaders->variants[r->materialRecords.data[draw.material].variant].layout;
            prGpuPassSetVertexBuffer(pass, 1, PrGpuBuffer{draw.attributes},
                uint64_t(draw.firstAttribute) * r->shaders->layouts[layout].stride);
            bound->attributes = draw.attributes;
            bound->firstAttribute = draw.firstAttribute;
        }
        if (bound->indices != part.indices) {
            prGpuPassSetIndexBuffer(pass, PrGpuBuffer{part.indices}, 0, true);
            bound->indices = part.indices;
        }
    }
    if (!r->bindless && (flags & DrawFlag_Materials) != 0 && bound->material != draw.material) {
        const RenderBindings& b = r->bindings[bound->library];
        const RenderMaterialSlots& slots = r->materialSlots.data[draw.material];
        for (uint32_t i = 0; i < b.materialTextureCount; ++i)
            bindTexture(commands, b.materialTextures[i], slots.textures[i], slots.samplers[i]);
        // The scene behind the surface is in the last of the material's slots, in a pass that has it.
        if (slots.behind && v.uniforms.behindLastLevel >= 0 && b.materialTextureCount != 0) {
            bindTexture(commands, b.materialTextures[b.materialTextureCount - 1], PrGpuTexture{v.uniforms.behind},
                r->behindSampler);
        }
        bound->material = draw.material;
    }
    PrRenderDrawPush push{.draw = index, .view = v.view};
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    uint64_t offset = uint64_t(index) * kPrRenderArgumentsBytes;
    if ((flags & DrawFlag_Indexed) != 0)
        prGpuPassDrawIndexedIndirect(pass, v.arguments, offset, 1, kPrRenderArgumentsBytes);
    else
        prGpuPassDrawIndirect(pass, v.arguments, offset, 1, kPrRenderArgumentsBytes);
}

// Returns the material type of a draw's material.
uint32_t drawVariant(const Render* r, const PrRenderDrawRecord& draw) {
    return r->materialRecords.data[draw.material].variant;
}

// Records the draws of one alpha mode, each way of culling with the pipeline that `key` names for it. A pass with
// DrawFlag_ByType draws each material type with the pipeline of that type, and one with DrawFlag_ByLayout each layout
// of the types' attributes with the pipeline of that layout. A bucket's draws are in the order of their types, so the
// pipeline changes at most once for each type that the bucket holds.
void drawAlpha(const ViewFrame& v, PrGpuPass* pass, DrawBindings* bound, uint32_t alpha, RenderPipelineKey key,
        DrawFlags flags) {
    const RenderSceneState* s = v.scene;
    for (uint32_t cull = 0; cull < kRenderCullCount; ++cull) {
        uint32_t bucket = alpha * kRenderCullCount + cull;
        uint32_t end = s->bucketFirst[bucket + 1];
        key.cull = cull;
        PrGpuPipeline pipeline{};
        bool chosen = false;
        for (uint32_t index = s->bucketFirst[bucket]; index < end; ++index) {
            uint32_t variant = drawVariant(v.render, s->draws.data[index]);
            uint32_t drawnVariant = (flags & DrawFlag_ByType) != 0 ? variant : 0;
            uint32_t layout = 0;
            if ((flags & (DrawFlag_ByType | DrawFlag_ByLayout)) != 0)
                layout = renderPipelineLayout(v.render, variant);
            if (!chosen || drawnVariant != key.variant || layout != key.layout) {
                key.variant = drawnVariant;
                key.layout = layout;
                pipeline = renderPipelineFor(v.render, key);
                chosen = true;
                if (pipeline.id != 0)
                    usePipeline(v, pass, bound, pipeline, key.kind, flags);
            }
            if (pipeline.id != 0)
                drawOne(v, pass, bound, index, flags);
        }
    }
}

// Records the view's blended draws over what the pass's target holds, from the farthest to the nearest. The order of
// depth comes before every other, so the pipeline changes wherever a draw's type or culling differs from that of the
// draw behind it.
void drawBlended(const ViewFrame& v, PrGpuPass* pass, DrawBindings* bound) {
    DrawFlags flags = DrawFlag_Indexed | DrawFlag_Materials | DrawFlag_Shading;
    for (uint32_t i = 0; i < v.blendCount; ++i) {
        uint32_t index = v.blendOrder[i];
        const PrRenderDrawRecord& draw = v.scene->draws.data[index];
        uint32_t drawFlags = draw.flags;
        RenderCullFlags cull = 0;
        if ((drawFlags & kPrRenderDrawDoubleSided) != 0)
            cull |= RenderCullFlag_DoubleSided;
        if ((drawFlags & kPrRenderDrawMirrored) != 0)
            cull |= RenderCullFlag_Mirrored;
        uint32_t variant = drawVariant(v.render, draw);
        RenderPipelineKey key{
            .kind = RenderPipelineKind_Forward,
            .cull = cull,
            .samples = v.samples,
            .blend = true,
            .variant = variant,
            .layout = renderPipelineLayout(v.render, variant),
        };
        PrGpuPipeline pipeline = renderPipelineFor(v.render, key);
        if (pipeline.id == 0)
            continue;
        usePipeline(v, pass, bound, pipeline, key.kind, flags);
        drawOne(v, pass, bound, index, flags);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Passes

// Records a view's cull, which writes the arguments of the view's draws.
void recordCull(void* PR_NULLABLE context, PrGpuCommands* commands) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    const Render* r = v.render;
    if (v.uniforms.drawCount == 0)
        return;
    prGpuCommandsSetPipeline(commands, r->cull);
    prGpuCommandsSetUniforms(commands, &v.uniforms, sizeof(v.uniforms));
    const RenderBindings& b = r->bindings[RenderLibrary_Cull];
    if (!r->bindless) {
        bindRecords(v, commands, b);
        prGpuCommandsSetRwBuffer(commands, b.arguments, v.arguments, 0);
    }
    uint32_t groupSize = prGpuPipelineGroupSize(r->device, r->cull).width;
    PrRenderDrawPush push{.view = v.view};
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuCommandsDispatch(commands, PrGpuExtent{(v.uniforms.drawCount + groupSize - 1) / groupSize, 1, 1});
    if (!r->bindless)
        prGpuCommandsSetRwBuffer(commands, b.arguments, PrGpuBuffer{}, 0);
}

// Draws the depth of a view's opaque and masked draws.
void drawDepth(void* PR_NULLABLE context, PrGpuPass* pass) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    bool shadow = v.view == kPrRenderViewLight;
    DrawBindings bound{};
    RenderPipelineKey opaque{.kind = RenderPipelineKind_Depth, .samples = v.samples, .shadow = shadow};
    drawAlpha(v, pass, &bound, kPrRenderMaterialOpaque, opaque, DrawFlag_Indexed);
    RenderPipelineKey mask{.kind = RenderPipelineKind_DepthMask, .samples = v.samples, .shadow = shadow};
    drawAlpha(v, pass, &bound, kPrRenderMaterialMask, mask, DrawFlag_Indexed | DrawFlag_Materials | DrawFlag_ByLayout);
    unbindTextures(v, pass, RenderLibrary_DepthMask, DrawFlag_Materials);
}

// Draws the scene's environment wherever the pass's depth holds no surface.
void drawBackground(const ViewFrame& v, PrGpuPass* pass, DrawBindings* bound) {
    RenderPipelineKey key{.kind = RenderPipelineKind_Background, .samples = v.samples};
    PrGpuPipeline pipeline = renderPipelineFor(v.render, key);
    if (pipeline.id == 0)
        return;
    usePipeline(v, pass, bound, pipeline, key.kind, DrawFlag_Environment);
    PrRenderDrawPush push{.view = v.view};
    prGpuCommandsSetPush(prGpuPassCommands(pass), &push, sizeof(push));
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

// Draws the forward technique's surfaces: the opaque and masked draws where the depth pass drew them, then the
// background behind them, and then the blended draws over both, unless a pass of their own draws those.
void drawForward(void* PR_NULLABLE context, PrGpuPass* pass) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    DrawFlags flags = DrawFlag_Indexed | DrawFlag_Materials | DrawFlag_Shading;
    DrawBindings bound{};
    RenderPipelineKey key{.kind = RenderPipelineKind_Forward, .samples = v.samples};
    drawAlpha(v, pass, &bound, kPrRenderMaterialOpaque, key, flags | DrawFlag_ByType);
    drawAlpha(v, pass, &bound, kPrRenderMaterialMask, key, flags | DrawFlag_ByType);
    drawBackground(v, pass, &bound);
    if (v.behind.id == 0)
        drawBlended(v, pass, &bound);
    unbindTextures(v, pass, RenderLibrary_Forward, flags);
    unbindTextures(v, pass, RenderLibrary_Background, DrawFlag_Environment);
}

// Draws the blended draws alone, over a color that an earlier pass shaded. With the scene behind them, the draws that
// read it sample that texture.
void drawBlendedOver(void* PR_NULLABLE context, PrGpuPass* pass) {
    ViewFrame& v = *static_cast<ViewFrame*>(context);
    if (v.behind.id != 0) {
        // The graph lends the texture its device texture only while it records.
        v.uniforms.behind = prGpuGraphTexture(v.graph, v.behind).id;
        v.uniforms.behindSampler = v.render->behindSampler.id;
        v.uniforms.behindLastLevel = float(prGpuMipLevels(v.extent) - 1);
    }
    DrawBindings bound{};
    drawBlended(v, pass, &bound);
    unbindTextures(v, pass, RenderLibrary_Forward, DrawFlag_Materials | DrawFlag_Shading);
}

// Copies the view's color into the first level of the scene behind its blended draws.
void recordBehindCopy(void* PR_NULLABLE context, PrGpuCommands* commands) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, v.extent);
    prGpuCommandsCopyTexture(commands, prGpuGraphTexture(v.graph, v.behindSource), &region,
        prGpuGraphTexture(v.graph, v.behind), &region);
}

// Fills the levels of the scene behind the view's blended draws, each from the one before it.
void recordBehindLevels(void* PR_NULLABLE context, PrGpuCommands* commands) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    prGpuCommandsGenerateMipmaps(commands, prGpuGraphTexture(v.graph, v.behind), PrGpuAccessFlag_ShaderRead);
}

// Draws which draw and which triangle each pixel shows, for the opaque and masked draws.
void drawVisibility(void* PR_NULLABLE context, PrGpuPass* pass) {
    const ViewFrame& v = *static_cast<const ViewFrame*>(context);
    DrawBindings bound{};
    RenderPipelineKey key{.kind = RenderPipelineKind_Visibility, .samples = 1};
    drawAlpha(v, pass, &bound, kPrRenderMaterialOpaque, key, 0);
    key.mask = true;
    drawAlpha(v, pass, &bound, kPrRenderMaterialMask, key, 0);
}

// Shades each pixel of the view's color from the visibility buffer.
void drawShade(void* PR_NULLABLE context, PrGpuPass* pass) {
    ViewFrame& v = *static_cast<ViewFrame*>(context);
    PrGpuPipeline pipeline = renderPipelineFor(v.render, RenderPipelineKey{.kind = RenderPipelineKind_Shade, .samples = 1});
    if (pipeline.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    // The graph lends the visibility buffer its device texture only while it records.
    v.uniforms.visibility = prGpuGraphTexture(v.graph, v.visibility).id;
    // The pass may be the first of the command buffer to draw, as in a scene without draws.
    PrRenderDrawPush push{.view = v.view};
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetUniforms(commands, &v.uniforms, sizeof(v.uniforms));
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

// Declares that a pass reads the scene's records, and the vertices of its deforming instances, which the frame's
// deformation writes.
void useRecords(const Render* r, const RenderSceneFrame& frame, PrGpuGraphPass pass) {
    if (frame.deformed.id != 0)
        prGpuGraphUseBuffer(r->graph, pass, frame.deformed, PrGpuAccessFlag_ShaderRead);
    prGpuGraphUseBuffer(r->graph, pass, frame.instances, PrGpuAccessFlag_ShaderRead);
    prGpuGraphUseBuffer(r->graph, pass, frame.draws, PrGpuAccessFlag_ShaderRead);
    prGpuGraphUseBuffer(r->graph, pass, r->frameParts, PrGpuAccessFlag_ShaderRead);
    prGpuGraphUseBuffer(r->graph, pass, r->frameMaterials, PrGpuAccessFlag_ShaderRead);
}

// Declares that a pass draws what a view's cull kept.
void useDraws(const Render* r, const RenderSceneFrame& frame, PrGpuGraphPass pass, PrGpuGraphBuffer arguments) {
    useRecords(r, frame, pass);
    prGpuGraphUseBuffer(r->graph, pass, arguments, PrGpuAccessFlag_IndirectRead);
}

// Declares that a pass shades surfaces, which reads the shadow map if the scene has one.
void useShading(const Render* r, const RenderSceneFrame& frame, PrGpuGraphPass pass) {
    prGpuGraphUseTexture(r->graph, pass, frame.shadowMap, PrGpuAccessFlag_FragmentRead);
}

// Makes the buffer of a view's indirect arguments hold every draw of the scene, adds it to the graph, and adds the
// view's cull, which writes it.
PrGpuGraphBuffer addCull(Render* r, const RenderSceneFrame& frame, ViewFrame* v, uint32_t buffer, PrStringView name) {
    RenderSceneState* s = v->scene;
    uint64_t size = uint64_t(s->draws.count) * kPrRenderArgumentsBytes;
    renderRecordBufferReserve(r, &s->arguments[buffer], size,
        PrGpuBufferUsageFlag_Storage | PrGpuBufferUsageFlag_Indirect, "render arguments");
    v->arguments = s->arguments[buffer].buffer;
    v->uniforms.arguments = v->arguments.id != 0 ? prGpuBufferStorageHandle(r->device, v->arguments) : 0;
    PrGpuGraphBuffer arguments = prGpuGraphImportBuffer(r->graph, v->arguments, PrGpuAccessFlag_IndirectRead);
    PrGpuGraphPass cull = prGpuGraphAddWork(r->graph, name, recordCull, v);
    useRecords(r, frame, cull);
    prGpuGraphUseBuffer(r->graph, cull, arguments, PrGpuAccessFlag_ComputeWrite);
    return arguments;
}

ViewFrame* allocViewFrame(Render* r, RenderSceneState* s, const PrRenderFrameUniforms& uniforms, uint32_t view) {
    ViewFrame* v = static_cast<ViewFrame*>(prGpuGraphAlloc(r->graph, sizeof(ViewFrame)));
    *v = ViewFrame{.render = r, .scene = s, .graph = r->graph, .uniforms = uniforms, .view = view, .samples = 1};
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// The passes that a scene's views share

// Writes the scene's lights to the uniforms, and returns the light that casts the scene's shadows, or null.
const PrRenderLightDesc* PR_NULLABLE setLights(const RenderSceneState* s, PrRenderFrameUniforms* uniforms) {
    const PrRenderLightDesc* caster = nullptr;
    uniforms->shadowLight = kPrRenderMaxLights;
    uint32_t count = 0;
    for (uint32_t id = 1; id < s->lights.count && count < kPrRenderMaxLights; ++id) {
        if (!s->lights.data[id].alive)
            continue;
        const PrRenderLightDesc& light = s->lights.data[id].desc;
        PrVec3 direction = light.direction;
        float length = prVec3Length(direction);
        direction = length > 0 ? direction / length : PrVec3{0, -1, 0};
        // glTF's lightAngleScale and lightAngleOffset.
        float outer = prF32Cos(light.outerConeAngle);
        float scale = 1 / prF32Max(0.001f, prF32Cos(light.innerConeAngle) - outer);
        uniforms->lights[count] = PrRenderLightRecord{
            .position = {light.position.x, light.position.y, light.position.z, light.range},
            .direction = {direction.x, direction.y, direction.z, float(light.kind)},
            .color = {
                light.color.x * light.intensity,
                light.color.y * light.intensity,
                light.color.z * light.intensity,
                0,
            },
            .cone = {
                scale,
                -outer * scale,
                light.kind == PrRenderLightKind_Directional ? light.angularRadius : light.radius,
                0,
            },
        };
        if (caster == nullptr && light.kind == PrRenderLightKind_Directional && light.castsShadows) {
            caster = &light;
            uniforms->shadowLight = count;
        }
        ++count;
    }
    uniforms->lightCount = count;
    return caster;
}

// A box of a light's space: across the light's direction in x and y, and along it in z, which grows away from the
// light.
struct LightBox {
    PrVec3 low;
    PrVec3 high;
};

// Widens a box of the space that `toLight` maps the world to, so that it holds a sphere of the world.
void lightBoxAdd(LightBox* box, bool* any, const PrMatrix4& toLight, PrVec3 center, float radius) {
    const float (*m)[4] = toLight.m;
    PrVec3 at{
        m[0][0] * center.x + m[0][1] * center.y + m[0][2] * center.z + m[0][3],
        m[1][0] * center.x + m[1][1] * center.y + m[1][2] * center.z + m[1][3],
        -(m[2][0] * center.x + m[2][1] * center.y + m[2][2] * center.z + m[2][3]),
    };
    PrVec3 low = at - PrVec3{radius, radius, radius};
    PrVec3 high = at + PrVec3{radius, radius, radius};
    if (!*any) {
        *box = {low, high};
        *any = true;
    }
    box->low = {prF32Min(box->low.x, low.x), prF32Min(box->low.y, low.y), prF32Min(box->low.z, low.z)};
    box->high = {prF32Max(box->high.x, high.x), prF32Max(box->high.y, high.y), prF32Max(box->high.z, high.z)};
}

// Returns the box of a light's space that holds the bounds of every part that casts a shadow. The bounds are spheres,
// so the box is as tight across the light as they allow.
LightBox shadowCasterBounds(const Render* r, const RenderSceneState* s, const PrMatrix4& toLight) {
    LightBox box{{-1e-3f, -1e-3f, -1e-3f}, {1e-3f, 1e-3f, 1e-3f}};
    bool any = false;
    for (uint32_t i = 0; i < s->draws.count; ++i) {
        const PrRenderDrawRecord& draw = s->draws.data[i];
        const PrRenderInstanceRecord& instance = s->records.data[draw.instance];
        if ((draw.flags & kPrRenderDrawBlend) != 0 || (instance.visibility & kPrRenderVisibilityShadow) == 0)
            continue;
        const PrRenderVec4& bounds = r->parts.data[draw.part].bounds;
        PrVec3 center = worldPoint(instance, PrVec3{bounds.x, bounds.y, bounds.z});
        lightBoxAdd(&box, &any, toLight, center, bounds.w * worldScale(instance));
    }
    return box;
}

// Makes the scene's shadow map, if it has none. Returns false if the device makes none.
bool createShadowMap(Render* r, RenderSceneState* s) {
    if (s->shadowMap.id != 0)
        return true;
    uint32_t size = r->options.shadowMapSize;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(kRenderDepthFormat, PrUVec2{size, size},
        PrGpuTextureUsageFlag_DepthStencil | PrGpuTextureUsageFlag_Sampled);
    desc.name = "render shadow map";
    return prGpuTextureCreate(r->device, &desc, &s->shadowMap) == PrStatus_Ok;
}

// The steps that the half width of a shadow map's view takes between one power of two and the next.
constexpr float kShadowWidthSteps = 32;

// Adds the light's cull and the pass that draws the shadow map, and writes the light's view to the uniforms.
//
// The light's view is the box that holds what the map covers: the sphere that the light names, or else the bounds of
// what casts shadows. The box is square across the light, so that a texel is, and its width and its place are rounded:
// the half width up to a step between powers of two, and the middle to a whole texel. So while the bounds move or
// change a little, as an animation's do, the texels stay where they were in the world, and a shadow's edge does not
// crawl.
void addShadowMap(Render* r, RenderSceneState* s, RenderSceneFrame* frame, const PrRenderLightDesc& light) {
    PrRenderFrameUniforms& u = frame->uniforms;
    PrRenderVec4 shines = u.lights[u.shadowLight].direction;
    PrMatrix4 lightView = prMatrix4LookAt(PrVec3{-shines.x, -shines.y, -shines.z}, PrVec3{});
    LightBox box{};
    if (light.shadowBounds.radius > 0) {
        bool any = false;
        lightBoxAdd(&box, &any, lightView, light.shadowBounds.center, light.shadowBounds.radius);
    } else {
        box = shadowCasterBounds(r, s, lightView);
    }
    float size = float(r->options.shadowMapSize);
    // A texel to each side is room for the rounding of the middle.
    float half = prF32Max(box.high.x - box.low.x, box.high.y - box.low.y) * 0.5f * (1 + 2 / size);
    float step = prF32Exp2(prF32Ceil(prF32Log2(half))) / kShadowWidthSteps;
    half = prF32Ceil(half / step) * step;
    float texel = 2 * half / size;
    lightView.m[0][3] -= prF32Round((box.low.x + box.high.x) * 0.5f / texel) * texel;
    lightView.m[1][3] -= prF32Round((box.low.y + box.high.y) * 0.5f / texel) * texel;
    // The distances are exchanged, so the map's depth grows with the distance from the light.
    PrMatrix4 lightProjection = prMatrix4OrthographicProjection(PrVec2{half, half}, box.high.z + texel, box.low.z - texel);
    setView(&u, kPrRenderViewLight, prMatrix4Concat(&lightView, &lightProjection));
    u.shadowSize = size;
    u.shadowTexel = texel;
    u.shadowDepth = box.high.z - box.low.z + 2 * texel;
    u.shadowSpread = prF32Tan(prF32Clamp(light.angularRadius, 0, 1));
    u.shadowMap = s->shadowMap.id;
    u.lightMask = kPrRenderVisibilityShadow;

    // The shadow map rests in the accesses of shader reads between frames.
    frame->shadowMap = prGpuGraphImportTexture(r->graph, s->shadowMap, PrGpuAccessFlag_ShaderRead);
    prGpuGraphLeaveTexture(r->graph, frame->shadowMap, PrGpuAccessFlag_ShaderRead);
    ViewFrame* v = allocViewFrame(r, s, u, kPrRenderViewLight);
    PrGpuGraphBuffer arguments = addCull(r, *frame, v, 0, "render cull light");
    PrGpuGraphPassDesc desc{
        .name = "render shadow map",
        .viewCount = 1,
        .depthStencil = {.texture = frame->shadowMap, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep},
        .clearDepth = 1,
    };
    PrGpuGraphPass shadow = prGpuGraphAddPass(r->graph, &desc, drawDepth, v);
    useDraws(r, *frame, shadow, arguments);
}

// ---------------------------------------------------------------------------------------------------------------------
// Techniques

// What a technique's passes take from the passes before them.
struct ViewInputs {
    const RenderSceneFrame* frame;
    ViewFrame* view;
    PrGpuGraphBuffer arguments;
    PrUVec2 extent;
    bool keepsDepth;
};

// The depth of a view at a draw, for the order of blended draws.
struct BlendDepth {
    uint32_t draw;
    float depth;
};

// Orders the view's blended draws from the farthest to the nearest, by the depth of the center of each part's bounds.
void orderBlended(Render* r, ViewFrame* v) {
    const RenderSceneState* s = v->scene;
    uint32_t first = s->bucketFirst[kPrRenderMaterialBlend * kRenderCullCount];
    uint32_t count = s->draws.count - first;
    if (count == 0)
        return;
    RawArray<BlendDepth> depths{};
    depths.resize(count);
    const PrRenderVec4& row = v->uniforms.viewProjection[kPrRenderViewCamera * 4 + 3];
    for (uint32_t i = 0; i < count; ++i) {
        const PrRenderDrawRecord& draw = s->draws.data[first + i];
        const PrRenderVec4& bounds = r->parts.data[draw.part].bounds;
        PrVec3 center = worldPoint(s->records.data[draw.instance], PrVec3{bounds.x, bounds.y, bounds.z});
        depths.data[i] = {first + i, row.x * center.x + row.y * center.y + row.z * center.z + row.w};
    }
    sortItems(Span<BlendDepth>(depths.data, count), [](const BlendDepth& a, const BlendDepth& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.draw < b.draw;
    });
    uint32_t* order = static_cast<uint32_t*>(prGpuGraphAlloc(r->graph, size_t(count) * sizeof(uint32_t)));
    for (uint32_t i = 0; i < count; ++i)
        order[i] = depths.data[i].draw;
    depths.reset();
    v->blendOrder = order;
    v->blendCount = count;
}

PrGpuTextureDesc depthDesc(const ViewInputs& inputs, uint8_t samples) {
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_DepthStencil;
    if (inputs.keepsDepth)
        usage |= PrGpuTextureUsageFlag_Sampled;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(kRenderDepthFormat, inputs.extent, usage);
    desc.samples = samples;
    return desc;
}

// Returns the most samples per pixel, of 8, 4, 2 and 1, that are no more than `samples` and that the device has.
uint8_t supportedSamples(const Render* r, uint8_t samples) {
    uint32_t counts = prGpuDeviceInfo(r->device).sampleCounts;
    for (uint8_t count = 8; count > 1; count /= 2) {
        if (count <= samples && (counts & count) != 0)
            return count;
    }
    return 1;
}

// Returns what the last pass that draws a view's depth does with it.
PrGpuStore depthStore(const ViewInputs& inputs) {
    if (inputs.keepsDepth)
        return PrGpuStore_Keep;
    return PrGpuStore_Discard;
}

// Adds the texture of the scene behind a view's blended draws, if the view's scene has a draw that reads it.
void addBehind(Render* r, ViewFrame* v, PrUVec2 extent) {
    v->extent = extent;
    if (v->scene->behindDraws == 0 || v->blendCount == 0)
        return;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(kRenderColorFormat, extent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    desc.levels = prGpuMipLevels(extent);
    v->behind = prGpuGraphAddTexture(r->graph, &desc);
}

// Adds the work that fills the scene behind a view's blended draws: a copy of `source`, the view's color, into its
// first level, unless a pass resolved into that level and `source` is none, and then its other levels.
void addBehindFill(Render* r, ViewFrame* v, PrGpuGraphTexture source) {
    PrGpuGraph* graph = r->graph;
    if (source.id != 0) {
        v->behindSource = source;
        PrGpuGraphPass copy = prGpuGraphAddWork(graph, "render behind copy", recordBehindCopy, v);
        prGpuGraphUseTexture(graph, copy, source, PrGpuAccessFlag_CopyRead);
        prGpuGraphUseTexture(graph, copy, v->behind, PrGpuAccessFlag_CopyWrite);
    }
    PrGpuGraphPass levels = prGpuGraphAddWork(graph, "render behind levels", recordBehindLevels, v);
    prGpuGraphUseTexture(graph, levels, v->behind, PrGpuAccessFlag_ShaderRead);
}

// Adds the forward technique's passes: the view's depth, and then its surfaces, which shade where they are the nearest
// that the depth pass drew. With more than one sample, the color resolves into the view's color.
//
// A view of a scene with a surface that reads the scene behind it draws in two passes. The first draws what does not
// blend, and its color becomes that scene: with more than one sample it resolves into the texture and keeps its
// samples. The second draws the blended draws over the first's color and depth.
PrRenderViewTextures addForward(Render* r, const ViewInputs& inputs) {
    ViewFrame* v = inputs.view;
    PrGpuGraph* graph = r->graph;
    addBehind(r, v, inputs.extent);
    bool behind = v->behind.id != 0;
    PrGpuTextureDesc viewColorDesc = prGpuTextureDesc2d(kRenderColorFormat, inputs.extent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    PrGpuTextureDesc depthTextureDesc = depthDesc(inputs, v->samples);
    PrGpuGraphTexture viewColor = prGpuGraphAddTexture(graph, &viewColorDesc);
    PrGpuGraphTexture depth = prGpuGraphAddTexture(graph, &depthTextureDesc);

    PrGpuGraphPassDesc depthPassDesc{
        .name = "render depth",
        .viewCount = 1,
        .depthStencil = {.texture = depth, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep},
        .clearDepth = 0,
    };
    PrGpuGraphPass depthPass = prGpuGraphAddPass(graph, &depthPassDesc, drawDepth, v);
    useDraws(r, *inputs.frame, depthPass, inputs.arguments);

    PrGpuGraphPassDesc surfaceDesc{
        .name = "render surfaces",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = viewColor, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
        .depthStencil = {.texture = depth, .load = PrGpuLoad_Keep, .store = depthStore(inputs)},
    };
    if (behind)
        surfaceDesc.depthStencil.store = PrGpuStore_Keep;
    PrGpuGraphTexture color{};
    if (v->samples > 1) {
        PrGpuTextureDesc colorDesc =
            prGpuTextureDesc2d(kRenderColorFormat, inputs.extent, PrGpuTextureUsageFlag_RenderTarget);
        colorDesc.samples = v->samples;
        color = prGpuGraphAddTexture(graph, &colorDesc);
        surfaceDesc.color[0] = {
            .texture = color,
            .resolve = viewColor,
            .load = PrGpuLoad_Clear,
            .store = PrGpuStore_Resolve,
        };
        if (behind) {
            surfaceDesc.color[0].resolve = v->behind;
            surfaceDesc.color[0].store = PrGpuStore_ResolveKeep;
        }
    }
    PrGpuGraphPass surfaces = prGpuGraphAddPass(graph, &surfaceDesc, drawForward, v);
    useDraws(r, *inputs.frame, surfaces, inputs.arguments);
    useShading(r, *inputs.frame, surfaces);
    if (behind) {
        addBehindFill(r, v, v->samples > 1 ? PrGpuGraphTexture{} : viewColor);
        PrGpuGraphPassDesc blendDesc{
            .name = "render blended",
            .viewCount = 1,
            .colorCount = 1,
            .color = {{.texture = viewColor, .load = PrGpuLoad_Keep, .store = PrGpuStore_Keep}},
            .depthStencil = {.texture = depth, .load = PrGpuLoad_Keep, .store = depthStore(inputs)},
        };
        if (v->samples > 1) {
            blendDesc.color[0] = {
                .texture = color,
                .resolve = viewColor,
                .load = PrGpuLoad_Keep,
                .store = PrGpuStore_Resolve,
            };
        }
        PrGpuGraphPass blended = prGpuGraphAddPass(graph, &blendDesc, drawBlendedOver, v);
        useDraws(r, *inputs.frame, blended, inputs.arguments);
        useShading(r, *inputs.frame, blended);
        prGpuGraphUseTexture(graph, blended, v->behind, PrGpuAccessFlag_FragmentRead);
    }
    return PrRenderViewTextures{.color = viewColor, .depth = inputs.keepsDepth ? depth : PrGpuGraphTexture{}};
}

// Adds the visibility technique's passes: one that draws which triangle each pixel shows, one that shades each pixel of
// the view's color from it, and one that draws the blended draws over that color as the forward technique does.
PrRenderViewTextures addVisibility(Render* r, const ViewInputs& inputs) {
    ViewFrame* v = inputs.view;
    PrGpuGraph* graph = r->graph;
    PrGpuTextureUsageFlags sampledTarget = PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled;
    PrGpuTextureDesc visibilityDesc = prGpuTextureDesc2d(kRenderVisibilityFormat, inputs.extent, sampledTarget);
    PrGpuTextureDesc depthTextureDesc = depthDesc(inputs, 1);
    PrGpuTextureDesc viewColorDesc = prGpuTextureDesc2d(kRenderColorFormat, inputs.extent, sampledTarget);
    v->visibility = prGpuGraphAddTexture(graph, &visibilityDesc);
    PrGpuGraphTexture depth = prGpuGraphAddTexture(graph, &depthTextureDesc);
    PrGpuGraphTexture viewColor = prGpuGraphAddTexture(graph, &viewColorDesc);
    addBehind(r, v, inputs.extent);
    // The blended draws test the depth that the visibility pass wrote.
    bool blends = v->blendCount != 0;
    PrGpuStore lastStore = depthStore(inputs);
    PrGpuStore firstStore = lastStore;
    if (blends)
        firstStore = PrGpuStore_Keep;

    PrGpuGraphPassDesc visibilityPassDesc{
        .name = "render visibility",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = v->visibility, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
        .depthStencil = {.texture = depth, .load = PrGpuLoad_Clear, .store = firstStore},
        .clearDepth = 0,
    };
    PrGpuGraphPass drawn = prGpuGraphAddPass(graph, &visibilityPassDesc, drawVisibility, v);
    useDraws(r, *inputs.frame, drawn, inputs.arguments);

    PrGpuGraphPassDesc shadeDesc{
        .name = "render shade",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = viewColor, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphPass shaded = prGpuGraphAddPass(graph, &shadeDesc, drawShade, v);
    useRecords(r, *inputs.frame, shaded);
    useShading(r, *inputs.frame, shaded);
    prGpuGraphUseTexture(graph, shaded, v->visibility, PrGpuAccessFlag_FragmentRead);

    if (blends) {
        // The shaded color is the scene behind the blended draws that read it.
        if (v->behind.id != 0)
            addBehindFill(r, v, viewColor);
        PrGpuGraphPassDesc blendDesc{
            .name = "render blended",
            .viewCount = 1,
            .colorCount = 1,
            .color = {{.texture = viewColor, .load = PrGpuLoad_Keep, .store = PrGpuStore_Keep}},
            .depthStencil = {.texture = depth, .load = PrGpuLoad_Keep, .store = lastStore},
        };
        PrGpuGraphPass blended = prGpuGraphAddPass(graph, &blendDesc, drawBlendedOver, v);
        useDraws(r, *inputs.frame, blended, inputs.arguments);
        useShading(r, *inputs.frame, blended);
        if (v->behind.id != 0)
            prGpuGraphUseTexture(graph, blended, v->behind, PrGpuAccessFlag_FragmentRead);
    }
    return PrRenderViewTextures{.color = viewColor, .depth = inputs.keepsDepth ? depth : PrGpuGraphTexture{}};
}

} // namespace

RenderSceneFrame* renderSceneFrame(Render* render, RenderSceneState* scene) {
    Render* r = render;
    RenderSceneState* s = scene;
    if (s->frameSerial == r->frameSerial && s->frame != nullptr)
        return s->frame;
    RenderSceneFrame* frame = static_cast<RenderSceneFrame*>(prGpuGraphAlloc(r->graph, sizeof(RenderSceneFrame)));
    *frame = RenderSceneFrame{};
    s->frameSerial = r->frameSerial;
    s->frame = frame;
    frame->instances = prGpuGraphImportBuffer(r->graph, s->instanceBuffer.buffer, PrGpuAccessFlag_ShaderRead);
    frame->draws = prGpuGraphImportBuffer(r->graph, s->drawBuffer.buffer, PrGpuAccessFlag_ShaderRead);
    renderDeformAddPass(r, s, frame);

    PrRenderFrameUniforms& u = frame->uniforms;
    renderEnvironmentUniforms(r, s->environment, &u);
    u.time = s->time;
    u.drawCount = s->draws.count;
    u.instances = s->instanceBuffer.buffer.id;
    u.draws = s->drawBuffer.buffer.id;
    u.parts = r->partBuffer.buffer.id;
    u.materials = r->materialBuffer.buffer.id;
    u.tables = r->tables.id;
    u.shadowMap = r->white.id;
    // A pass without the scene behind its surfaces has a texture that no shader reads.
    u.behind = r->white.id;
    u.behindSampler = r->whiteSampler.id;
    u.behindLastLevel = -1;
    u.exposure = 1;
    const PrRenderLightDesc* caster = setLights(s, &u);
    if (caster != nullptr && createShadowMap(r, s))
        addShadowMap(r, s, frame, *caster);
    else
        u.shadowLight = kPrRenderMaxLights;
    return frame;
}

} // namespace pr_impl

using namespace pr_impl;

bool prRenderSupportsTechnique(const PrRender* render, PrRenderTechnique technique) {
    return technique == PrRenderTechnique_Forward || render->bindless;
}

PrRenderCamera prRenderCameraLookAt(PrVec3 eye, PrVec3 target, float verticalFov, float aspect, float nearDistance) {
    return PrRenderCamera{
        .view = prMatrix4LookAt(eye, target),
        .projection = prMatrix4PerspectiveProjection(verticalFov, aspect, nearDistance),
    };
}

PrRenderViewDesc prRenderViewDescDefault(PrRenderScene scene) {
    return PrRenderViewDesc{
        .scene = scene,
        .camera = {.view = prMatrix4Identity(), .projection = prMatrix4Identity()},
        .technique = PrRenderTechnique_Forward,
        .samples = 4,
        .mask = 0xffffffffu,
        .exposure = 1,
        .shadows = true,
    };
}

PrRenderViewTextures prRenderViewAdd(PrRender* render, const PrRenderViewDesc* desc) {
    Render* r = render;
    RenderSceneState* s = renderSceneFind(r, desc->scene.id);
    if (s == nullptr || r->graph == nullptr || desc->extent.x == 0 || desc->extent.y == 0)
        return PrRenderViewTextures{};
    RenderSceneFrame* frame = renderSceneFrame(r, s);
    PR_ASSERT(frame->viewCount < kPrRenderMaxViews);
    if (frame->viewCount >= kPrRenderMaxViews)
        return PrRenderViewTextures{};
    uint32_t slot = frame->viewCount++;

    ViewFrame* v = allocViewFrame(r, s, frame->uniforms, kPrRenderViewCamera);
    PrRenderFrameUniforms& u = v->uniforms;
    setView(&u, kPrRenderViewCamera, prMatrix4Concat(&desc->camera.view, &desc->camera.projection));
    // The camera stands where its view matrix maps to the origin: at minus the matrix's translation, turned back to the
    // world.
    const float (*m)[4] = desc->camera.view.m;
    u.cameraPosition = {
        -(m[0][0] * m[0][3] + m[1][0] * m[1][3] + m[2][0] * m[2][3]),
        -(m[0][1] * m[0][3] + m[1][1] * m[1][3] + m[2][1] * m[2][3]),
        -(m[0][2] * m[0][3] + m[1][2] * m[1][3] + m[2][2] * m[2][3]),
        1,
    };
    u.targetExtent = {float(desc->extent.x), float(desc->extent.y), 0, 0};
    setBackgroundRay(&u, desc->camera);
    // The background's ray is its middle column at the target's middle, and moves by its first column from there to the
    // target's right edge.
    float across = prF32Sqrt(u.backgroundRay[0].x * u.backgroundRay[0].x + u.backgroundRay[1].x * u.backgroundRay[1].x +
        u.backgroundRay[2].x * u.backgroundRay[2].x);
    float ahead = prF32Sqrt(u.backgroundRay[0].z * u.backgroundRay[0].z + u.backgroundRay[1].z * u.backgroundRay[1].z +
        u.backgroundRay[2].z * u.backgroundRay[2].z);
    u.pixelSpread = ahead > 0 ? across / ahead * 2 / float(desc->extent.x) : 0;
    u.cameraMask = desc->mask & ~kPrRenderVisibilityShadow;
    u.exposure = desc->exposure > 0 ? desc->exposure : 1;
    if (!desc->shadows)
        u.shadowLight = kPrRenderMaxLights;
    v->shadowMap = PrGpuTexture{u.shadowMap};
    v->environment = PrGpuTexture{u.environment};
    bool visibility = desc->technique == PrRenderTechnique_Visibility && r->bindless;
    u.cameraPulls = visibility ? 1 : 0;
    v->samples = visibility ? 1 : supportedSamples(r, desc->samples);
    orderBlended(r, v);

    ViewInputs inputs{
        .frame = frame,
        .view = v,
        .arguments = addCull(r, *frame, v, 1 + slot, "render cull"),
        .extent = desc->extent,
        .keepsDepth = desc->keepsDepth,
    };
    PrRenderViewTextures textures = visibility ? addVisibility(r, inputs) : addForward(r, inputs);
    if (desc->meter.id != 0)
        renderMeterAdd(r, desc->meter.id, textures.color, desc->extent, u.exposure);
    return textures;
}

