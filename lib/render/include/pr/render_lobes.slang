// The lobes that lib/render's shading models are built of: Fresnel terms, the GGX microfacet lobe with one width and
// with two, the sheen and fuzz lobes, the diffuse lobes, and a thin film's interference. A shading model is a rule that
// layers them. The Khronos PBR model (pr/render_brdf.slang) mixes them by Fresnel terms, as glTF's extensions specify,
// and OpenPBR Surface by albedo scaling, as its specification mandates. Both call these functions, so the two models
// share their microfacets, and an application's model may call them too.
//
// The functions read numbers and directions alone: no surface, no texture and no uniform. Each names its source: a
// section of a Khronos extension's specification with the glTF Sample Renderer's shader that implements it
// (source/Renderer/shaders), or a section of the OpenPBR Surface specification v1.1 with MaterialX's GLSL
// (libraries/pbrlib/genglsl/lib).

#pragma once

static const float kRenderPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------------------------
// Fresnel terms

// Returns Schlick's Fresnel term: what an interface reflects at the cosine `cosine`, where it reflects `f0` at normal
// incidence and `f90` at a grazing angle (glTF 2.0, appendix B.2.2, "Fresnel"; F_Schlick of brdf.glsl).
float3 renderFresnelSchlick(float3 f0, float3 f90, float cosine) {
    return f0 + (f90 - f0) * pow(1 - saturate(cosine), 5);
}

// Returns what an interface of the relative index of refraction `eta` reflects at normal incidence (KHR_materials_ior,
// "Implementation": ((ior - 1) / (ior + 1))^2).
float renderIorToF0(float eta) {
    float r = (eta - 1) / (eta + 1);
    return r * r;
}

// Returns the relative index of refraction of an interface that reflects `f0` at normal incidence, which is below 1
// (Fresnel0ToIor of iridescence.glsl).
float3 renderF0ToIor(float3 f0) {
    float3 root = sqrt(f0);
    return (1 + root) / (1 - root);
}

// Returns what a smooth interface between two dielectrics reflects of unpolarized light that meets it at the cosine
// `cosine`, where `eta` is the ratio of the index of refraction past the interface to the index before it: the Fresnel
// equations, which OpenPBR mandates for its dielectrics ("Dielectric base"), in the form of Lagarde's "Memo on Fresnel
// equations" (2013), as MaterialX's mx_fresnel_dielectric has it. Past the critical angle the interface reflects all.
float renderFresnelDielectric(float cosine, float eta) {
    float g2 = eta * eta + cosine * cosine - 1;
    if (g2 < 0)
        return 1;
    float g = sqrt(g2);
    float a = (g - cosine) / (g + cosine);
    float b = ((g + cosine) * cosine - 1) / ((g - cosine) * cosine + 1);
    return 0.5 * a * a * (1 + b * b);
}

// Returns what a dielectric interface reflects at the cosine `cosine`, by its two polarizations (p in x, s in y), as
// MaterialX's mx_fresnel_dielectric_polarized has them.
float2 renderFresnelPolarized(float cosine, float eta) {
    float c2 = saturate(cosine) * saturate(cosine);
    float s2 = 1 - c2;
    float t0 = max(eta * eta - s2, 0);
    float t1 = t0 + c2;
    float t2 = 2 * sqrt(t0) * saturate(cosine);
    float rs = (t1 - t2) / (t1 + t2);
    float t3 = c2 * t0 + s2 * s2;
    float t4 = t2 * s2;
    float rp = rs * (t3 - t4) / (t3 + t4);
    return float2(rp, rs);
}

// Returns Schlick's Fresnel term for a reflection of `f0` at normal incidence and of 1 at a grazing angle.
float3 renderFresnelSchlick(float3 f0, float cosine) {
    return f0 + (1 - f0) * pow(1 - saturate(cosine), 5);
}

// The cosine at which the F82-tint model fixes a metal's tint, 1/7, which is about 82 degrees (OpenPBR, "Metal").
static const float kRenderF82Cosine = 1.0 / 7.0;

// Returns a metal's Fresnel term at the cosine `cosine`: Schlick's term of `f0`, less a bump about 82 degrees that
// brings it to `tint` times Schlick's term there (OpenPBR, "Metal": the F82-tint model of Hoffman, "Generalization of
// Adobe's Fresnel Model", 2023). A white tint is Schlick's term alone. The result is never brighter than Schlick's.
float3 renderFresnelF82(float3 f0, float3 tint, float cosine) {
    float x = saturate(cosine);
    float at = kRenderF82Cosine;
    float3 schlickAt = renderFresnelSchlick(f0, at);
    float bump = x * pow(1 - x, 6) / (at * pow(1 - at, 6));
    return max(renderFresnelSchlick(f0, x) - bump * schlickAt * (1 - tint), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Thin films
//
// Both specifications give a thin film's reflection by Belcour and Barla, "A Practical Extension to Microfacet Theory
// for the Modeling of Varying Iridescence" (2017): the Airy sum of the film's internal reflections, integrated over the
// visible spectrum in Fourier space with Gaussian fits of the color matching functions. They differ in the Fresnel term
// of each interface and in the color space of the result, so there is one function for each.

// Returns the CIE XYZ color that an optical path difference of `opd` meters gives a phase shift of `shift`
// (evalSensitivity of iridescence.glsl, which is mx_eval_sensitivity of MaterialX: the fits of the paper's section 4).
float3 renderThinFilmSensitivity(float opd, float3 shift) {
    float phase = 2 * kRenderPi * opd;
    float3 value = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 position = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 variance = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = value * sqrt(2 * kRenderPi * variance) * cos(position * phase + shift) *
        exp(-variance * phase * phase);
    xyz.x += 9.7470e-14 * sqrt(2 * kRenderPi * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) *
        exp(-4.5282e+09 * phase * phase);
    return xyz / 1.0685e-7;
}

// Returns the phase shifts on reflection off a dielectric of the relative index `eta`, from a film of index `filmIor`:
// pi where the index falls at the interface, and 0 where it rises.
float3 renderThinFilmPhase(float3 eta, float filmIor) {
    return float3(eta.x < filmIor ? kRenderPi : 0.0, eta.y < filmIor ? kRenderPi : 0.0, eta.z < filmIor ? kRenderPi : 0.0);
}

// Returns what a thin film over a base reflects at the cosine `cosine`, in linear sRGB, as KHR_materials_iridescence
// specifies ("Implementation", "Iridescence Fresnel"; evalIridescence of iridescence.glsl). The film has the index
// `filmIor` and is `thickness` nanometers thick, under a medium of index `outsideIor`, and the base reflects `baseF0`
// at normal incidence. Each interface's Fresnel term is Schlick's. A film of no thickness returns the base's own term,
// to the fit's accuracy, since the film's index then moves to the outside's.
float3 renderThinFilmSchlick(float outsideIor, float filmIor, float cosine, float thickness, float3 baseF0) {
    // The film's index moves to the outside's as its thickness goes to zero.
    float ior = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));
    float ratio = outsideIor / ior;
    float sin2 = ratio * ratio * (1 - cosine * cosine);
    float cos2 = 1 - sin2;
    if (cos2 < 0)
        return float3(1, 1, 1);
    float cosFilm = sqrt(cos2);

    // The first interface.
    float r0 = renderIorToF0(ior / outsideIor);
    float r12 = r0 + (1 - r0) * pow(1 - saturate(cosine), 5);
    float t121 = 1 - r12;
    float phi12 = ior < outsideIor ? kRenderPi : 0.0;
    float phi21 = kRenderPi - phi12;

    // The second interface.
    float3 baseIor = renderF0ToIor(clamp(baseF0, 0, 0.9999));
    float3 r1 = (baseIor - ior) / (baseIor + ior);
    float3 r23 = renderFresnelSchlick(r1 * r1, cosFilm);
    float3 phi23 = renderThinFilmPhase(baseIor, ior);

    float opd = 2 * ior * thickness * cosFilm * 1e-9;
    float3 phi = phi21 + phi23;
    float3 r123Squared = clamp(r12 * r23, 1e-5, 0.9999);
    float3 r123 = sqrt(r123Squared);
    float3 rs = t121 * t121 * r23 / (1 - r123Squared);

    // The term without interference, and then the pairs of the sum's terms that interfere.
    float3 sum = r12 + rs;
    float3 cm = rs - t121;
    for (uint m = 1; m <= 2; ++m) {
        cm *= r123;
        float3 xyz = 2 * renderThinFilmSensitivity(float(m) * opd, float(m) * phi);
        // CIE XYZ to linear sRGB (XYZ_TO_REC709 of iridescence.glsl).
        sum += cm * float3(dot(float3(3.2404542, -1.5371385, -0.4985314), xyz),
            dot(float3(-0.9692660, 1.8760108, 0.0415560), xyz), dot(float3(0.0556434, -0.2040259, 1.0572252), xyz));
    }
    return max(sum, 0);
}

// The number of the film's internal reflections that renderThinFilmAiry adds, as MaterialX's hardware shaders do
// (GenOptions::hwAiryFresnelIterations).
static const uint kRenderThinFilmAiryIterations = 2;

// Returns what a thin film of index `filmIor` and `thickness` micrometers reflects at the cosine `cosine` over a
// substrate, as linear RGB, as OpenPBR specifies ("Thin-film iridescence": the film is folded into the Fresnel term of
// the lobe under it) and MaterialX's mx_fresnel_airy computes it, with the Fresnel equations of each polarization. The
// substrate is a dielectric of the relative index `eta` where `metal` is false, and else a metal whose Fresnel term is
// the F82-tint model of `f0` and `tint`, whose phase is that of a dielectric of the index that gives `f0`. The film
// adds no energy: it moves what the substrate reflects among the wavelengths.
float3 renderThinFilmAiry(float cosine, float filmIor, float thickness, float eta, bool metal, float3 f0, float3 tint) {
    float eta2 = max(filmIor, 1);
    float c = saturate(cosine);
    float cosFilm2 = 1 - (1 - c * c) / (eta2 * eta2);

    // The first interface, from the outside into the film, and the second, from the film onto the substrate.
    float2 r12 = cosFilm2 <= 0 ? float2(1, 1) : renderFresnelPolarized(c, eta2);
    float cosFilm = sqrt(max(cosFilm2, 0));
    float2 t121 = 1 - r12;
    float3 eta3;
    float3 r23p;
    float3 r23s;
    if (metal) {
        float3 root = sqrt(clamp(f0, 0.01, 0.99));
        eta3 = (1 + root) / (1 - root);
        r23p = 0.5 * renderFresnelF82(f0, tint, cosFilm);
        r23s = r23p;
    } else {
        eta3 = float3(eta, eta, eta);
        float2 r = renderFresnelPolarized(cosFilm, eta / eta2);
        r23p = float3(r.x, r.x, r.x);
        r23s = float3(r.y, r.y, r.y);
    }

    // The phase shifts at each interface: at the first, p flips past Brewster's angle.
    float cosBrewster = cos(atan(eta2));
    float2 phi21 = float2(c < cosBrewster ? 0.0 : kRenderPi, kRenderPi);
    float3 phi23 = renderThinFilmPhase(eta3, eta2);
    float3 r123p = sqrt(max(r12.x * r23p, 0));
    float3 r123s = sqrt(max(r12.y * r23s, 0));
    float opd = 2 * eta2 * cosFilm * thickness * 1e-6;

    float3 sum = float3(0, 0, 0);
    // Each polarization: the reflection of the sum's first term, and the terms that interfere.
    float3 rp = t121.x * t121.x * r23p / (1 - r12.x * r23p);
    sum += r12.x + rp;
    float3 cm = rp - t121.x;
    for (uint m = 1; m <= kRenderThinFilmAiryIterations; ++m) {
        cm *= r123p;
        sum += cm * 2 * renderThinFilmSensitivity(float(m) * opd, float(m) * (phi23 + phi21.x));
    }
    float3 rs = t121.y * t121.y * r23s / (1 - r12.y * r23s);
    sum += r12.y + rs;
    cm = rs - t121.y;
    for (uint m = 1; m <= kRenderThinFilmAiryIterations; ++m) {
        cm *= r123s;
        sum += cm * 2 * renderThinFilmSensitivity(float(m) * opd, float(m) * (phi23 + phi21.y));
    }
    float3 xyz = sum * 0.5;
    // CIE XYZ to RGB under illuminant E, as MaterialX's mx_fresnel_airy converts it.
    float3 rgb = float3(dot(float3(2.3706743, -0.9000405, -0.4706338), xyz),
        dot(float3(-0.5138850, 1.4253036, 0.0885814), xyz), dot(float3(0.0052982, -0.0146949, 1.0093968), xyz));
    return saturate(rgb);
}

// ---------------------------------------------------------------------------------------------------------------------
// GGX

// Returns the width of the GGX lobe of a roughness, which is the roughness squared (glTF 2.0, appendix B.3.2). A lobe
// is never narrower than a light without size can be shaded with.
float renderBrdfAlpha(float roughness) {
    return max(roughness * roughness, 0.002);
}

// What a GGX lobe holds of its roughness and the eye alone, which every light of a pixel shares.
struct RenderBrdfLobe {
    float alpha2;       // the square of the lobe's width
    float nv;           // the cosine of the normal with the eye
    float eyeTerm;      // the eye's part of the Smith visibility
};

// Returns the terms of the GGX lobe of the width `alpha` for an eye at the cosine `nv` from the normal.
RenderBrdfLobe renderBrdfLobeOfAlpha(float alpha, float nv) {
    RenderBrdfLobe lobe;
    lobe.alpha2 = alpha * alpha;
    lobe.nv = nv;
    lobe.eyeTerm = sqrt(nv * nv * (1 - lobe.alpha2) + lobe.alpha2);
    return lobe;
}

// Returns the terms of the GGX lobe of a roughness for an eye at the cosine `nv` from the normal.
RenderBrdfLobe renderBrdfLobe(float roughness, float nv) {
    return renderBrdfLobeOfAlpha(renderBrdfAlpha(roughness), nv);
}

// Returns a GGX lobe without its Fresnel term: the distribution of microfacets times the height-correlated Smith
// visibility, for the cosines of the normal with the half vector (`nh`) and the light (`nl`) (glTF 2.0, appendix B.3.2;
// D_GGX and V_GGX of brdf.glsl). A lobe of any model that reflects from GGX microfacets is this times its Fresnel term.
float renderBrdfGgx(RenderBrdfLobe lobe, float nh, float nl) {
    float alpha2 = lobe.alpha2;
    float d = nh * nh * (alpha2 - 1) + 1;
    float distribution = alpha2 / (kRenderPi * d * d);
    float visibility = 0.5 / (nl * lobe.eyeTerm + lobe.nv * sqrt(nl * nl * (1 - alpha2) + alpha2));
    return distribution * visibility;
}

// Returns the GGX lobe of a roughness without its Fresnel term, for the cosines of the normal with the half vector
// (`nh`), the light (`nl`) and the eye (`nv`).
float renderBrdfGgx(float roughness, float nh, float nl, float nv) {
    return renderBrdfGgx(renderBrdfLobe(roughness, nv), nh, nl);
}

// A GGX lobe with two widths: one along a direction of the surface and one across it. It holds what the lobe has of the
// surface's frame and the eye alone.
struct RenderBrdfAnisotropicLobe {
    float3 tangent;     // the unit direction along which the width is `alphaT`, perpendicular to the normal
    float3 bitangent;
    float alphaT;       // the lobe's width along the tangent
    float alphaB;       // and along the bitangent
    float nv;
    float eyeTerm;      // the eye's part of the Smith visibility
};

// Returns the terms of the GGX lobe of the widths `alphaT` along `tangent` and `alphaB` along `bitangent`, which with
// `normal` are an orthonormal frame, for the unit direction `toEye`.
RenderBrdfAnisotropicLobe renderBrdfAnisotropicLobe(float3 normal, float3 tangent, float3 bitangent, float alphaT,
    float alphaB, float3 toEye) {
    RenderBrdfAnisotropicLobe lobe;
    lobe.tangent = tangent;
    lobe.bitangent = bitangent;
    lobe.alphaT = alphaT;
    lobe.alphaB = alphaB;
    lobe.nv = max(dot(normal, toEye), 1e-4);
    lobe.eyeTerm = length(float3(alphaT * dot(tangent, toEye), alphaB * dot(bitangent, toEye), lobe.nv));
    return lobe;
}

// Returns an anisotropic GGX lobe without its Fresnel term, for a unit half vector and a unit direction to the light:
// the distribution of Burley's "Physically Based Shading at Disney" (2012, addenda) times the height-correlated Smith
// visibility of Heitz (JCGT 2014), which KHR_materials_anisotropy specifies ("Implementation"; D_GGX_anisotropic and
// V_GGX_anisotropic of brdf.glsl). With equal widths it is renderBrdfGgx.
float renderBrdfGgxAnisotropic(RenderBrdfAnisotropicLobe lobe, float3 normal, float3 h, float3 toLight) {
    float at = lobe.alphaT;
    float ab = lobe.alphaB;
    float a2 = at * ab;
    float3 f = float3(ab * dot(lobe.tangent, h), at * dot(lobe.bitangent, h), a2 * dot(normal, h));
    float w2 = a2 / dot(f, f);
    float distribution = a2 * w2 * w2 / kRenderPi;
    float nl = saturate(dot(normal, toLight));
    float lightTerm = length(float3(at * dot(lobe.tangent, toLight), ab * dot(lobe.bitangent, toLight), nl));
    float visibility = 0.5 / max(nl * lobe.eyeTerm + lobe.nv * lightTerm, 1e-7);
    return distribution * visibility;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sheen and fuzz
//
// The two specifications name different lobes for the light that fibers scatter at a surface's edge. glTF specifies the
// Charlie distribution with its visibility (KHR_materials_sheen), and OpenPBR the volumetric lobe of Zeltner, Burley
// and Chiang as a linearly transformed cosine ("Fuzz"). They share the rule that layers them: the lobe, over what is
// under it times one minus the lobe's directional albedo.

// Returns the Charlie distribution of the width `alpha` at the cosine `nh` of the normal with the half vector (Estevez
// and Kulla, "Production Friendly Microfacet Sheen BRDF", 2017; KHR_materials_sheen, "Sheen distribution"; D_Charlie of
// brdf.glsl).
float renderSheenCharlie(float alpha, float nh) {
    float inverse = 1 / alpha;
    // The square of the sine is never below zero, which rounding could make it for a half vector along the normal.
    float sin2 = max(1 - nh * nh, 0);
    return (2 + inverse) * pow(sin2, inverse * 0.5) / (2 * kRenderPi);
}

float renderSheenLambdaFit(float x, float alpha) {
    float t = (1 - alpha) * (1 - alpha);
    float a = lerp(21.5473, 25.3245, t);
    float b = lerp(3.82987, 3.32435, t);
    float c = lerp(0.19823, 0.16801, t);
    float d = lerp(-1.97760, -1.27393, t);
    float e = lerp(-4.32054, -4.85967, t);
    return a / (1 + b * pow(x, c)) + d * x + e;
}

// Returns the sheen's shadowing term of one direction at the cosine `cosine` from the normal (Estevez and Kulla's fit,
// KHR_materials_sheen, "Sheen visibility"; lambdaSheen of brdf.glsl).
float renderSheenLambda(float cosine, float alpha) {
    if (abs(cosine) < 0.5)
        return exp(renderSheenLambdaFit(cosine, alpha));
    return exp(2 * renderSheenLambdaFit(0.5, alpha) - renderSheenLambdaFit(1 - cosine, alpha));
}

// Returns glTF's sheen lobe without its color: the Charlie distribution times its visibility, for a sheen roughness and
// the cosines of the normal with the half vector, the light and the eye (KHR_materials_sheen, "Implementation";
// BRDF_specularSheen of brdf.glsl).
float renderSheenLobe(float roughness, float nh, float nl, float nv) {
    float alpha = max(roughness, 0.000001);
    alpha *= alpha;
    float visibility =
        saturate(1 / ((1 + renderSheenLambda(nv, alpha) + renderSheenLambda(nl, alpha)) * (4 * nv * nl)));
    return renderSheenCharlie(alpha, nh) * visibility;
}

// Returns the directional albedo of OpenPBR's fuzz at the cosine `nv` of the eye, for a fuzz roughness: the part of an
// even light that the fuzz scatters toward the eye. The fuzz is Zeltner, Burley and Chiang's "Practical
// Multiple-Scattering Sheen Using Linearly Transformed Cosines" (2022), which the specification recommends ("Fuzz"), by
// the fit that MaterialX's mx_zeltner_sheen_dir_albedo takes from the paper's code.
float renderFuzzAlbedo(float roughness, float nv) {
    float x = nv;
    float y = clamp(roughness, 0.01, 1);
    float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    float m = y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) / (1.26264 + y * (-1.92021 + y));
    float d = (x - m) / s;
    return saturate(exp(-0.5 * d * d) / (s * sqrt(2 * kRenderPi)) + o);
}

// Returns the fuzz's lobe toward the light from `toLight`, as a distribution over the directions of light that holds
// the cosine of its angle and integrates to 1: the linearly transformed cosine of Zeltner et al., whose inverse
// transform is fitted in the eye's cosine and the roughness (MaterialX's mx_zeltner_sheen_brdf). The lobe's radiance is
// this times the fuzz's albedo and its color.
float renderFuzzLobe(float3 n, float roughness, float3 toEye, float3 toLight, float nv) {
    float y = clamp(roughness, 0.01, 1);
    float3 x = toEye - n * nv;
    float length2 = dot(x, x);
    x = length2 > 1e-12 ? x * rsqrt(length2) : normalize(cross(n, abs(n.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0)));
    float3 w = float3(dot(toLight, x), dot(toLight, cross(n, x)), dot(toLight, n));
    float aInv = (2.58126 * nv + 0.813703 * y) * y / (1 + 0.310327 * nv * nv + 2.60994 * nv * y);
    float bInv = sqrt(1 - nv) * (y - 1) * y * y * y / (0.0000254053 + 1.71228 * nv - 1.71506 * nv * y + 1.34174 * y * y);
    float3 wo = float3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
    float lengthSquared = dot(wo, wo);
    float scale = aInv / lengthSquared;
    return max(wo.z, 0) / kRenderPi * scale * scale;
}

// ---------------------------------------------------------------------------------------------------------------------
// Diffuse lobes
//
// Lambert's lobe is 1 / pi, which glTF specifies (appendix B.3.1) and which is OpenPBR's lobe at a diffuse roughness of
// zero.

static const float kRenderFujii1 = 0.5 - 2.0 / (3.0 * kRenderPi);
static const float kRenderFujii2 = 2.0 / 3.0 - 28.0 / (15.0 * kRenderPi);

// Returns the directional albedo of Fujii's Oren-Nayar lobe of a white surface at a cosine (OpenPBR, "Glossy-diffuse",
// the albedo E_FON of the energy-preserving Oren-Nayar model; MaterialX's mx_oren_nayar_fujii_diffuse_dir_albedo).
float renderOrenNayarAlbedo(float cosine, float roughness) {
    float a = 1 / (1 + kRenderFujii1 * roughness);
    float b = roughness * a;
    float c = clamp(cosine, 1e-4, 1);
    float s = sqrt(max(0, 1 - c * c));
    float g = s * (acos(c) - s * c) + 2 * ((s / c) * (1 - s * s * s) - s) / 3;
    return a + b * g / kRenderPi;
}

// Returns the diffuse lobe of a rough surface of the albedo `color`, times pi: the energy-preserving Oren-Nayar model
// that OpenPBR specifies ("Glossy-diffuse": Portsmouth, Kutz and Hill, "EON: A Practical Energy-Preserving Rough
// Diffuse BRDF", 2025; MaterialX's mx_oren_nayar_compensated_diffuse). It is Fujii's lobe plus the light that bounces
// between the surface's facets more than once, which Fujii's lobe loses, so a white surface returns all that it
// receives. At a roughness of zero it is `color`, which is Lambert's lobe. `lv` is the cosine between the light and the
// eye.
float3 renderOrenNayar(float3 color, float roughness, float nl, float nv, float lv) {
    float s = lv - nl * nv;
    float stinv = s > 0 ? s / max(nl, nv) : s;
    float a = 1 / (1 + kRenderFujii1 * roughness);
    float3 single = color * a * (1 + roughness * stinv);
    float average = a * (1 + kRenderFujii2 * roughness);
    float3 multiColor = color * color * average / (1 - color * max(0, 1 - average));
    float3 multiple = multiColor * max(1e-7, 1 - renderOrenNayarAlbedo(nv, roughness)) *
        max(1e-7, 1 - renderOrenNayarAlbedo(nl, roughness)) / max(1e-7, 1 - average);
    return single + multiple;
}

// Returns the directional albedo of renderOrenNayar's lobe at a cosine: what a surface of the albedo `color` returns of
// an even light (MaterialX's mx_oren_nayar_compensated_diffuse_dir_albedo).
float3 renderOrenNayarAlbedo(float3 color, float roughness, float cosine) {
    float a = 1 / (1 + kRenderFujii1 * roughness);
    float average = a * (1 + kRenderFujii2 * roughness);
    float3 multiColor = color * color * average / (1 - color * max(0, 1 - average));
    return lerp(multiColor, color, renderOrenNayarAlbedo(cosine, roughness));
}
