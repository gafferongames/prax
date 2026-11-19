// The surface and the lobes of the Khronos PBR material model, which is the material model of glTF 2.0 with Khronos's
// ratified material extensions: metallic and roughness, with a Lambertian diffuse lobe under a GGX specular lobe, and
// the layers that the extensions add. The shading model that lib/render ships, Pbr, is this model
// (pr/render_shading.slang). The functions read only a RenderPbrSurface and what their caller gives them of the
// environment, so any shader may call them.
//
// glTF defines a material as a mix of two: a dielectric, whose specular lobe reflects 4% at normal incidence over a
// diffuse lobe of the base color, and a metal, which has the specular lobe alone, colored by the base color. The
// metallic value mixes them. Schlick's Fresnel term is linear in what a surface reflects at normal incidence, so the
// specular lobes of the two mix into one lobe of the mixed reflection. The diffuse lobe is the dielectric's alone, and
// gets what the dielectric's specular lobe leaves, whatever the metallic value.
//
// Each extension is a layer of this one model, by the rule that its specification gives, in the order of the glTF
// Sample Renderer's pbr.frag (the comments name each extension's section):
//
//     material = mix(coated, clearcoat lobe, clearcoat * its Fresnel term)          KHR_materials_clearcoat
//     coated   = sheen lobe + base * (1 - the sheen's albedo)                       KHR_materials_sheen
//     base     = mix(dielectric, metal, metallic)
//     dielectric = mix(diffuse, specular lobe, Fresnel) + what passes through       KHR_materials_ior, _specular
//     diffuse  = mix(mix(Lambert, diffuse transmission), specular transmission)     KHR_materials_diffuse_transmission,
//                                                                                   _transmission, _volume, _dispersion
//
// with a thin film in the Fresnel terms of the dielectric and the metal (KHR_materials_iridescence), and two widths of
// the specular lobe (KHR_materials_anisotropy). A surface without layers is shaded by the core model's arithmetic
// alone: the layers are behind one test of the context, which a material type that sets none of them folds away.

#pragma once

#include "pr/render_lobes.slang"

// What the Khronos PBR model reads at a point of a surface: the inputs of glTF's core material and of its extensions.
struct RenderPbrSurface {
    float3 baseColor;   // linear sRGB
    float alpha;        // the part of the point that the surface covers
    float metallic;
    float roughness;

    // The strength of a dielectric's reflection, from 0 for none to 1 for glTF's default (KHR_materials_specular's
    // specular factor). A surface with no base color, no specular and some emission radiates the emission alone.
    float specular;

    // What the surface's dielectric reflects at normal incidence, before `specular` scales it: 4% for glTF's index of
    // refraction of 1.5, times the specular color (KHR_materials_ior and KHR_materials_specular). renderPbrDielectricF0
    // computes it.
    float3 dielectricF0;

    float3 normal;      // in the world, of unit length
    float occlusion;    // the part of the indirect light that reaches the point, from 0 to 1
    float3 emissive;    // radiance that the point adds, linear sRGB

    // KHR_materials_clearcoat: the coat's strength, its roughness and its own normal.
    float clearcoat;
    float clearcoatRoughness;
    float3 clearcoatNormal;

    // KHR_materials_sheen. A black sheen is none.
    float3 sheenColor;
    float sheenRoughness;

    // KHR_materials_iridescence: the film's strength, its index of refraction and its thickness in nanometers.
    float iridescence;
    float iridescenceIor;
    float iridescenceThickness;

    // KHR_materials_anisotropy: the strength, from 0 to 1, and the direction along which the lobe is wider, in the
    // world, which need not be of unit length nor perpendicular to the normal.
    float anisotropy;
    float3 anisotropyDirection;

    // KHR_materials_diffuse_transmission: the part of the diffuse light that passes through, and its color.
    float diffuseTransmission;
    float3 diffuseTransmissionColor;

    // KHR_materials_transmission, KHR_materials_volume and KHR_materials_dispersion: the part of the light that the
    // dielectric does not reflect which passes through it, the index of refraction that bends it, the thickness of the
    // volume under the point in the world, the color that white light has after `attenuationDistance` of the volume (a
    // distance of zero or less is no attenuation), and the dispersion (20 over the Abbe number).
    float transmission;
    float ior;
    float thickness;
    float3 attenuationColor;
    float attenuationDistance;
    float dispersion;

    // The point of the surface in the world, which a surface that transmits reads to find what lies behind it. A
    // material that sets `transmission` sets it.
    float3 position;
};

// Returns a surface that reflects and emits nothing: black, opaque and rough, with the normal `normal`, and without
// layers.
RenderPbrSurface renderPbrSurfaceDefault(float3 normal) {
    RenderPbrSurface surface;
    surface.baseColor = float3(0, 0, 0);
    surface.alpha = 1;
    surface.metallic = 0;
    surface.roughness = 1;
    surface.specular = 0;
    surface.dielectricF0 = float3(0.04, 0.04, 0.04);
    surface.normal = normal;
    surface.occlusion = 1;
    surface.emissive = float3(0, 0, 0);
    surface.clearcoat = 0;
    surface.clearcoatRoughness = 0;
    surface.clearcoatNormal = normal;
    surface.sheenColor = float3(0, 0, 0);
    surface.sheenRoughness = 0;
    surface.iridescence = 0;
    surface.iridescenceIor = 1.3;
    surface.iridescenceThickness = 400;
    surface.anisotropy = 0;
    surface.anisotropyDirection = float3(0, 0, 0);
    surface.diffuseTransmission = 0;
    surface.diffuseTransmissionColor = float3(1, 1, 1);
    surface.transmission = 0;
    surface.ior = 1.5;
    surface.thickness = 0;
    surface.attenuationColor = float3(1, 1, 1);
    surface.attenuationDistance = 0;
    surface.dispersion = 0;
    surface.position = float3(0, 0, 0);
    return surface;
}

// Returns what a dielectric of the index of refraction `ior` reflects at normal incidence, tinted by `specularColor`,
// for RenderPbrSurface::dielectricF0: ((ior - 1) / (ior + 1))^2 (KHR_materials_ior, "Implementation") times the color,
// and no more than 1 (KHR_materials_specular, "Implementation": dielectric_f0 = min(f0 * specular_color, 1)). An index
// of zero stands for an infinite one, which reflects all (KHR_materials_ior, "ior").
float3 renderPbrDielectricF0(float ior, float3 specularColor) {
    float f0 = ior > 0 ? renderIorToF0(ior) : 1;
    return min(f0 * specularColor, 1);
}

// Returns the part of the light that a surface's dielectric reflects at normal incidence.
float3 renderBrdfDielectricF0(RenderPbrSurface surface) {
    return surface.dielectricF0 * surface.specular;
}

// Returns the color of a surface's reflection at normal incidence.
float3 renderBrdfF0(RenderPbrSurface surface) {
    return lerp(renderBrdfDielectricF0(surface), surface.baseColor, surface.metallic);
}

// Returns the part of a surface's reflection at a grazing angle that it keeps. A dielectric reflects fully there only
// as far as its specular strength allows.
float renderBrdfF90(RenderPbrSurface surface) {
    return lerp(surface.specular, 1, surface.metallic);
}

// What the coat reflects at normal incidence: a coat of the index 1.5 (KHR_materials_clearcoat, "Clearcoat BRDF").
static const float kRenderClearcoatF0 = 0.04;

// What the model computes of a surface and the eye alone, once for a pixel: the context of its shading model.
struct RenderPbrContext {
    RenderBrdfLobe lobe;    // the specular lobe's terms
    float3 f0;              // the color of the surface's reflection at normal incidence
    float f90;              // the part of its reflection at a grazing angle that the surface keeps
    float3 dielectricF0;    // what the surface's dielectric reflects at normal incidence

    // True if the surface has a layer of an extension. The rest of the context is for such a surface, and a surface
    // without layers is shaded without reading it.
    bool layered;
    float nv;                               // the cosine of the normal with the eye, at least a little above zero
    RenderBrdfAnisotropicLobe anisotropic;  // the specular lobe with two widths
    RenderBrdfLobe clearcoatLobe;
    float clearcoatFresnel;                 // the coat's strength times what it reflects at the eye's angle
    float sheenAlbedo;                      // the sheen's directional albedo toward the eye
    float sheenScale;                       // the largest channel of the sheen's color
    float3 iridescenceDielectric;           // what the film reflects over the dielectric at the eye's angle
    float3 iridescenceMetal;                // and over the metal
    float3 passes;                          // what the dielectric's interface passes at the eye's angle
};

// Returns true if a surface has a layer of an extension.
bool renderBrdfLayered(RenderPbrSurface surface) {
    return surface.clearcoat > 0 || any(surface.sheenColor > 0) || surface.iridescence > 0 || surface.anisotropy > 0 ||
        surface.diffuseTransmission > 0 || surface.transmission > 0;
}

// Returns the unit direction along which a surface's specular lobe is wider, perpendicular to its normal: the surface's
// anisotropy direction, or any direction for a surface without one.
float3 renderBrdfAnisotropyTangent(RenderPbrSurface surface) {
    float3 n = surface.normal;
    float3 t = surface.anisotropyDirection - n * dot(n, surface.anisotropyDirection);
    float length2 = dot(t, t);
    if (length2 < 1e-12) {
        t = cross(n, abs(n.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0));
        length2 = dot(t, t);
    }
    return t * rsqrt(length2);
}

// Returns the context of a surface for the unit direction `toEye`. `sheenAlbedo` is the directional albedo of the sheen
// lobe of the surface's sheen roughness toward the eye, which a renderer's shaders read from the library's table
// (renderSheenAlbedo, pr/render_shading.slang), and which a surface without sheen does not read.
RenderPbrContext renderBrdfPrepare(RenderPbrSurface surface, float3 toEye, float sheenAlbedo) {
    RenderPbrContext context;
    float nv = max(dot(surface.normal, toEye), 1e-4);
    context.lobe = renderBrdfLobe(surface.roughness, nv);
    context.f0 = renderBrdfF0(surface);
    context.f90 = renderBrdfF90(surface);
    context.dielectricF0 = renderBrdfDielectricF0(surface);
    context.layered = renderBrdfLayered(surface);
    context.nv = nv;
    if (!context.layered)
        return context;

    // KHR_materials_anisotropy, "Implementation": the width along the direction rises with the strength squared, and
    // the width across it is the roughness's.
    float alpha = surface.roughness * surface.roughness;
    float3 tangent = renderBrdfAnisotropyTangent(surface);
    context.anisotropic = renderBrdfAnisotropicLobe(surface.normal, tangent, cross(surface.normal, tangent),
        lerp(alpha, 1, surface.anisotropy * surface.anisotropy), clamp(alpha, 0.001, 1), toEye);

    // KHR_materials_clearcoat, "Layering": the coat's Fresnel term at the eye's angle to the coat's normal weighs the
    // coat against what is under it.
    float coatNv = max(dot(surface.clearcoatNormal, toEye), 1e-4);
    context.clearcoatLobe = renderBrdfLobe(surface.clearcoatRoughness, coatNv);
    context.clearcoatFresnel =
        surface.clearcoat * (kRenderClearcoatF0 + (1 - kRenderClearcoatF0) * pow(1 - saturate(coatNv), 5));

    context.sheenAlbedo = sheenAlbedo;
    context.sheenScale = max(surface.sheenColor.r, max(surface.sheenColor.g, surface.sheenColor.b));

    // KHR_materials_iridescence, "Implementation": the film's Fresnel term is evaluated once, at the eye's angle, over
    // each of the two base materials.
    context.iridescenceDielectric = float3(0, 0, 0);
    context.iridescenceMetal = float3(0, 0, 0);
    if (surface.iridescence > 0) {
        context.iridescenceDielectric = renderThinFilmSchlick(1, surface.iridescenceIor, nv,
            surface.iridescenceThickness, context.dielectricF0);
        context.iridescenceMetal =
            renderThinFilmSchlick(1, surface.iridescenceIor, nv, surface.iridescenceThickness, surface.baseColor);
    }

    // What the dielectric's interface does not reflect toward the eye passes through it (pbr.frag's `transmittance`).
    context.passes = 1 - (context.dielectricF0 + (surface.specular - context.dielectricF0) * pow(1 - nv, 5));
    return context;
}

// Returns what light of a volume keeps after a path of `distance` through it: Beer's law, where white light has the
// color `attenuationColor` after `attenuationDistance` (KHR_materials_volume, "Attenuation"; applyVolumeAttenuation of
// punctual.glsl). A distance of zero or less is no attenuation.
float3 renderBrdfAttenuation(float distance, float3 attenuationColor, float attenuationDistance) {
    if (attenuationDistance <= 0)
        return float3(1, 1, 1);
    return pow(max(attenuationColor, 1e-6), distance / attenuationDistance);
}

// Returns the roughness of the lobe of the light that a surface transmits: the surface's roughness, less for an index
// of refraction near 1, where the interface bends light little (KHR_materials_transmission, "Implementation notes";
// applyIorToRoughness of punctual.glsl).
float renderBrdfTransmissionRoughness(float roughness, float ior) {
    return roughness * saturate(ior * 2 - 2);
}

// Returns the radiance that leaves a layered surface toward `toEye` for light from `toLight`, as renderBrdfDirect does
// for any surface. The light may come from behind the surface, which a surface that transmits passes on.
// `sheenAlbedoLight` is the directional albedo of the surface's sheen lobe toward the light, as the context holds the
// one toward the eye.
float3 renderBrdfDirectLayered(RenderPbrSurface surface, RenderPbrContext context, float3 toEye, float3 toLight,
    float sheenAlbedoLight) {
    float3 n = surface.normal;
    float nlSigned = dot(n, toLight);
    float nl = saturate(nlSigned);
    float3 h = normalize(toEye + toLight);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(toEye, h));
    float schlick = pow(1 - vh, 5);

    // The Fresnel terms of the dielectric and of the metal, each with the film's in the film's part.
    float3 dielectric = context.dielectricF0 + (surface.specular - context.dielectricF0) * schlick;
    float3 metal = surface.baseColor + (1 - surface.baseColor) * schlick;
    if (surface.iridescence > 0) {
        dielectric = lerp(dielectric, context.iridescenceDielectric, surface.iridescence);
        metal = lerp(metal, context.iridescenceMetal, surface.iridescence);
    }

    float3 color = float3(0, 0, 0);
    if (nl > 0) {
        float ggx = surface.anisotropy > 0 ? renderBrdfGgxAnisotropic(context.anisotropic, n, h, toLight)
                                           : renderBrdfGgx(context.lobe, nh, nl);
        // What neither kind of transmission takes is reflected diffusely.
        float3 diffuse = surface.baseColor *
            ((1 - surface.diffuseTransmission) * (1 - surface.transmission) * nl / kRenderPi);
        float3 dielectricLobe = diffuse * (1 - dielectric) + dielectric * (ggx * nl);
        color = lerp(dielectricLobe, metal * (ggx * nl), surface.metallic);
    } else {
        // Light from behind. KHR_materials_diffuse_transmission, "Implementation": a Lambertian lobe about the reversed
        // normal, of the transmission's color, which the volume attenuates along its thickness.
        float3 through = float3(0, 0, 0);
        if (surface.diffuseTransmission > 0) {
            through += surface.diffuseTransmissionColor * (surface.diffuseTransmission * (1 - surface.transmission) *
                -nlSigned / kRenderPi) *
                renderBrdfAttenuation(surface.thickness, surface.attenuationColor, surface.attenuationDistance);
        }
        // KHR_materials_transmission, "Implementation notes": the microfacet lobe of the light that the surface
        // refracts, as the reflection lobe of the light's mirror image in the surface, which is exact for an interface
        // that does not bend light (getPunctualRadianceTransmission of punctual.glsl). The volume moves the light by
        // the refracted ray's path and attenuates it along that path.
        if (surface.transmission > 0) {
            float3 mirrored = toLight - 2 * nlSigned * n;
            float3 hm = normalize(mirrored + toEye);
            float roughness = renderBrdfTransmissionRoughness(surface.roughness, surface.ior);
            float ggx = renderBrdfGgx(renderBrdfLobe(roughness, context.nv), saturate(dot(n, hm)), -nlSigned);
            through += surface.baseColor * (surface.transmission * ggx * -nlSigned) *
                renderBrdfAttenuation(surface.thickness, surface.attenuationColor, surface.attenuationDistance);
        }
        color = through * context.passes * (1 - surface.metallic);
    }

    // KHR_materials_sheen, "Sheen layering": the sheen's lobe, over the base scaled by what the sheen lets through
    // toward the eye or toward the light, whichever is less, which keeps the layering reciprocal.
    if (context.sheenScale > 0) {
        float3 sheen = float3(0, 0, 0);
        if (nl > 0)
            sheen = surface.sheenColor * (renderSheenLobe(surface.sheenRoughness, nh, nl, context.nv) * nl);
        color = sheen + color * (1 - context.sheenScale * max(context.sheenAlbedo, sheenAlbedoLight));
    }

    // KHR_materials_clearcoat, "Layering": the coat's lobe, about the coat's own normal, mixed over the rest by the
    // coat's strength times its Fresnel term at the eye's angle.
    if (surface.clearcoat > 0) {
        float3 cn = surface.clearcoatNormal;
        float cnl = saturate(dot(cn, toLight));
        float3 coat = float3(0, 0, 0);
        if (cnl > 0) {
            float fresnel = kRenderClearcoatF0 + (1 - kRenderClearcoatF0) * schlick;
            coat = fresnel * renderBrdfGgx(context.clearcoatLobe, saturate(dot(cn, h)), cnl) * cnl;
        }
        color = lerp(color, coat, context.clearcoatFresnel);
    }
    return color;
}

// Returns the radiance that leaves a surface toward `toEye`, for light that arrives from `toLight` with unit irradiance
// on a plane that faces it. Both directions are of unit length, and `context` is the surface's for `toEye`. The result
// includes the cosine of the light's angle to the normal. `sheenAlbedoLight` is the directional albedo of the surface's
// sheen lobe toward the light, which only a surface with sheen reads.
float3 renderBrdfDirect(RenderPbrSurface surface, RenderPbrContext context, float3 toEye, float3 toLight,
    float sheenAlbedoLight) {
    if (context.layered)
        return renderBrdfDirectLayered(surface, context, toEye, toLight, sheenAlbedoLight);
    float3 n = surface.normal;
    float nl = saturate(dot(n, toLight));
    if (nl <= 0)
        return float3(0, 0, 0);
    float3 h = normalize(toEye + toLight);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(toEye, h));
    float ggx = renderBrdfGgx(context.lobe, nh, nl);
    float schlick = pow(1 - vh, 5);
    float3 fresnel = context.f0 + (context.f90 - context.f0) * schlick;
    float3 dielectric = context.dielectricF0 + (surface.specular - context.dielectricF0) * schlick;

    float3 diffuse = (1 - dielectric) * (1 - surface.metallic) * surface.baseColor / kRenderPi;
    return (diffuse + fresnel * ggx) * nl;
}

// Returns the radiance that leaves a surface toward `toEye` under one light, as renderBrdfDirect does, for a caller
// without the sheen's albedo toward the light: it scales what is under the sheen by the albedo toward the eye alone,
// which conserves energy as well.
float3 renderBrdfDirect(RenderPbrSurface surface, RenderPbrContext context, float3 toEye, float3 toLight) {
    return renderBrdfDirect(surface, context, toEye, toLight, context.sheenAlbedo);
}

// What a specular lobe reflects of an even environment: the part that leaves after one bounce off a microfacet, and the
// part that leaves after more.
struct RenderBrdfReflection {
    float3 single;
    float3 multiple;
};

// Returns what a specular lobe reflects of an even environment, for a lobe that reflects `f0` at normal incidence and
// `f90` at a grazing angle, seen at the cosine `nv` from the normal.
//
// `split` is the lobe's split sum: the scale and the bias of the split-sum approximation of the lobe's reflection of an
// even environment. The part of the environment's radiance that the lobe reflects is f0 times the scale plus f90 times
// the bias, and their sum is the lobe's directional albedo: what it reflects if it reflects all at every angle. A
// renderer's shaders read the two from the library's table (renderSplitSum, pr/render_shading.slang).
//
// The lobe of renderBrdfDirect loses the light that bounces between microfacets more than once, which darkens a rough
// metal. Fdez-Aguera's "A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting" (JCGT 2019) adds it
// back: the light that the first bounce does not return bounces again, each time colored by the lobe's mean reflection.
RenderBrdfReflection renderBrdfReflection(float3 f0, float f90, float roughness, float nv, float2 split) {
    // A rough surface's Fresnel term rises less toward a grazing angle.
    float3 rise = max(f90 * (1 - roughness), f0) - f0;
    float lost = 1 - (split.x + split.y);
    float3 average = f0 + (f90 - f0) / 21;
    RenderBrdfReflection reflection;
    reflection.single = (f0 + rise * pow(1 - nv, 5)) * split.x + f90 * split.y;
    reflection.multiple = reflection.single * average / (1 - average * lost) * lost;
    return reflection;
}

// The environment's light that a layered surface gathers, which a renderer's shaders read for it (RenderPbrSurface's
// indirectRadiance, pr/render_shading.slang). Each is radiance, and a layer that the surface lacks may be zero.
struct RenderPbrEnvironment {
    float3 radiance;        // about the direction that the specular lobe mirrors toward the eye, for its roughness
    float3 irradiance;      // what a white surface without gloss would leave, with the surface's normal
    float3 behind;          // the same with the reversed normal: the diffuse light on the surface's far side
    float3 coat;            // about the direction that the coat mirrors, for the coat's roughness
    float3 sheen;           // as the sheen's lobe gathers it
    float3 transmitted;     // the scene behind the surface, as the surface refracts it, with the volume's attenuation
};

// Returns the normal about which an anisotropic lobe gathers an environment that was filtered for an isotropic one, for
// a surface with the unit normal `n` whose lobe is narrower along the unit direction `across`: the normal, bent toward
// the plane that holds the eye and `across`, less for a rougher surface (getIBLRadianceAnisotropy of ibl.glsl).
float3 renderBrdfBentNormal(float3 n, float3 across, float3 toEye, float anisotropy, float roughness) {
    float3 tangent = cross(across, toEye);
    float3 bent = cross(tangent, across);
    float bend = 1 - anisotropy * (1 - roughness);
    bend *= bend;
    return normalize(lerp(bent, n, bend * bend));
}

// Returns the direction about which a surface's specular lobe gathers the environment for the eye: the eye's mirror
// image in the normal, which anisotropy bends toward the plane across the lobe's wider direction
// (KHR_materials_anisotropy, "Image-based lighting"; getIBLRadianceAnisotropy of ibl.glsl, after Revie's "Implementing
// Fur Using Deferred Shading", GPU Pro 2, and McAuley's "Rendering the World of Far Cry 4", 2015).
float3 renderBrdfMirrored(RenderPbrSurface surface, float3 toEye) {
    float3 n = surface.normal;
    if (surface.anisotropy > 0) {
        // The reference's anisotropicB, the direction of the lobe's width of the roughness.
        float3 across = cross(n, renderBrdfAnisotropyTangent(surface));
        n = renderBrdfBentNormal(n, across, toEye, surface.anisotropy, surface.roughness);
    }
    return reflect(-toEye, n);
}

// Returns the radiance that leaves a layered surface toward `toEye` under an environment, as renderBrdfIndirect does
// for a surface without layers, before the surface's occlusion.
float3 renderBrdfIndirectLayered(RenderPbrSurface surface, RenderPbrContext context, RenderPbrEnvironment environment,
    float2 split) {
    float nv = context.nv;
    RenderBrdfReflection dielectric =
        renderBrdfReflection(context.dielectricF0, surface.specular, surface.roughness, nv, split);
    RenderBrdfReflection metal = renderBrdfReflection(surface.baseColor, 1, surface.roughness, nv, split);

    // What the dielectric does not reflect: the diffuse lobe's light, and in the parts that transmit, the light from
    // behind the surface (pbr.frag: f_diffuse mixed with the diffuse transmission, then with the transmission).
    float3 under = surface.baseColor * environment.irradiance;
    if (surface.diffuseTransmission > 0) {
        float3 behind = surface.diffuseTransmissionColor * environment.behind *
            renderBrdfAttenuation(surface.thickness, surface.attenuationColor, surface.attenuationDistance);
        under = lerp(under, behind, surface.diffuseTransmission);
    }
    if (surface.transmission > 0)
        under = lerp(under, surface.baseColor * environment.transmitted, surface.transmission);

    float3 dielectricLight = under * (1 - dielectric.single - dielectric.multiple) +
        dielectric.single * environment.radiance + dielectric.multiple * environment.irradiance;
    float3 metalLight = metal.single * environment.radiance + metal.multiple * environment.irradiance;
    if (surface.iridescence > 0) {
        // The film's Fresnel term in place of the lobe's, with the lobe's albedo at full reflection.
        float albedo = split.x + split.y;
        float3 filmed = context.iridescenceDielectric * albedo;
        dielectricLight = lerp(dielectricLight, under * (1 - filmed) + filmed * environment.radiance,
            surface.iridescence);
        metalLight = lerp(metalLight, context.iridescenceMetal * albedo * environment.radiance, surface.iridescence);
    }
    float3 color = lerp(dielectricLight, metalLight, surface.metallic);

    if (context.sheenScale > 0) {
        color = surface.sheenColor * context.sheenAlbedo * environment.sheen +
            color * (1 - context.sheenScale * context.sheenAlbedo);
    }
    if (surface.clearcoat > 0)
        color = lerp(color, environment.coat, context.clearcoatFresnel);
    return color;
}

// Returns the radiance that leaves a surface without layers toward `toEye` under an environment, by the split-sum
// approximation. `radiance` is the environment's radiance about the mirror direction, as the specular lobe of the
// surface's roughness gathers it, and `irradiance` is the radiance that a white surface without gloss would leave.
// `split` is the split sum of the surface's lobe at the eye's angle.
//
// The light that bounces between microfacets more than once leaves in every direction, as diffuse light does. The
// dielectric's diffuse lobe gets what its specular lobe returns by neither path. So a white surface in an even
// environment returns all that it receives, at any roughness and any metallic value.
float3 renderBrdfIndirect(RenderPbrSurface surface, float3 toEye, float3 radiance, float3 irradiance, float2 split) {
    float nv = saturate(dot(surface.normal, toEye));
    float3 f0 = renderBrdfDielectricF0(surface);
    RenderBrdfReflection dielectric = renderBrdfReflection(f0, surface.specular, surface.roughness, nv, split);
    RenderBrdfReflection metal = renderBrdfReflection(surface.baseColor, 1, surface.roughness, nv, split);
    float3 diffuse = surface.baseColor * (1 - dielectric.single - dielectric.multiple);
    float3 single = lerp(dielectric.single, metal.single, surface.metallic);
    float3 scattered = lerp(dielectric.multiple + diffuse, metal.multiple, surface.metallic);
    return single * radiance + scattered * irradiance;
}

// Returns the radiance that a surface emits toward the eye: its emission, under the coat, which passes what it does not
// reflect at the eye's angle (pbr.frag: f_emissive * (1 - clearcoatFactor * clearcoatFresnel)).
float3 renderBrdfEmitted(RenderPbrSurface surface, RenderPbrContext context) {
    if (context.layered && surface.clearcoat > 0)
        return surface.emissive * (1 - context.clearcoatFresnel);
    return surface.emissive;
}
