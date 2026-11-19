// The scene behind a surface, as a shading model reads it: the camera's view of the scene's surfaces that do not blend,
// with the background, in a texture whose every level is a blur of the level before it. A surface that transmits light
// shows it, bent and blurred.
//
// It is a resource of the frame that a material opts into. A material that is set with `readsBehind`
// (PrRenderMaterialDesc) is drawn after the surfaces that do not blend, with the blended draws and in their order, in a
// pass that has the texture. The renderer makes the texture only for a view of a scene that has such a material. A
// model asks renderBehindAvailable before it reads, since a surface of its material type may be drawn in a pass without
// the texture: a material that was not set to read it, or a view that the renderer shades otherwise.
//
// On a device without bindless the texture takes the last of a draw's material texture slots
// (kPrRenderSlotMaterialTextures - 1), so a material that reads the scene behind it samples one texture fewer there.
//
// What the texture holds is the opaque scene alone. A surface that reads it does not show another such surface, or a
// blended one, behind it.

#pragma once

#include "pr/render_material.slang"
#include "pr/render_scene.slang"

// Returns true if the pass that shades the surface has the scene behind it.
bool renderBehindAvailable() {
    return frame.behindLastLevel >= 0;
}

#if defined(PR_RENDER_SLOTS)

float3 renderBehindSample(float2 uv, float level) {
    return renderSlotMaterialTexture4.SampleLevel(renderSlotMaterialTexture4Sampler, uv, level).rgb;
}

#else

// Returns the scene's radiance at a place of the camera's target, from 0 to 1 across it and down it, at a level of the
// texture.
float3 renderBehindSample(float2 uv, float level) {
    Texture2D<float4> scene = prGpuGet<Texture2D<float4>>(frame.behind);
    return scene.SampleLevel(prGpuGet<SamplerState>(frame.behindSampler), uv, level).rgb;
}

#endif

// Returns the scene's radiance behind a surface along the line from the camera through `point`, a point of the world,
// blurred for a transmitted lobe of `roughness`: the texture's level is the roughness times its number of levels, as
// KHR_materials_transmission's implementation notes give it ("the framebuffer's mip chain", log2 of the width times the
// roughness). A point outside the camera's view shows the nearest of the view's edge. The texture holds the view's
// color, which the view's exposure scales, and the radiance that this returns is the scene's own.
float3 renderBehind(float3 point, float roughness) {
    float4 clip = renderClipPoint(kPrRenderViewCamera, point);
    float2 uv = float2(clip.x, -clip.y) / max(clip.w, 1e-6) * 0.5 + 0.5;
    return renderBehindSample(saturate(uv), saturate(roughness) * frame.behindLastLevel) / frame.exposure;
}
