// A renderer's pipelines: what each kind's states are, and the table that holds the ones made.

#include "render/src/render_pipeline.h"

#include "pr/assert.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

uint64_t packKey(const RenderPipelineKey& key) {
    return uint64_t(key.kind) | uint64_t(key.cull) << 4 | uint64_t(key.samples) << 8 | uint64_t(key.shadow) << 12 |
        uint64_t(key.blend) << 13 | uint64_t(key.mask) << 14 | uint64_t(key.format) << 16 | uint64_t(key.variant) << 32 |
        uint64_t(key.layout) << 48;
}

// Returns the vertex format of lib/gpu that fetches an attribute of `format`, which the build checked is one.
PrGpuVertexFormat vertexFormatOf(PrMeshFormat format) {
    uint32_t count = format.count;
    switch (format.component) {
        case PrMeshComponent_Float32:
            return PrGpuVertexFormat(PrGpuVertexFormat_Float + count - 1);
        case PrMeshComponent_Float16:
            return count == 2 ? PrGpuVertexFormat_Half2 : PrGpuVertexFormat_Half4;
        case PrMeshComponent_Uint32:
            return PrGpuVertexFormat(PrGpuVertexFormat_Uint + count - 1);
        case PrMeshComponent_Int32:
            return PrGpuVertexFormat(PrGpuVertexFormat_Sint + count - 1);
        case PrMeshComponent_Uint8:
            return format.normalized ? PrGpuVertexFormat_Unorm8x4 : PrGpuVertexFormat_Uint8x4;
        case PrMeshComponent_Int8:
            return PrGpuVertexFormat_Snorm8x4;
        case PrMeshComponent_Uint16:
            return count == 2 ? PrGpuVertexFormat_Unorm16x2 : PrGpuVertexFormat_Unorm16x4;
        case PrMeshComponent_Int16:
            return count == 2 ? PrGpuVertexFormat_Snorm16x2 : PrGpuVertexFormat_Snorm16x4;
        default:
            PR_ASSERT(false);
            return PrGpuVertexFormat_None;
    }
}

// Sets a pipeline's vertex layout to a mesh's geometry, or its position alone, and with a layout the attributes of that
// layout from a second buffer, at the locations after the geometry's.
void setVertexLayout(const Render* r, PrGpuGraphicsPipelineDesc* desc, bool positionOnly, uint32_t layout) {
    PrGpuVertexLayout& v = desc->vertexLayout;
    v.bufferCount = 1;
    v.buffers[0] = {.stride = kPrRenderGeometryBytes, .step = PrGpuVertexStep_Vertex};
    v.attributes[0] = {.format = PrGpuVertexFormat_Float3, .buffer = 0, .offset = 0};
    v.attributes[1] = {.format = PrGpuVertexFormat_Snorm16x4, .buffer = 0, .offset = 12};
    v.attributes[2] = {.format = PrGpuVertexFormat_Snorm16x4, .buffer = 0, .offset = 20};
    v.attributeCount = positionOnly ? 1 : 3;
    if (positionOnly || layout == 0)
        return;
    const PrRenderVertexLayout& described = r->shaders->layouts[layout - 1];
    if (described.attributeCount == 0)
        return;
    PR_ASSERT(3 + described.attributeCount <= kPrGpuMaxVertexAttributes);
    v.bufferCount = 2;
    v.buffers[1] = {.stride = described.stride, .step = PrGpuVertexStep_Vertex};
    for (uint32_t i = 0; i < described.attributeCount; ++i) {
        const PrRenderVertexAttribute& attribute = described.attributes[i];
        v.attributes[3 + i] = {
            .format = vertexFormatOf(attribute.format),
            .buffer = 1,
            .offset = uint16_t(attribute.offset),
        };
    }
    v.attributeCount = uint8_t(3 + described.attributeCount);
}

// Sets the states of a pipeline that draws meshes with a depth test. A triangle faces front if its vertices run
// counterclockwise on the target, or clockwise for a mirrored instance. The light's view keeps both faces of a mesh, so
// that a mesh shadows what lies behind its back faces too.
void setMeshStates(PrGpuGraphicsPipelineDesc* desc, const RenderPipelineKey& key) {
    desc->depthTest = true;
    desc->formats.depthStencil = kRenderDepthFormat;
    desc->formats.samples = key.samples;
    desc->frontClockwise = (key.cull & RenderCullFlag_Mirrored) != 0;
    desc->cull = (key.cull & RenderCullFlag_DoubleSided) != 0 || key.shadow ? PrGpuCull_None : PrGpuCull_Back;
}

PrGpuGraphicsPipelineDesc describePipeline(const Render* r, const RenderPipelineKey& key) {
    switch (key.kind) {
        case RenderPipelineKind_Depth:
        case RenderPipelineKind_DepthMask: {
            bool mask = key.kind == RenderPipelineKind_DepthMask;
            const PrGpuShaderLibrary* library = r->libraries[mask ? RenderLibrary_DepthMask : RenderLibrary_Depth];
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
            setVertexLayout(r, &desc, !mask, key.layout);
            setMeshStates(&desc, key);
            desc.vertex = "depthVertex";
            if (mask) {
                const PrRenderVertexLayout& layout = r->shaders->layouts[key.layout - 1];
                desc.vertex = layout.depthMaskVertex;
                desc.fragment = layout.depthMaskFragment;
            }
            desc.depthWrite = true;
            desc.depthCompare = key.shadow ? PrGpuCompare_Less : PrGpuCompare_Greater;
            desc.name = "render depth";
            return desc;
        }
        case RenderPipelineKind_Forward: {
            // Surfaces shade only where they are the nearest that the depth pass drew. The fragment shader is the one
            // that the shader set compiled for the material type.
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Forward]);
            setVertexLayout(r, &desc, false, key.layout);
            setMeshStates(&desc, key);
            desc.vertex = r->shaders->layouts[key.layout - 1].forwardVertex;
            desc.fragment = r->shaders->variants[key.variant].forwardFragment;
            desc.depthCompare = PrGpuCompare_GreaterEqual;
            desc.formats.colorCount = 1;
            desc.formats.color[0] = kRenderColorFormat;
            if (key.blend)
                desc.blend[0] = prGpuBlendPremultiplied();
            desc.name = "render forward";
            return desc;
        }
        case RenderPipelineKind_Background: {
            // The background lies at the far plane, whose depth is 0: it passes only where no surface was drawn.
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Background]);
            desc.vertex = "backgroundVertex";
            desc.fragment = "backgroundPixel";
            desc.depthTest = true;
            desc.depthCompare = PrGpuCompare_GreaterEqual;
            desc.formats.colorCount = 1;
            desc.formats.color[0] = kRenderColorFormat;
            desc.formats.depthStencil = kRenderDepthFormat;
            desc.formats.samples = key.samples;
            desc.name = "render background";
            return desc;
        }
        case RenderPipelineKind_Visibility: {
            // The visibility pass pulls its vertices, so its pipeline has no vertex layout. A masked draw's fragment
            // shader finds its point on the triangle and the attributes there, as the shading does, and tests its
            // alpha.
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Visibility]);
            setMeshStates(&desc, key);
            desc.vertex = "visibilityVertex";
            desc.fragment = key.mask ? "visibilityMaskPixel" : "visibilityPixel";
            desc.depthWrite = true;
            desc.depthCompare = PrGpuCompare_Greater;
            desc.formats.colorCount = 1;
            desc.formats.color[0] = kRenderVisibilityFormat;
            desc.name = "render visibility";
            return desc;
        }
        case RenderPipelineKind_Shade: {
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Visibility]);
            desc.vertex = "shadeVertex";
            desc.fragment = "shadePixel";
            desc.formats.colorCount = 1;
            desc.formats.color[0] = kRenderColorFormat;
            desc.name = "render shade";
            return desc;
        }
        case RenderPipelineKind_Meter: {
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Meter]);
            desc.vertex = "meterVertex";
            desc.fragment = "meterPixel";
            desc.formats.colorCount = 1;
            desc.formats.color[0] = kRenderColorFormat;
            desc.name = "render meter";
            return desc;
        }
        default: {
            PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(r->libraries[RenderLibrary_Display]);
            desc.vertex = "displayVertex";
            desc.fragment = "displayPixel";
            desc.formats.colorCount = 1;
            desc.formats.color[0] = key.format;
            desc.name = "render display";
            return desc;
        }
    }
}

// Sets `out` to the pipeline of a packed key and returns true, if the renderer has made one. The caller holds the
// pipelines' lock.
bool findPipeline(const Render* r, uint64_t packed, PrGpuPipeline* out) {
    for (uint32_t i = 0; i < r->pipelines.count; ++i) {
        if (r->pipelines.data[i].packed == packed) {
            *out = r->pipelines.data[i].pipeline;
            return true;
        }
    }
    return false;
}

} // namespace

PrGpuPipeline renderPipelineFor(Render* render, const RenderPipelineKey& key) {
    Render* r = render;
    uint64_t packed = packKey(key);
    PrGpuPipeline pipeline{};
    prMutexLock(&r->pipelineLock);
    bool found = findPipeline(r, packed, &pipeline);
    prMutexUnlock(&r->pipelineLock);
    if (found)
        return pipeline;

    // The pipeline is made outside the lock, which a frame takes for each pipeline that it draws with. Two threads that
    // need one pipeline at once may each make it, and the second to finish frees its own.
    PrGpuGraphicsPipelineDesc desc = describePipeline(r, key);
    PrGpuPipeline made{};
    PrTimePerf began = prTimePerfNow();
    prGpuPipelineCreateGraphics(r->device, &desc, &made);
    double seconds = prTimePerfSeconds(began, prTimePerfNow());

    prMutexLock(&r->pipelineLock);
    r->pipelineSeconds += seconds;
    found = findPipeline(r, packed, &pipeline);
    if (!found) {
        r->pipelines.push(RenderPipeline{.key = key, .packed = packed, .pipeline = made});
        pipeline = made;
    }
    prMutexUnlock(&r->pipelineLock);
    if (found)
        prGpuPipelineRelease(r->device, made);
    return pipeline;
}

void renderPipelinesWarm(Render* render, const PrRenderMaterialDesc& desc) {
    Render* r = render;
    // A material that reads the scene behind it is drawn with the blended draws.
    bool blend = desc.alphaMode == PrRenderAlphaMode_Blend || (desc.flags & PrRenderMaterialFlag_ReadsBehind) != 0;
    bool mask = desc.alphaMode == PrRenderAlphaMode_Mask;
    RenderCullFlags cull = (desc.flags & PrRenderMaterialFlag_DoubleSided) != 0 ? RenderCullFlag_DoubleSided : 0;
    PR_ASSERT(desc.attributes < r->shaders->materialTypes[desc.type].variantCount);
    uint32_t variant = r->shaders->materialTypes[desc.type].firstVariant + desc.attributes;
    uint32_t layout = renderPipelineLayout(r, variant);

    // Each pipeline that the renderer has says a state that some pass draws in. The material's draws need, in that
    // state, the pipeline of their own kind, culling and variant.
    RawArray<RenderPipelineKey> needed{};
    prMutexLock(&r->pipelineLock);
    for (uint32_t i = 0; i < r->pipelines.count; ++i) {
        RenderPipelineKey key = r->pipelines.data[i].key;
        key.cull = cull;
        switch (key.kind) {
            case RenderPipelineKind_Depth:
            case RenderPipelineKind_DepthMask:
                // A pass that draws depth draws every material that does not blend.
                if (blend)
                    continue;
                key.kind = mask ? RenderPipelineKind_DepthMask : RenderPipelineKind_Depth;
                key.layout = mask ? layout : 0;
                break;
            case RenderPipelineKind_Forward:
                // A pass that shades opaque surfaces as it draws them shades blended ones too. A pass that draws
                // blended surfaces alone, over another technique's color, says nothing of the opaque ones.
                if (key.blend && !blend)
                    continue;
                key.blend = blend;
                key.variant = variant;
                key.layout = layout;
                break;
            case RenderPipelineKind_Visibility:
                if (blend)
                    continue;
                key.mask = mask;
                break;
            default:
                continue;
        }
        needed.push(key);
    }
    prMutexUnlock(&r->pipelineLock);
    for (uint32_t i = 0; i < needed.count; ++i)
        renderPipelineFor(r, needed.data[i]);
    needed.reset();
}

void renderPipelinesReset(Render* render) {
    for (uint32_t i = 0; i < render->pipelines.count; ++i)
        prGpuPipelineRelease(render->device, render->pipelines.data[i].pipeline);
    render->pipelines.reset();
}

} // namespace pr_impl
