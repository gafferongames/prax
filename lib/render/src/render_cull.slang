// lib/render's cull: a dispatch that writes the indirect arguments of a view's draws, one for each of the scene's
// draws.

#include "pr/render_scene.slang"

#if defined(PR_RENDER_SLOTS)

PR_GPU_RW_BUFFER(RWByteAddressBuffer, renderSlotArguments, 0);

// Writes a word of the draws' arguments at a byte offset.
void renderStoreArgument(uint offset, uint value) {
    renderSlotArguments.Store(offset, value);
}

#else

void renderStoreArgument(uint offset, uint value) {
    prGpuGet<RWByteAddressBuffer>(frame.arguments).Store(offset, value);
}

#endif

// Returns true if any of a sphere in the world lies inside a view's frustum.
bool viewSeesSphere(uint view, float3 center, float radius) {
    bool sees = true;
    for (uint i = 0; i < kPrRenderViewPlanes; ++i) {
        float4 plane = frame.planes[view * kPrRenderViewPlanes + i];
        sees = sees && dot(plane.xyz, center) + plane.w >= -radius;
    }
    return sees;
}

// Writes the arguments of a draw for a view. The draw has one instance of the part if the view's mask shares a bit with
// the instance's and the view sees the part's bounds, and none otherwise. A blended draw casts no shadow, so the
// light's view never has it.
//
// The arguments are those of an indexed draw of the part's indices. A draw that is not indexed reads their first four
// words, which then start at vertex zero: such a draw pulls its vertices, and counts them from zero on every device
// only if its first vertex is zero.
void writeDraw(uint view, uint index, uint argumentsOffset) {
    PrRenderDrawRecord draw = renderDraw(index);
    PrRenderInstanceRecord instance = renderInstance(draw.instance);
    PrRenderPartRecord part = renderPart(draw.part);
    bool blends = (draw.flags & kPrRenderDrawBlend) != 0;
    uint mask = view == kPrRenderViewCamera ? frame.cameraMask : frame.lightMask;
    float3 center = renderWorldPoint(instance, part.bounds.xyz);
    bool sees = (instance.visibility & mask) != 0 && !(blends && view == kPrRenderViewLight) &&
        viewSeesSphere(view, center, part.bounds.w * renderWorldScale(instance));
    bool pulls = view == kPrRenderViewCamera && frame.cameraPulls != 0 && !blends;
    uint at = argumentsOffset + index * kPrRenderArgumentsBytes;
    renderStoreArgument(at, part.indexCount);
    renderStoreArgument(at + 4, sees ? 1 : 0);
    renderStoreArgument(at + 8, pulls ? 0 : part.firstIndex);
    renderStoreArgument(at + 12, 0);
    renderStoreArgument(at + 16, 0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cullDraws(uint3 id : SV_DispatchThreadID) {
    if (id.x < frame.drawCount)
        writeDraw(push.view, id.x, push.argumentsOffset);
}
