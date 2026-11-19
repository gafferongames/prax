// The material type of a glTF material with Khronos's material extensions: the core material's factors and textures,
// and those of KHR_materials_ior, _specular, _clearcoat, _sheen, _iridescence, _anisotropy, _transmission, _volume,
// _dispersion and _diffuse_transmission. It computes the surface of the Khronos PBR model, as the core material's type
// does (pr/render_material_khronos_pbr.slang), with the model's layers.
//
// The type also draws the archived KHR_materials_pbrSpecularGlossiness, by its own definition and not by a conversion
// to metallic and roughness: a diffuse color that the specular color's largest channel dims, under a specular lobe that
// reflects the specular color at normal incidence, with a roughness of one minus the glossiness. That is this model's
// dielectric with a colored reflection, so a material of that workflow is a surface of the same model, texel by texel,
// where a conversion of its textures to metallic and roughness is exact only where the specular color is gray.
//
// A material without extensions draws as the core type, which samples five textures in the slots of their indices and
// whose shader holds no layer. A loader chooses the core type for a material that sets none of what only this type has.
//
// The type samples the textures of a layer only where the material's factor turns the layer on, since each of the
// extensions multiplies its texture by a factor that is zero by default.

#pragma once

#include "pr/render_brdf.slang"
#include "pr/render_material.slang"
#include "pr/render_material_khronos_pbr.slang"
#include "pr/render_shading.slang"

struct RenderMaterialKhronosPbrLayered : IRenderMaterial {
    typedef RenderPbrSurface Surface;

    // The core material's properties, as the core type names them, and then each extension's, by its names without
    // their Factor, with the extension's defaults. Colors are linear, and a color's fourth component is unused, but for
    // the base color's alpha. The attenuation distance is in the world, and zero, or one past 3e38, is no attenuation.
    // The anisotropy's rotation is in radians counterclockwise from the tangent. The thickness is in the mesh's space.
    // `specularGlossiness` is nonzero for a material of the archived KHR_materials_pbrSpecularGlossiness: the base
    // color and its texture are then the diffuse color, the specular color and the glossiness are the workflow's, and
    // the metallic-roughness texture is the specular-glossiness texture, with the specular color in RGB and the
    // glossiness in alpha.
    struct Parameters {
        [RenderParameter(1, 1, 1, 1)] float4 baseColor;
        [RenderParameter(0, 0, 0, 0)] float4 emissive;
        [RenderParameter(1, 1, 1, 0)] float4 specularColor;
        [RenderParameter(0, 0, 0, 0)] float4 sheenColor;
        [RenderParameter(1, 1, 1, 0)] float4 attenuationColor;
        [RenderParameter(1, 1, 1, 0)] float4 diffuseTransmissionColor;
        [RenderParameter(1, 0, 0, 0)] float emissiveStrength;
        [RenderParameter(1, 0, 0, 0)] float metallic;
        [RenderParameter(1, 0, 0, 0)] float roughness;
        [RenderParameter(1, 0, 0, 0)] float normalScale;
        [RenderParameter(1, 0, 0, 0)] float occlusionStrength;
        [RenderParameter(1, 0, 0, 0)] float specular;
        [RenderParameter(0, 0, 0, 0)] float sheenRoughness;
        [RenderParameter(0, 0, 0, 0)] float attenuationDistance;
        [RenderParameter(0, 0, 0, 0)] float diffuseTransmission;
        [RenderParameter(1.5, 0, 0, 0)] float ior;
        [RenderParameter(0, 0, 0, 0)] float clearcoat;
        [RenderParameter(0, 0, 0, 0)] float clearcoatRoughness;
        [RenderParameter(1, 0, 0, 0)] float clearcoatNormalScale;
        [RenderParameter(0, 0, 0, 0)] float iridescence;
        [RenderParameter(1.3, 0, 0, 0)] float iridescenceIor;
        [RenderParameter(100, 0, 0, 0)] float iridescenceThicknessMinimum;
        [RenderParameter(400, 0, 0, 0)] float iridescenceThicknessMaximum;
        [RenderParameter(0, 0, 0, 0)] float anisotropyStrength;
        [RenderParameter(0, 0, 0, 0)] float anisotropyRotation;
        [RenderParameter(0, 0, 0, 0)] float transmission;
        [RenderParameter(0, 0, 0, 0)] float thickness;
        [RenderParameter(0, 0, 0, 0)] float dispersion;
        [RenderParameter(0, 0, 0, 0)] float specularGlossiness;
        [RenderParameter(1, 0, 0, 0)] float glossiness;
    };

    // The core material's textures, by the core type's indices, and then each extension's, in the channels that the
    // extension reads: the clearcoat in red and its roughness in green, the sheen's color in RGB and its roughness in
    // alpha, the specular strength in alpha and its color in RGB, the iridescence in red and its thickness in green,
    // the anisotropy's direction in red and green and its strength in blue, the transmission in red, the thickness in
    // green, and the diffuse transmission in alpha and its color in RGB. An extension's stand-in turns its layer off or
    // leaves its factor as it is.
    struct Textures {
        [RenderColor(1, 1, 1, 1)] RenderTexture baseColor;
        [RenderData(1, 1, 0, 1)] RenderTexture metallicRoughness;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture normal;
        [RenderData(1, 1, 1, 1)] [RenderReads("r")] RenderTexture occlusion;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture emissive;
        [RenderData(0, 0, 0, 1)] [RenderReads("r")] RenderTexture clearcoat;
        [RenderData(0, 0, 0, 1)] [RenderReads("g")] RenderTexture clearcoatRoughness;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture clearcoatNormal;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture sheenColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("a")] RenderTexture sheenRoughness;
        [RenderData(1, 1, 1, 1)] [RenderReads("a")] RenderTexture specular;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture specularColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("r")] RenderTexture iridescence;
        [RenderData(0, 0, 0, 1)] [RenderReads("g")] RenderTexture iridescenceThickness;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture anisotropy;
        [RenderData(0, 0, 0, 1)] [RenderReads("r")] RenderTexture transmission;
        [RenderData(1, 1, 1, 1)] [RenderReads("g")] RenderTexture thickness;
        [RenderData(1, 1, 1, 0)] [RenderReads("a")] RenderTexture diffuseTransmission;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture diffuseTransmissionColor;
    };

    struct Attributes {
        RENDER_KHRONOS_PBR_ATTRIBUTES
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexCoord uv[2] = {attributes.uv0, attributes.uv1};
        float4 baseColor =
            parameters.baseColor * attributes.color * renderMaterialSample(textures.baseColor, uv);
        float4 metallicRoughness = renderMaterialSample(textures.metallicRoughness, uv);
        float occlusion = renderMaterialSample(textures.occlusion, uv).r;

        float3 normal = renderMaterialNormal(textures.normal, parameters.normalScale, inputs, uv);
        RenderPbrSurface surface = renderPbrSurfaceDefault(normal);
        surface.baseColor = baseColor.rgb;
        surface.alpha = baseColor.a;
        surface.metallic = parameters.metallic * metallicRoughness.b;
        surface.roughness = parameters.roughness * metallicRoughness.g;
        surface.occlusion = 1 + parameters.occlusionStrength * (occlusion - 1);
        surface.emissive = parameters.emissive.rgb * parameters.emissiveStrength *
            renderMaterialSample(textures.emissive, uv).rgb;

        // KHR_materials_ior and KHR_materials_specular: the strength is the specular texture's alpha, and the color is
        // the specular color texture's.
        surface.ior = parameters.ior;
        float3 specularColor =
            parameters.specularColor.rgb * renderMaterialSample(textures.specularColor, uv).rgb;
        surface.specular =
            parameters.specular * renderMaterialSample(textures.specular, uv).a;
        surface.dielectricF0 = renderPbrDielectricF0(surface.ior, specularColor);

        // KHR_materials_pbrSpecularGlossiness, "Specular-Glossiness": c_diff = diffuse * (1 - max(specular)), F0 =
        // specular, and alpha = (1 - glossiness)^2.
        if (parameters.specularGlossiness != 0) {
            float3 specular = parameters.specularColor.rgb * metallicRoughness.rgb;
            surface.metallic = 0;
            surface.roughness = 1 - parameters.glossiness * metallicRoughness.a;
            surface.specular = 1;
            surface.dielectricF0 = specular;
            surface.baseColor = baseColor.rgb * (1 - max(specular.r, max(specular.g, specular.b)));
        }

        // KHR_materials_clearcoat: the strength in red, the roughness in green, and the coat's own normal map over the
        // mesh's normal. A coat without a normal map is as smooth as the mesh, whatever the base's normal map says.
        if (parameters.clearcoat > 0) {
            surface.clearcoat =
                parameters.clearcoat * renderMaterialSample(textures.clearcoat, uv).r;
            surface.clearcoatRoughness = parameters.clearcoatRoughness *
                renderMaterialSample(textures.clearcoatRoughness, uv).g;
            surface.clearcoatNormal =
                renderMaterialNormal(textures.clearcoatNormal, parameters.clearcoatNormalScale, inputs, uv);
        }

        // KHR_materials_sheen: the color in the color texture, and the roughness in the roughness texture's alpha.
        if (any(parameters.sheenColor.rgb > 0)) {
            surface.sheenColor =
                parameters.sheenColor.rgb * renderMaterialSample(textures.sheenColor, uv).rgb;
            surface.sheenRoughness =
                parameters.sheenRoughness * renderMaterialSample(textures.sheenRoughness, uv).a;
        }

        // KHR_materials_iridescence: the strength in red, and the thickness in green, between the least and the most.
        if (parameters.iridescence > 0) {
            surface.iridescence =
                parameters.iridescence * renderMaterialSample(textures.iridescence, uv).r;
            surface.iridescenceIor = parameters.iridescenceIor;
            surface.iridescenceThickness = parameters.iridescenceThicknessMaximum;
            if (renderMaterialHasTexture(textures.iridescenceThickness)) {
                surface.iridescenceThickness = lerp(parameters.iridescenceThicknessMinimum,
                    parameters.iridescenceThicknessMaximum, renderMaterialSample(textures.iridescenceThickness, uv).g);
            }
            // A film without thickness is no film (pbr.frag).
            if (surface.iridescenceThickness <= 0)
                surface.iridescence = 0;
        }

        // KHR_materials_anisotropy: the texture's red and green are a direction in the tangent's frame, which the
        // rotation turns, and its blue scales the strength. The frame is the mesh's tangent about the shaded normal.
        if (parameters.anisotropyStrength > 0) {
            float2 direction = float2(1, 0);
            float strength = parameters.anisotropyStrength;
            if (renderMaterialHasTexture(textures.anisotropy)) {
                float3 sampled = renderMaterialSample(textures.anisotropy, uv).xyz;
                direction = sampled.xy * 2 - 1;
                strength *= sampled.z;
            }
            float2 turn = float2(cos(parameters.anisotropyRotation), sin(parameters.anisotropyRotation));
            direction =
                float2(turn.x * direction.x - turn.y * direction.y, turn.y * direction.x + turn.x * direction.y);
            float3 t = inputs.tangent.xyz - normal * dot(normal, inputs.tangent.xyz);
            float length2 = dot(t, t);
            if (length2 < 1e-12) {
                t = cross(normal, abs(normal.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0));
                length2 = dot(t, t);
            }
            t *= rsqrt(length2);
            float3 b = cross(normal, t) * (inputs.tangent.w < 0 ? -1.0 : 1.0);
            surface.anisotropy = saturate(strength);
            surface.anisotropyDirection = t * direction.x + b * direction.y;
        }

        // KHR_materials_diffuse_transmission: the part in the texture's alpha, and the color in the color texture.
        if (parameters.diffuseTransmission > 0) {
            surface.diffuseTransmission = parameters.diffuseTransmission *
                renderMaterialSample(textures.diffuseTransmission, uv).a;
            surface.diffuseTransmissionColor = parameters.diffuseTransmissionColor.rgb *
                renderMaterialSample(textures.diffuseTransmissionColor, uv).rgb;
        }

        // KHR_materials_transmission: the part in red. KHR_materials_volume: the thickness in green, in the mesh's
        // space, which the instance's scale brings to the world.
        if (parameters.transmission > 0) {
            surface.transmission =
                parameters.transmission * renderMaterialSample(textures.transmission, uv).r;
        }
        if (parameters.thickness > 0 && (surface.transmission > 0 || surface.diffuseTransmission > 0)) {
            surface.thickness = parameters.thickness *
                renderMaterialSample(textures.thickness, uv).g *
                renderWorldScale(renderInstance(inputs.instance));
        }
        surface.attenuationColor = parameters.attenuationColor.rgb;
        // An infinite distance, glTF's default, is no attenuation.
        float distance = parameters.attenuationDistance;
        surface.attenuationDistance = distance > 0 && distance < 3.0e38 ? distance : 0;
        surface.dispersion = parameters.dispersion;
        surface.position = inputs.position;
        return surface;
    }
}
