// What lib/render gives a shading model: the scene's lights as they arrive at a point of a surface, with the scene's
// shadow map, the direction to the eye, and the split sum of the library's specular lobe. The file also holds the sum
// over the lights that every model shares, and the shading model that the library ships, which is the Khronos PBR
// material model (pr/render_brdf.slang).
//
// A shading model is a structure that implements IRenderSurface (pr/render_surface.slang): the model's surface, with
// the method that prepares a pixel's context, and the methods that compute the radiance that leaves the surface under
// one light, under the scene's environment, and by its own emission. renderShade calls them for any model, so no
// technique names a model or reads a field of its surface but `alpha`.

#pragma once

#include "pr/render_behind.slang"
#include "pr/render_brdf.slang"
#include "pr/render_environment.slang"
#include "pr/render_scene.slang"
#include "pr/render_surface.slang"
#include "pr/render_table.slang"

// ---------------------------------------------------------------------------------------------------------------------
// The split sum

// The library's table, which every shader set has first (prRenderSplitSumTable, pr/render_shaders.h).
static const RenderTable kRenderTableSplitSum = {float2(0, 0), float2(32, 32)};

// Returns the split sum of the library's specular lobe, GGX with the height-correlated Smith visibility and Schlick's
// Fresnel term, for a roughness and the cosine `nv` of the eye's angle to the normal: the scale and the bias that
// renderBrdfReflection takes (pr/render_brdf.slang). The two are read from a table of the lobe's integral over the
// directions of light. The table's second axis is the square root of the cosine, which gives the grazing angles, where
// the split sum changes fastest, more of its rows.
float2 renderSplitSum(float roughness, float nv) {
    return renderTableSample(kRenderTableSplitSum, float2(roughness, sqrt(saturate(nv)))).xy;
}

// Returns the directional albedo of glTF's sheen lobe (renderSheenLobe, pr/render_lobes.slang) for a sheen roughness
// and the cosine `nv` of a direction's angle to the normal: the part of an even light that the lobe scatters toward
// that direction, which KHR_materials_sheen's layering takes from what is under the sheen ("Sheen layering", the
// function E). It is the third number of the library's table, beside the split sum, by the same axes.
float renderSheenAlbedo(float roughness, float nv) {
    return renderTableSample(kRenderTableSplitSum, float2(roughness, sqrt(saturate(nv)))).z;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shadows

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(Texture2D<float>, renderSlotShadowMap, 0);

// Returns the depth that the scene's shadow map holds at a texel.
float renderShadowDepth(int2 texel) {
    return renderSlotShadowMap.Load(int3(texel, 0));
}

#else

float renderShadowDepth(int2 texel) {
    return prGpuGet<Texture2D<float>>(frame.shadowMap).Load(int3(texel, 0));
}

#endif

// Returns the depth that the shadow map holds at the texel nearest to a place in it, where a texel's center is at whole
// numbers. A texel outside the map holds nothing, which is the far plane's depth.
float renderShadowStored(float2 at) {
    int2 texel = int2(floor(at + 0.5));
    int size = int(frame.shadowSize);
    if (any(texel < 0) || any(texel >= size))
        return 1;
    return renderShadowDepth(texel);
}

// The numbers of taps with which a point's shadow is found: those that look for what stands between the point and the
// light, and those that weigh how much of the light's source it hides.
static const uint kRenderShadowSearchTaps = 16;
static const uint kRenderShadowFilterTaps = 32;

// The widest that a shadow's edge is filtered for the size of the light's source, as a radius in texels. The taps are
// too few to cover a wider disc evenly.
static const float kRenderShadowMaxRadius = 32;

// The turn between one tap and the next, as its cosine and sine: the golden angle, which spreads any number of taps
// evenly about a disc's middle (Vogel's spiral).
static const float2 kRenderShadowTurn = float2(-0.7373688780783197, 0.6754902942615238);

// Returns the place of tap `index` of `count` on the unit disc, and turns `direction`, the unit direction of the tap,
// to the next tap's. The taps cover equal areas of the disc.
float2 renderShadowTap(uint index, uint count, inout float2 direction) {
    float2 tap = direction * sqrt((float(index) + 0.5) / float(count));
    direction = float2(direction.x * kRenderShadowTurn.x - direction.y * kRenderShadowTurn.y,
        direction.x * kRenderShadowTurn.y + direction.y * kRenderShadowTurn.x);
    return tap;
}

// Returns the part of the shadowing light that reaches a point of a surface, from 0 to 1: the part of the light's
// source that the point sees past what the shadow map holds, as a filter of the map estimates it (Fernando's
// "Percentage-Closer Soft Shadows", 2005).
//
// The source is a disc of the light's angular size. From the point, what stands at a distance d toward the light hides
// the source across a disc of the map of radius d times the tangent of that size. So the taps first look for what the
// map holds nearer to the light than the point, across the disc of the nearest that the map can hold. With nothing
// there, the point is lit. Otherwise the mean depth of what was found gives the disc that the second taps cover, and
// the part of them that the map does not hide is the result. A shadow's edge is therefore sharp where the shadow
// touches what casts it, and widens with the distance from it, as the sun's does.
//
// The disc is never smaller than a texel of the map, nor than a pixel of the camera's target at the point, so that an
// edge is never sharper than the picture can show.
//
// Each tap compares the map with the depth of the surface's own plane at the tap, by the surface's normal, since the
// plane of a surface that slants from the light is nearer to the light on one side of the point. The point is first
// moved off its surface by the width of a texel, and each depth by another, so that a surface does not shadow itself. A
// depth past the map's far plane, such as that of ground beyond what casts shadows, is compared as a depth on that
// plane, so that only what the map holds shadows it.
float renderShadowLight(float3 position, float3 normal) {
    float texel = frame.shadowTexel;
    float4 clip = renderClipPoint(kPrRenderViewLight, position + normal * texel);
    float2 at = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5) * frame.shadowSize - 0.5;
    float depth = clip.z;
    float bias = texel / frame.shadowDepth;

    // The change of the plane's depth from one texel to the next. The rows of the light's matrix are its axes over the
    // half width and the depth of its view. A plane that the light grazes, or does not face, slants as one that turns a
    // fifth of the way toward the light.
    uint row = kPrRenderViewLight * 4;
    float half = texel * frame.shadowSize * 0.5;
    float2 across = float2(dot(frame.viewProjection[row].xyz, normal), dot(frame.viewProjection[row + 1].xyz, normal));
    float along = min(dot(frame.viewProjection[row + 2].xyz, normal) * frame.shadowDepth, -0.2);
    float2 slope = float2(-across.x, across.y) * (half * texel / (along * frame.shadowDepth));

    // A texel's width is the least radius, and a pixel's at the point if it is wider.
    float distance = length(frame.cameraPosition.xyz - position);
    float least = max(1.0, distance * frame.pixelSpread / texel);
    float spread = frame.shadowSpread * frame.shadowDepth / texel;
    float searchRadius = max(min(max(depth, 0) * spread, kRenderShadowMaxRadius), least);

    float blockers = 0;
    float blockerDepth = 0;
    float2 direction = float2(1, 0);
    for (uint i = 0; i < kRenderShadowSearchTaps; ++i) {
        float2 tap = renderShadowTap(i, kRenderShadowSearchTaps, direction) * searchRadius;
        float stored = renderShadowStored(at + tap);
        if (stored < min(depth + dot(slope, tap), 1.0) - bias) {
            blockers += 1;
            blockerDepth += stored;
        }
    }
    if (blockers == 0)
        return 1;
    float radius = max(min((depth - blockerDepth / blockers) * spread, searchRadius), least);
    // Where everything about the point stands in the light's way, so it does within the second disc, which is no wider.
    if (blockers == float(kRenderShadowSearchTaps))
        return 0;

    float lit = 0;
    direction = float2(1, 0);
    for (uint i = 0; i < kRenderShadowFilterTaps; ++i) {
        float2 tap = renderShadowTap(i, kRenderShadowFilterTaps, direction) * radius;
        if (renderShadowStored(at + tap) >= min(depth + dot(slope, tap), 1.0) - bias)
            lit += 1;
    }
    return lit / float(kRenderShadowFilterTaps);
}

// ---------------------------------------------------------------------------------------------------------------------
// Lights

// Returns the irradiance that a light gives a plane that faces it at a point, and writes the unit direction from the
// point toward the light. A point or spot light falls off with the square of the distance, reaches zero at its range,
// and a spot light falls off from its inner cone to its outer one, as glTF's KHR_lights_punctual defines them.
float3 renderLightIrradiance(PrRenderLightRecord light, float3 position, out float3 toLight) {
    uint kind = uint(light.direction.w);
    if (kind == kPrRenderLightDirectional) {
        toLight = -light.direction.xyz;
        return light.color.rgb;
    }
    float3 offset = light.position.xyz - position;
    float distance2 = max(dot(offset, offset), 1e-8);
    float distance = sqrt(distance2);
    toLight = offset / distance;
    float falloff = 1 / distance2;
    if (light.position.w > 0) {
        float ratio = distance / light.position.w;
        float ratio2 = ratio * ratio;
        falloff *= saturate(1 - ratio2 * ratio2);
    }
    if (kind == kPrRenderLightSpot) {
        float cone = saturate(dot(light.direction.xyz, -toLight) * light.cone.x + light.cone.y);
        falloff *= cone * cone;
    }
    return light.color.rgb * falloff;
}

// Returns the unit direction from a point of a surface toward the camera.
float3 renderShadingToEye(RenderSurfaceInputs inputs) {
    return normalize(frame.cameraPosition.xyz - inputs.position);
}

// Returns the number of the scene's lights that shade a surface.
uint renderShadingLightCount() {
    return frame.lightCount;
}

// Returns the angle from the middle of a light's source to its edge, as a point sees it, in radians: the light's own
// for a directional light, and for a point or spot light the angle that the radius of its source spans at the point.
float renderLightAngularRadius(PrRenderLightRecord light, float3 position) {
    float size = light.cone.z;
    if (uint(light.direction.w) == kPrRenderLightDirectional || size <= 0)
        return size;
    return asin(saturate(size / max(length(light.position.xyz - position), 1e-4)));
}

// Returns the scene's light `index` as it arrives at a point of a surface. The point's own normal, not its material's,
// places it in the light's shadow, on the side of the surface that faces the light: a surface that a light reaches from
// behind, as one that transmits light passes it, does not shadow the light that passes through it.
RenderShadingLight renderShadingLight(uint index, RenderSurfaceInputs inputs) {
    RenderShadingLight light;
    light.irradiance = renderLightIrradiance(frame.lights[index], inputs.position, light.toLight);
    light.angularRadius = renderLightAngularRadius(frame.lights[index], inputs.position);
    light.visibility = 1;
    if (index == frame.shadowLight) {
        float3 facing = dot(inputs.normal, light.toLight) < 0 ? -inputs.normal : inputs.normal;
        light.visibility = renderShadowLight(inputs.position, facing);
    }
    return light;
}

// Returns the radiance that leaves a point of a surface toward the camera by the surface's shading model: the
// environment's light, each light's, and what the surface emits times `emission`, the factor that takes the emission to
// luminance (renderMaterialEmission). The model prepares its context once, and every term reads it.
float3 renderShade<S : IRenderSurface>(S surface, RenderSurfaceInputs inputs, float emission) {
    float3 toEye = renderShadingToEye(inputs);
    S.Context context = surface.prepare(toEye);
    float3 color = surface.indirectRadiance(context, toEye);
    for (uint i = 0; i < renderShadingLightCount(); ++i)
        color += surface.directRadiance(context, toEye, renderShadingLight(i, inputs));
    return color + surface.emittedRadiance(context, toEye) * emission;
}

// Returns the radiance that leaves a point of a surface toward the camera, as scene-referred linear sRGB, and in alpha
// the surface's alpha. It is inlined so that the compiled code has no function for it beside renderShade's.
[ForceInline]
float4 renderShadeAlpha<S : IRenderSurface>(S surface, RenderSurfaceInputs inputs, float emission) {
    return float4(renderShade(surface, inputs, emission), surface.alpha);
}

// ---------------------------------------------------------------------------------------------------------------------
// The environment, as the layers of the Khronos PBR model gather it

// Returns the environment's radiance as glTF's sheen lobe of a roughness gathers it, for a surface with the unit normal
// `normal` seen from `toEye`.
//
// The reference filters a second cube for the Charlie distribution (getIBLRadianceCharlie of ibl.glsl). The scene has
// one cube, filtered for GGX, and a second would take a texture slot that the smallest device does not have. The sheen
// lobe is wide at every roughness: its distribution is a power of the sine of the half vector's angle, so it gathers
// light from most of the hemisphere, with more from the side opposite the eye for a low roughness. So the sheen reads
// the cube's level of a roughness of 1 about the mirror direction, mixed with the environment's diffuse light about the
// normal by the sheen's roughness. It is exact in an even environment, and it has no highlight of a small source.
float3 renderEnvironmentSheen(float3 normal, float3 toEye, float roughness) {
    float3 about = renderEnvironmentRadiance(reflect(-toEye, normal), 1);
    return lerp(about, renderEnvironmentIrradiance(normal), saturate(roughness) * 0.5);
}

// Returns the light that arrives at a point behind a surface along the unit direction `direction`, as a transmitted
// lobe of `roughness` gathers it: the scene behind the surface, where the pass has it, and else the environment.
float3 renderPbrBehind(float3 point, float3 direction, float roughness) {
    if (renderBehindAvailable())
        return renderBehind(point, roughness);
    return renderEnvironmentRadiance(direction, roughness);
}

// Returns the light that a surface at `position` transmits toward the eye from behind it, with the volume's attenuation
// along the path of the eye's ray through the surface's thickness (KHR_materials_transmission and KHR_materials_volume;
// getIBLVolumeRefraction and getVolumeTransmissionRay of the reference).
//
// The eye's ray bends into the surface by its index of refraction and leaves it after the surface's thickness. The
// light is what the scene behind the surface shows at the point where the ray leaves, which is the reference's rule: it
// does not bend the ray again where it leaves, and it reads the scene as the camera sees it, so what is not on screen
// is not refracted. A surface without thickness is a thin wall, which does not bend the ray at all. Dispersion refracts
// each channel by an index of its own (KHR_materials_dispersion, "Implementation": the indices spread about the
// surface's by (ior - 1) * 0.025 * dispersion).
float3 renderPbrTransmitted(RenderPbrSurface surface, float3 position, float3 toEye) {
    float ior = max(surface.ior, 1e-3);
    float roughness = renderBrdfTransmissionRoughness(surface.roughness, ior);
    float3 light;
    float path = surface.thickness;
    if (surface.dispersion > 0) {
        float spread = (ior - 1) * 0.025 * surface.dispersion;
        float3 iors = float3(ior - spread, ior, ior + spread);
        for (uint i = 0; i < 3; ++i) {
            float3 direction = refract(-toEye, surface.normal, 1 / iors[i]);
            if (dot(direction, direction) <= 0)
                direction = -toEye;
            light[i] = renderPbrBehind(position + direction * surface.thickness, direction, roughness)[i];
        }
    } else {
        float3 direction = refract(-toEye, surface.normal, 1 / ior);
        if (dot(direction, direction) <= 0)
            direction = -toEye;
        light = renderPbrBehind(position + direction * surface.thickness, direction, roughness);
    }
    return light * renderBrdfAttenuation(path, surface.attenuationColor, surface.attenuationDistance);
}

// ---------------------------------------------------------------------------------------------------------------------
// The Khronos PBR model

// The shading model of a RenderPbrSurface, which the library's material types compute, and any type of an application
// that names it as its Surface.
//
// The model shades a light as a source without size: it does not read RenderShadingLight::angularRadius, and a lobe is
// never narrower than renderBrdfAlpha allows.
extension RenderPbrSurface : IRenderSurface {
    typedef RenderPbrContext Context;

    // What every light of the pixel shares: the specular lobe's terms for the eye, the surface's reflection, and what
    // each layer reflects toward the eye.
    Context prepare(float3 toEye) {
        float sheen = 0;
        if (any(sheenColor > 0))
            sheen = renderSheenAlbedo(sheenRoughness, dot(normal, toEye));
        return renderBrdfPrepare(this, toEye, sheen);
    }

    // The model's lobes under one light.
    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) {
        float sheen = 0;
        if (context.layered && context.sheenScale > 0)
            sheen = renderSheenAlbedo(sheenRoughness, dot(normal, light.toLight));
        return renderBrdfDirect(this, context, toEye, light.toLight, sheen) * (light.irradiance * light.visibility);
    }

    // The environment's diffuse light, and its radiance about the direction that the surface mirrors toward the eye,
    // filtered for the surface's roughness, both dimmed by the surface's occlusion. Each layer gathers the environment
    // for itself.
    float3 indirectRadiance(Context context, float3 toEye) {
        float2 split = renderSplitSum(roughness, dot(normal, toEye));
        if (!context.layered) {
            float3 mirrored = reflect(-toEye, normal);
            float3 radiance = renderEnvironmentRadiance(mirrored, roughness);
            return renderBrdfIndirect(this, toEye, radiance, renderEnvironmentIrradiance(normal), split) * occlusion;
        }
        RenderPbrEnvironment environment;
        environment.radiance = renderEnvironmentRadiance(renderBrdfMirrored(this, toEye), roughness);
        environment.irradiance = renderEnvironmentIrradiance(normal);
        environment.behind = float3(0, 0, 0);
        environment.coat = float3(0, 0, 0);
        environment.sheen = float3(0, 0, 0);
        environment.transmitted = float3(0, 0, 0);
        if (diffuseTransmission > 0)
            environment.behind = renderEnvironmentIrradiance(-normal);
        if (clearcoat > 0)
            environment.coat = renderEnvironmentRadiance(reflect(-toEye, clearcoatNormal), clearcoatRoughness);
        if (context.sheenScale > 0)
            environment.sheen = renderEnvironmentSheen(normal, toEye, sheenRoughness);
        if (transmission > 0)
            environment.transmitted = renderPbrTransmitted(this, this.position, toEye);
        return renderBrdfIndirectLayered(this, context, environment, split) * occlusion;
    }

    override float3 emittedRadiance(Context context, float3 toEye) {
        return renderBrdfEmitted(this, context);
    }
}
