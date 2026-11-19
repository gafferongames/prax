// OpenPBR Surface as a shading model of lib/render: a surface of its own that lights itself, as IRenderSurface asks. It
// is the library's second physically based model, beside the Khronos PBR model (pr/render_brdf.slang), and a shader set
// has it only if it lists the material type of pr/render_material_openpbr.slang, as it lists any type. The model
// prepares a context for each pixel, with what its layers reflect toward the eye, which every light and the environment
// share.
//
// The model follows the OpenPBR Surface specification v1.1 (Academy Software Foundation), whose sections the comments
// name, and its MaterialX reference implementation (libraries/bxdf/open_pbr_surface.mtlx and the GLSL of
// libraries/pbrlib), where the specification leaves a choice to the implementation. Its structure is the
// specification's ("Formalism", "Model"):
//
//     surface = layer(coated base, fuzz, fuzz_weight)
//     coated base = layer(base substrate, coat, coat_weight)
//     base substrate = mix(dielectric base, metal, base_metalness)
//     dielectric base = layer(mix(opaque base, translucent base, transmission_weight), specular)
//     opaque base = mix(diffuse, subsurface, subsurface_weight)
//
// Each layer is combined by albedo scaling ("Formalism", "Layering"): the layer's lobe, over what is under it times one
// minus the layer's directional albedo toward the eye. So a layer that reflects all it does not pass on, over a base
// that returns all it receives, returns all it receives (the white furnace). The lobes are the library's
// (pr/render_lobes.slang), which the Khronos model mixes by Fresnel terms instead.
//
// The model reads every input of the specification. What a rasterizer cannot compute, it degrades, as the specification
// asks of an implementation ("Introduction": a renderer that lacks a feature ignores it or approximates it):
//
// - The translucent base shows the scene behind the surface, bent by the surface's index of refraction and blurred by
//   its roughness, each channel by an index of its own with dispersion. The model has no input for the length of the
//   path through the object, which a path tracer finds, so the material gives one: the surface's `thickness`. The
//   medium absorbs along that length by Beer's law, from transmission_color and transmission_depth. The medium's
//   scattering (transmission_scatter and its anisotropy) is not drawn. A light behind the surface shows as the scene
//   behind shows it, and has no highlight of its own on the surface.
// - Subsurface scattering is drawn as a diffuse lobe of subsurface_color: the color that the specification defines as
//   the multiple-scattering albedo. Light does not bleed along the surface, so subsurface_radius, its scale and its
//   anisotropy change nothing. In thin-walled mode the lobe is half reflected and half transmitted through the wall, as
//   the specification gives it ("Thin-walled mode").
// - In thin-walled mode the translucent base does not bend light, and transmission_color tints what passes.
// - Anisotropy stretches each lobe under a light exactly. Under the environment, the lobe gathers the cube that was
//   filtered for an isotropic lobe, about a mirror direction that is bent as the Khronos model bends it, and each
//   lobe's albedo is that of the isotropic lobe of the same mean roughness.
// - The fuzz is shaded once, about the coat's normal by the coat's weight, where the specification asks for the fuzz
//   to follow each of the two normals ("Fuzz").
//
// The GGX lobe and the split sum of the environment's reflection are the library's (pr/render_lobes.slang and
// pr/render_shading.slang), so the two models share their microfacets and their environment.

#pragma once

#include "pr/render_brdf.slang"
#include "pr/render_environment.slang"
#include "pr/render_shading.slang"

// What OpenPBR reads at a point of a surface: the parameters of the specification that the model implements, as the
// material computed them there.
struct RenderOpenPbrSurface {
    float3 baseColor;           // base_color times base_weight, linear
    float alpha;                // geometry_opacity
    float3 specularColor;       // specular_color
    float specularWeight;       // specular_weight
    float3 coatColor;           // coat_color
    float coatWeight;           // coat_weight
    float3 fuzzColor;           // fuzz_color
    float fuzzWeight;           // fuzz_weight
    float3 emission;            // emission_color times emission_luminance
    float metalness;            // base_metalness
    float3 normal;              // geometry_normal and geometry_coat_normal, in the world, of unit length
    float specularRoughness;    // specular_roughness
    float specularIor;          // specular_ior
    float coatRoughness;        // coat_roughness
    float coatIor;              // coat_ior
    float coatDarkening;        // coat_darkening
    float fuzzRoughness;        // fuzz_roughness
    float thinFilmWeight;       // thin_film_weight
    float thinFilmThickness;    // thin_film_thickness, in micrometers
    float thinFilmIor;          // thin_film_ior
    float occlusion;            // the part of the environment's light that reaches the point, from 0 to 1

    float diffuseRoughness;     // base_diffuse_roughness
    float specularAnisotropy;   // specular_roughness_anisotropy
    float3 tangent;             // geometry_tangent, in the world, of any length
    float3 coatNormal;          // geometry_coat_normal, in the world, of unit length
    float coatAnisotropy;       // coat_roughness_anisotropy
    float3 coatTangent;         // geometry_coat_tangent, in the world, of any length

    float transmissionWeight;   // transmission_weight
    float3 transmissionColor;   // transmission_color
    float transmissionDepth;    // transmission_depth, in the world's units
    // transmission_dispersion_scale over transmission_dispersion_abbe_number, or zero for no dispersion.
    float dispersion;

    float subsurfaceWeight;     // subsurface_weight
    float3 subsurfaceColor;     // subsurface_color
    bool thinWalled;            // geometry_thin_walled

    // What a rasterizer needs in place of the object's interior: the point of the surface in the world, and the length
    // of the path of a transmitted ray through the object under it, in the world.
    float3 position;
    float thickness;
};

// Returns OpenPBR's defaults with the normal `normal` (the specification's parameter tables).
RenderOpenPbrSurface renderOpenPbrSurfaceDefault(float3 normal) {
    RenderOpenPbrSurface surface;
    surface.baseColor = float3(0.8, 0.8, 0.8);
    surface.alpha = 1;
    surface.specularColor = float3(1, 1, 1);
    surface.specularWeight = 1;
    surface.coatColor = float3(1, 1, 1);
    surface.coatWeight = 0;
    surface.fuzzColor = float3(1, 1, 1);
    surface.fuzzWeight = 0;
    surface.emission = float3(0, 0, 0);
    surface.metalness = 0;
    surface.normal = normal;
    surface.specularRoughness = 0.3;
    surface.specularIor = 1.5;
    surface.coatRoughness = 0;
    surface.coatIor = 1.6;
    surface.coatDarkening = 1;
    surface.fuzzRoughness = 0.5;
    surface.thinFilmWeight = 0;
    surface.thinFilmThickness = 0.5;
    surface.thinFilmIor = 1.4;
    surface.occlusion = 1;
    surface.diffuseRoughness = 0;
    surface.specularAnisotropy = 0;
    surface.tangent = float3(0, 0, 0);
    surface.coatNormal = normal;
    surface.coatAnisotropy = 0;
    surface.coatTangent = float3(0, 0, 0);
    surface.transmissionWeight = 0;
    surface.transmissionColor = float3(1, 1, 1);
    surface.transmissionDepth = 0;
    surface.dispersion = 0;
    surface.subsurfaceWeight = 0;
    surface.subsurfaceColor = float3(0.8, 0.8, 0.8);
    surface.thinWalled = false;
    surface.position = float3(0, 0, 0);
    surface.thickness = 0;
    return surface;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lobes

// What a GGX lobe returns of an even environment seen at a cosine: its directional albedo after one bounce and after
// every bounce, and the factor between them, which the lobe's direct light is scaled by.
struct RenderOpenPbrAlbedo {
    float3 single;
    float3 total;
    float3 compensation;
};

// Returns the albedo of a GGX lobe of `roughness`, whose Fresnel term reflects `f0` at normal incidence and `mirrored`
// toward the eye's mirror direction, seen at the cosine `nv`.
//
// The library's split sum is the albedo of a lobe with Schlick's term. The lobe's own term differs from Schlick's (the
// Fresnel equations for a dielectric, F82 for a metal, and a thin film's interference), so the albedo is scaled by the
// ratio of the two terms in the mirror direction. At a roughness of 0 the result is the lobe's own term, to the table's
// accuracy, and a lobe whose term is zero, such as a dielectric of specular weight 0, reflects nothing.
//
// The light that bounces between microfacets more than once is added back as the library's Pbr model adds it
// (Fdez-Aguera, "A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting", JCGT 2019), which the
// specification leaves to the implementation ("Microfacet model", which names Kulla and Conty 2017 and Turquin 2019).
// The compensation is the same factor for direct light, as MaterialX's mx_ggx_energy_compensation scales it, so a white
// metal conserves energy under a light as it does under the environment.
RenderOpenPbrAlbedo renderOpenPbrLobeAlbedo(float3 f0, float3 mirrored, float roughness, float nv) {
    float2 split = renderSplitSum(roughness, nv);
    float3 schlick = renderFresnelSchlick(f0, nv);
    float lost = 1 - (split.x + split.y);
    float3 average = f0 + (1 - f0) / 21;
    RenderOpenPbrAlbedo result;
    result.single = (f0 * split.x + split.y) * (mirrored / max(schlick, 1e-6));
    result.compensation = 1 / (1 - average * lost);
    result.total = result.single * result.compensation;
    return result;
}

// Returns the roughness of the base under the coat: the coat widens what the base reflects ("Coat", "Roughening").
float renderOpenPbrBaseRoughness(RenderOpenPbrSurface surface) {
    float r = surface.specularRoughness;
    float coat = surface.coatRoughness;
    float coated = pow(min(1.0, r * r * r * r + 2 * coat * coat * coat * coat), 0.25);
    return lerp(r, coated, surface.coatWeight);
}

// Returns the relative index of refraction of the specular layer: of the base against the outside, or under the coat
// against the coat, mixed by the coat's weight ("Coat", equation of the specular IOR ratio). A ratio below 1 under the
// coat is inverted, as the specification's "Total internal reflection" suggests and MaterialX does. The specular weight
// then scales the layer's reflection at normal incidence by changing the ratio ("Dielectric base").
float renderOpenPbrSpecularEta(RenderOpenPbrSurface surface) {
    float underCoat = surface.specularIor / surface.coatIor;
    if (underCoat < 1)
        underCoat = 1 / underCoat;
    float eta = lerp(surface.specularIor, underCoat, surface.coatWeight);
    float f0 = renderIorToF0(eta);
    float epsilon = sign(eta - 1) * sqrt(clamp(surface.specularWeight * f0, 0, 0.99999));
    return (1 + epsilon) / (1 - epsilon);
}

// The Fresnel terms of the base's two lobes at one cosine, with the thin film mixed in by its weight.
struct RenderOpenPbrBaseFresnel {
    float3 dielectric;
    float3 metal;
};

// Returns the Fresnel terms of the base at the cosine `cosine`. The metal's includes the specular weight, which scales
// the metal's whole lobe ("Metal").
RenderOpenPbrBaseFresnel renderOpenPbrBaseFresnel(RenderOpenPbrSurface surface, float eta, float cosine) {
    RenderOpenPbrBaseFresnel fresnel;
    fresnel.dielectric = renderFresnelDielectric(cosine, eta);
    fresnel.metal = renderFresnelF82(surface.baseColor, surface.specularColor, cosine);
    if (surface.thinFilmWeight > 0) {
        float w = surface.thinFilmWeight;
        float t = surface.thinFilmThickness;
        float n = surface.thinFilmIor;
        float3 none = float3(0, 0, 0);
        fresnel.dielectric =
            lerp(fresnel.dielectric, renderThinFilmAiry(cosine, n, t, eta, false, none, none), w);
        fresnel.metal = lerp(fresnel.metal,
            renderThinFilmAiry(cosine, n, t, eta, true, surface.baseColor, surface.specularColor), w);
    }
    fresnel.metal *= surface.specularWeight;
    return fresnel;
}

// Returns what the base's two lobes reflect at normal incidence, as renderOpenPbrBaseFresnel returns their terms at a
// cosine. A pixel needs it once, for the albedo of each lobe.
RenderOpenPbrBaseFresnel renderOpenPbrBaseF0(RenderOpenPbrSurface surface, float eta) {
    float f0 = renderIorToF0(eta);
    RenderOpenPbrBaseFresnel fresnel;
    fresnel.dielectric = float3(f0, f0, f0);
    fresnel.metal = surface.baseColor;
    if (surface.thinFilmWeight > 0) {
        float w = surface.thinFilmWeight;
        float t = surface.thinFilmThickness;
        float n = surface.thinFilmIor;
        float3 none = float3(0, 0, 0);
        fresnel.dielectric = lerp(fresnel.dielectric, renderThinFilmAiry(1, n, t, eta, false, none, none), w);
        fresnel.metal = lerp(fresnel.metal,
            renderThinFilmAiry(1, n, t, eta, true, surface.baseColor, surface.specularColor), w);
    }
    fresnel.metal *= surface.specularWeight;
    return fresnel;
}

// What the coat does to the light of the base, for the eye alone: the length of the eye's path through the coat, and
// the darkening of the base by the light that the coat reflects back onto it.
struct RenderOpenPbrCoatedBase {
    float eyePath;      // half of one over the cosine of the eye's direction inside the coat
    float3 darkened;
};

// Returns the eye's part of the coat's effect on the base, for the eye at the cosine `nv` ("Coat").
//
// The coat absorbs along the paths that light takes through it, which are longer at a slant: coat_color is the square
// of its transmittance at normal incidence, and a path at the cosine c inside the coat passes through 1 / c of it
// ("View-dependent absorption"). Light that the coat reflects back onto the base, more than once, darkens and saturates
// it, by the internal reflection of a coat over a rough base and the base's albedo as MaterialX estimates them
// ("Darkening").
RenderOpenPbrCoatedBase renderOpenPbrCoatedBasePrepare(RenderOpenPbrSurface surface, float nv) {
    float eta = surface.coatIor;
    float inV = sqrt(max(1 - (1 - nv * nv) / (eta * eta), 1e-4));
    float internal = 1 - (1 - renderIorToF0(eta)) / (eta * eta);
    float3 baseAlbedo = lerp(surface.baseColor, surface.baseColor * surface.specularWeight, surface.metalness);
    float3 darkening = (1 - internal) / (1 - baseAlbedo * internal);
    RenderOpenPbrCoatedBase coated;
    coated.eyePath = 0.5 / inV;
    coated.darkened = lerp(float3(1, 1, 1), darkening, surface.coatWeight * surface.coatDarkening);
    return coated;
}

// Returns the factor by which the coat dims the light of the base, which passes through the coat toward the light at
// the cosine `nl` and toward the eye, where the coat's albedo toward the eye is `coatAlbedo` ("Coat", "Formalism": the
// coated base is coat_weight times the coat's lobe over this times the base).
float3 renderOpenPbrCoatedBase(RenderOpenPbrSurface surface, RenderOpenPbrCoatedBase coated, float nl, float coatAlbedo) {
    float eta = surface.coatIor;
    float inL = sqrt(max(1 - (1 - nl * nl) / (eta * eta), 1e-4));
    float3 transmitted = pow(max(surface.coatColor, 1e-6), 0.5 / inL + coated.eyePath);
    return lerp(float3(1, 1, 1), transmitted * (1 - coatAlbedo), surface.coatWeight) * coated.darkened;
}

// Returns the directional albedo of the fuzz toward the eye at the cosine `nv`, times its weight: the part of the light
// that the fuzz scatters and does not pass on to what is under it ("Fuzz").
float renderOpenPbrFuzzAlbedo(RenderOpenPbrSurface surface, float nv) {
    if (surface.fuzzWeight <= 0)
        return 0;
    return renderFuzzAlbedo(surface.fuzzRoughness, nv) * surface.fuzzWeight;
}

// Returns the GGX lobe of two widths of a roughness and an anisotropy, about `normal`, stretched along `tangent`
// ("Microfacet model": alpha_t = r^2 sqrt(2 / (1 + (1 - a)^2)) and alpha_b = (1 - a) alpha_t, which keep the mean of
// the squared widths whatever the anisotropy). A tangent of zero gives any direction, which a lobe without anisotropy
// does not show.
RenderBrdfAnisotropicLobe renderOpenPbrAnisotropicLobe(float3 normal, float3 tangent, float roughness, float anisotropy,
    float3 toEye) {
    float3 t = tangent - normal * dot(normal, tangent);
    float length2 = dot(t, t);
    if (length2 < 1e-12) {
        t = cross(normal, abs(normal.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0));
        length2 = dot(t, t);
    }
    t *= rsqrt(length2);
    float keeps = 1 - saturate(anisotropy);
    float alphaT = renderBrdfAlpha(roughness) * sqrt(2 / (1 + keeps * keeps));
    return renderBrdfAnisotropicLobe(normal, t, cross(normal, t), min(alphaT, 1), max(keeps * alphaT, 0.001), toEye);
}

// Returns what the medium of a surface that transmits keeps of the light that crosses the surface's thickness
// ("Translucent base": Beer's law, where white light has the color transmission_color after transmission_depth). With a
// depth of zero the medium is absent and the color tints the light, as it does through a thin wall ("Thin-walled mode":
// the color is the wall's transmittance).
float3 renderOpenPbrTransmittance(RenderOpenPbrSurface surface) {
    if (surface.thinWalled || surface.transmissionDepth <= 0)
        return surface.transmissionColor;
    return pow(max(surface.transmissionColor, 1e-6), surface.thickness / surface.transmissionDepth);
}

// Returns the light from behind a surface along the eye's ray, refracted by an index of refraction of `ior`. A thin
// wall does not refract the ray (renderOpenPbrTransmitted).
float3 renderOpenPbrBehind(RenderOpenPbrSurface surface, float3 toEye, float ior, float blur) {
    float3 direction = -toEye;
    float thickness = 0;
    if (!surface.thinWalled) {
        thickness = surface.thickness;
        direction = refract(-toEye, surface.normal, 1 / ior);
        if (dot(direction, direction) <= 0)
            direction = -toEye;
    }
    return renderBehindAvailable() ? renderBehind(surface.position + direction * thickness, blur)
                                   : renderEnvironmentRadiance(direction, blur);
}

// Returns the light that a surface transmits toward the eye from behind it, before the medium's absorption: the scene
// behind the surface where the eye's ray leaves it, after the surface's thickness along the direction that its index of
// refraction bends the ray into, or the environment along that direction in a pass without that scene. A thin wall does
// not bend the ray. Dispersion bends each channel by an index of its own: the indices spread about the surface's by
// (ior - 1) / (2 abbe), which is the spread of the Abbe number's definition between the F and C lines, as
// KHR_materials_dispersion applies it too.
float3 renderOpenPbrTransmitted(RenderOpenPbrSurface surface, float3 toEye, float roughness) {
    float ior = max(surface.specularIor, 1e-3);
    float blur = surface.thinWalled ? roughness : renderBrdfTransmissionRoughness(roughness, ior);
    float spread = surface.thinWalled ? 0 : (ior - 1) * 0.5 * surface.dispersion;
    if (spread <= 0)
        return renderOpenPbrBehind(surface, toEye, ior, blur);
    return float3(renderOpenPbrBehind(surface, toEye, ior - spread, blur).r,
        renderOpenPbrBehind(surface, toEye, ior, blur).g, renderOpenPbrBehind(surface, toEye, ior + spread, blur).b);
}

// ---------------------------------------------------------------------------------------------------------------------
// The shading model

// What OpenPBR computes of a surface and the eye alone, once for a pixel: the roughness and the index of the base under
// the coat, each GGX lobe's terms for the eye, and what each layer reflects toward the eye, which decides what it
// passes to the layer under it. Every light of the pixel, the environment and the emission read it.
struct RenderOpenPbrContext {
    float nv;                   // the cosine of the normal with the eye
    float roughness;            // of the base, under the coat
    float eta;                  // the relative index of refraction of the specular layer
    RenderBrdfLobe baseLobe;
    RenderBrdfLobe coatLobe;
    RenderOpenPbrAlbedo dielectric;   // the albedo of the base's dielectric lobe
    RenderOpenPbrAlbedo metal;        // the albedo of the base's metal lobe
    RenderOpenPbrAlbedo coat;         // the coat's albedo, in x
    float coatFresnel;                // what the coat's interface reflects at the eye's angle
    RenderOpenPbrCoatedBase coated;
    float fuzz;                                // the fuzz's albedo times its weight
    float coatNv;                              // the cosine of the coat's normal with the eye
    float3 fuzzNormal;                         // the normal that the fuzz follows
    RenderBrdfAnisotropicLobe baseAnisotropic; // the base's lobe with two widths, for a surface with anisotropy
    RenderBrdfAnisotropicLobe coatAnisotropic;
    float3 opaque;              // the albedo of the opaque base: the diffuse color and the subsurface color, mixed
};

// Returns the context of a surface for the unit direction `toEye`.
RenderOpenPbrContext renderOpenPbrPrepare(RenderOpenPbrSurface surface, float3 toEye) {
    RenderOpenPbrContext context;
    float nv = max(dot(surface.normal, toEye), 1e-4);
    context.nv = nv;
    context.roughness = renderOpenPbrBaseRoughness(surface);
    context.eta = renderOpenPbrSpecularEta(surface);
    context.baseLobe = renderBrdfLobe(context.roughness, nv);
    float coatNv = max(dot(surface.coatNormal, toEye), 1e-4);
    context.coatNv = coatNv;
    context.coatLobe = renderBrdfLobe(surface.coatRoughness, coatNv);
    context.baseAnisotropic =
        renderOpenPbrAnisotropicLobe(surface.normal, surface.tangent, context.roughness, surface.specularAnisotropy, toEye);
    context.coatAnisotropic = renderOpenPbrAnisotropicLobe(surface.coatNormal, surface.coatTangent,
        surface.coatRoughness, surface.coatAnisotropy, toEye);
    context.fuzzNormal = normalize(lerp(surface.normal, surface.coatNormal, surface.coatWeight));
    context.opaque = lerp(surface.baseColor, surface.subsurfaceColor, surface.subsurfaceWeight);
    RenderOpenPbrBaseFresnel atEye = renderOpenPbrBaseFresnel(surface, context.eta, nv);
    RenderOpenPbrBaseFresnel atNormal = renderOpenPbrBaseF0(surface, context.eta);
    context.dielectric = renderOpenPbrLobeAlbedo(atNormal.dielectric, atEye.dielectric, context.roughness, nv);
    context.metal = renderOpenPbrLobeAlbedo(atNormal.metal, atEye.metal, context.roughness, nv);
    float coatF0 = renderIorToF0(surface.coatIor);
    float coatMirrored = renderFresnelDielectric(coatNv, surface.coatIor);
    context.coat = renderOpenPbrLobeAlbedo(float3(coatF0, coatF0, coatF0),
        float3(coatMirrored, coatMirrored, coatMirrored), surface.coatRoughness, coatNv);
    context.coatFresnel = coatMirrored;
    context.coated = renderOpenPbrCoatedBasePrepare(surface, coatNv);
    context.fuzz = renderOpenPbrFuzzAlbedo(surface, max(dot(context.fuzzNormal, toEye), 1e-4));
    return context;
}

// Returns the radiance that leaves a surface toward `toEye` under one light, for a light of unit irradiance on a plane
// that faces it, including the cosine of the light's angle to the normal. `context` is the surface's for `toEye`.
float3 renderOpenPbrBrdf(RenderOpenPbrSurface surface, RenderOpenPbrContext context, float3 toEye, float3 toLight) {
    float3 n = surface.normal;
    float nl = saturate(dot(n, toLight));
    if (nl <= 0) {
        // A light behind a thin wall: the half of the subsurface's light that the wall transmits ("Thin-walled mode"),
        // under the layers above it as the eye sees them.
        float through = surface.thinWalled ? surface.subsurfaceWeight * (1 - surface.transmissionWeight) : 0;
        if (through <= 0)
            return float3(0, 0, 0);
        float3 lobe = surface.subsurfaceColor * (0.5 * through * saturate(-dot(n, toLight)) / kRenderPi);
        float3 under = lobe * (1 - context.dielectric.total) * (1 - surface.metalness);
        return (1 - context.fuzz) * renderOpenPbrCoatedBase(surface, context.coated, context.coatNv,
            context.coat.total.x) * under;
    }
    float nv = context.nv;
    float3 h = normalize(toEye + toLight);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(toEye, h));

    // The base: the specular layer over the diffuse lobe, and the metal, mixed by the metalness.
    float ggx = renderBrdfGgx(context.baseLobe, nh, nl) * nl;
    if (surface.specularAnisotropy > 0)
        ggx = renderBrdfGgxAnisotropic(context.baseAnisotropic, n, h, toLight) * nl;
    RenderOpenPbrBaseFresnel atHalf = renderOpenPbrBaseFresnel(surface, context.eta, vh);
    float3 specular = surface.specularColor * atHalf.dielectric * ggx * context.dielectric.compensation;
    // The diffuse lobe is Lambert's, of base_color times base_weight, under what the specular layer does not reflect
    // toward the eye ("Glossy-diffuse": the base color is defined so that it is the diffuse albedo under the gloss).
    float3 diffuse = surface.baseColor * (1 - context.dielectric.total) * (nl / kRenderPi);
    if (surface.diffuseRoughness > 0 || surface.subsurfaceWeight > 0 || surface.transmissionWeight > 0) {
        // The opaque base: the rough diffuse lobe and the subsurface's diffuse lobe, by the subsurface's weight. A thin
        // wall reflects half of the subsurface's light. The translucent base takes its weight of both.
        float3 rough = renderOrenNayar(surface.baseColor, surface.diffuseRoughness, nl, nv, dot(toEye, toLight));
        float3 under = surface.subsurfaceColor * (surface.thinWalled ? 0.5 : 1.0);
        float3 opaque = lerp(rough, under, surface.subsurfaceWeight) * (1 - surface.transmissionWeight);
        diffuse = opaque * (1 - context.dielectric.total) * (nl / kRenderPi);
    }
    float3 base = lerp(specular + diffuse, atHalf.metal * ggx * context.metal.compensation, surface.metalness);

    // The coat over the base, about the coat's own normal.
    float3 cn = surface.coatNormal;
    float cnl = saturate(dot(cn, toLight));
    float coatGgx = renderBrdfGgx(context.coatLobe, saturate(dot(cn, h)), cnl);
    if (surface.coatAnisotropy > 0)
        coatGgx = renderBrdfGgxAnisotropic(context.coatAnisotropic, cn, h, toLight);
    float coatLobe = 0;
    if (cnl > 0)
        coatLobe = renderFresnelDielectric(vh, surface.coatIor) * coatGgx * cnl * context.coat.compensation.x;
    float3 coated = surface.coatWeight * coatLobe +
        renderOpenPbrCoatedBase(surface, context.coated, nl, context.coat.total.x) * base;

    // The fuzz over everything ("Fuzz": its albedo scaling leaves the color of what is under it).
    float fuzz = context.fuzz;
    float3 fuzzed = float3(0, 0, 0);
    if (fuzz > 0)
        fuzzed = surface.fuzzColor * fuzz * renderFuzzLobe(context.fuzzNormal, surface.fuzzRoughness, toEye, toLight,
            max(dot(context.fuzzNormal, toEye), 1e-4));
    return fuzzed + (1 - fuzz) * coated;
}

// Returns the radiance that leaves a surface toward `toEye` under one light, as renderOpenPbrBrdf does with a context,
// for a caller that shades one light.
float3 renderOpenPbrBrdf(RenderOpenPbrSurface surface, float3 toEye, float3 toLight) {
    return renderOpenPbrBrdf(surface, renderOpenPbrPrepare(surface, toEye), toEye, toLight);
}

// OpenPBR is a shading model: the surface lights itself, with the library's interface.
extension RenderOpenPbrSurface : IRenderSurface {
    typedef RenderOpenPbrContext Context;

    // What the layers reflect toward the eye, which no light changes.
    Context prepare(float3 toEye) {
        return renderOpenPbrPrepare(this, toEye);
    }

    // One light's part: the BRDF under the light, scaled by the light's irradiance and visibility.
    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) {
        return renderOpenPbrBrdf(this, context, toEye, light.toLight) * (light.irradiance * light.visibility);
    }

    // Returns the radiance that leaves the surface toward `toEye` under the scene's environment, dimmed by the
    // surface's occlusion. Each lobe's single bounce reflects the environment about the mirror direction, filtered for
    // the lobe's roughness, and the light of its further bounces and of the diffuse lobe is the environment's diffuse
    // light, as the library's Pbr model divides them.
    float3 indirectRadiance(Context context, float3 toEye) {
        float3 n = this.normal;
        float3 mirrored = reflect(-toEye, n);
        float3 irradiance = renderEnvironmentIrradiance(n);

        float3 radiance = renderEnvironmentRadiance(mirrored, context.roughness);
        if (this.specularAnisotropy > 0) {
            float3 bent = renderBrdfBentNormal(n, context.baseAnisotropic.bitangent, toEye, this.specularAnisotropy,
                context.roughness);
            radiance = renderEnvironmentRadiance(reflect(-toEye, bent), context.roughness);
        }
        RenderOpenPbrAlbedo dielectric = context.dielectric;
        RenderOpenPbrAlbedo metal = context.metal;
        float3 specular = this.specularColor * (dielectric.single * radiance + (dielectric.total - dielectric.single) *
            irradiance);
        float3 diffuse = this.baseColor * (1 - dielectric.total) * irradiance;
        if (this.diffuseRoughness > 0 || this.subsurfaceWeight > 0 || this.transmissionWeight > 0) {
            // Under the specular layer: the opaque base's two diffuse lobes, and in the translucent base's part the
            // light from behind the surface. A thin wall passes half of the subsurface's light from its far side.
            float3 rough = renderOrenNayarAlbedo(this.baseColor, this.diffuseRoughness, context.nv) * irradiance;
            float3 under = this.subsurfaceColor * irradiance;
            if (this.thinWalled)
                under = this.subsurfaceColor * 0.5 * (irradiance + renderEnvironmentIrradiance(-n));
            float3 opaque = lerp(rough, under, this.subsurfaceWeight);
            if (this.transmissionWeight > 0) {
                float3 transmitted = renderOpenPbrTransmitted(this, toEye, context.roughness) *
                    renderOpenPbrTransmittance(this);
                opaque = lerp(opaque, transmitted, this.transmissionWeight);
            }
            diffuse = opaque * (1 - dielectric.total);
        }
        float3 base = lerp(specular + diffuse, metal.single * radiance + (metal.total - metal.single) * irradiance,
            this.metalness);

        RenderOpenPbrAlbedo coat = context.coat;
        float3 coatMirror = this.coatNormal;
        if (this.coatAnisotropy > 0) {
            coatMirror = renderBrdfBentNormal(this.coatNormal, context.coatAnisotropic.bitangent, toEye,
                this.coatAnisotropy, this.coatRoughness);
        }
        float3 coatLight =
            coat.single.x * renderEnvironmentRadiance(reflect(-toEye, coatMirror), this.coatRoughness) +
            (coat.total.x - coat.single.x) * irradiance;
        float3 coated = this.coatWeight * coatLight +
            renderOpenPbrCoatedBase(this, context.coated, context.coatNv, coat.total.x) * base;

        float fuzz = context.fuzz;
        return (this.fuzzColor * fuzz * irradiance + (1 - fuzz) * coated) * this.occlusion;
    }

    // Returns the radiance that the surface emits toward `toEye`. The emission lies under the coat, which tints it by
    // its absorption and passes what its interface does not reflect at the eye's angle ("Emission"), so a coated
    // emitter darkens toward its silhouette.
    override float3 emittedRadiance(Context context, float3 toEye) {
        float3 coated = this.coatColor * (1 - context.coatFresnel);
        return this.emission * lerp(float3(1, 1, 1), coated, this.coatWeight);
    }
}
