// The OpenPBR material type: OpenPBR Surface's inputs, each the value of a texture of its own where the material names
// one, as the surface of the OpenPBR shading model (pr/render_openpbr.slang), which lights itself. A shader set has the
// type if it lists it, as it lists any type.
//
// Each texture is one input's, read in the channel that the material's network reads and scaled and biased as the
// network scales it (PrRenderMaterialTextureRecord), so a MaterialX graph that reads roughness and metalness from two
// images, or one channel of an image of four, draws as it is written. The base color and the opacity are times the
// vertex's color and its alpha, so a geometric property that a graph reads into them is a value of that attribute.

#pragma once

#include "pr/render_material.slang"
#include "pr/render_material_khronos_pbr.slang"

#include "pr/render_openpbr.slang"

struct RenderMaterialOpenPbr : IRenderMaterial {
    typedef RenderOpenPbrSurface Surface;

    // The inputs of OpenPBR Surface (the parameter tables of the OpenPBR Surface specification v1.1, section "Model"),
    // by their names in the specification, run together, with the specification's defaults. Colors are linear, and
    // their fourth components are unused. The emission's luminance is in the scene's units of radiance: nits, for
    // lights in lux. The thin film's thickness is in micrometers, and the transmission's depth in the world's units.
    // The tangents are the angles in radians by which they are turned from the mesh's tangent, counterclockwise about
    // the normal. A thin wall is nonzero.
    //
    // It has one number that the specification does not have: the length of the path of a transmitted ray through the
    // object, in the mesh's space, which an instance's scale brings to the world, in place of the object's interior
    // that a path tracer finds. Zero reads the scene straight behind the surface.
    struct Parameters {
        [RenderParameter(0.8, 0.8, 0.8, 0)] float4 baseColor;
        [RenderParameter(1, 1, 1, 0)] float4 specularColor;
        [RenderParameter(1, 1, 1, 0)] float4 coatColor;
        [RenderParameter(1, 1, 1, 0)] float4 fuzzColor;
        [RenderParameter(1, 1, 1, 0)] float4 emissionColor;
        [RenderParameter(1, 1, 1, 0)] float4 transmissionColor;
        [RenderParameter(0, 0, 0, 0)] float4 transmissionScatter;
        [RenderParameter(0.8, 0.8, 0.8, 0)] float4 subsurfaceColor;
        [RenderParameter(1, 0.5, 0.25, 0)] float4 subsurfaceRadius;
        [RenderParameter(1, 0, 0, 0)] float baseWeight;
        [RenderParameter(1, 0, 0, 0)] float specularWeight;
        [RenderParameter(0, 0, 0, 0)] float coatWeight;
        [RenderParameter(0, 0, 0, 0)] float fuzzWeight;
        [RenderParameter(0, 0, 0, 0)] float emissionLuminance;
        [RenderParameter(0, 0, 0, 0)] float transmissionWeight;
        [RenderParameter(0, 0, 0, 0)] float transmissionScatterAnisotropy;
        [RenderParameter(0, 0, 0, 0)] float subsurfaceWeight;
        [RenderParameter(1, 0, 0, 0)] float subsurfaceRadiusScale;
        [RenderParameter(0, 0, 0, 0)] float baseMetalness;
        [RenderParameter(0.3, 0, 0, 0)] float specularRoughness;
        [RenderParameter(1.5, 0, 0, 0)] float specularIor;
        [RenderParameter(0, 0, 0, 0)] float coatRoughness;
        [RenderParameter(1.6, 0, 0, 0)] float coatIor;
        [RenderParameter(1, 0, 0, 0)] float coatDarkening;
        [RenderParameter(0.5, 0, 0, 0)] float fuzzRoughness;
        [RenderParameter(1, 0, 0, 0)] float geometryOpacity;
        [RenderParameter(0, 0, 0, 0)] float thinFilmWeight;
        [RenderParameter(0.5, 0, 0, 0)] float thinFilmThickness;
        [RenderParameter(1.4, 0, 0, 0)] float thinFilmIor;
        [RenderParameter(0, 0, 0, 0)] float baseDiffuseRoughness;
        [RenderParameter(0, 0, 0, 0)] float specularRoughnessAnisotropy;
        [RenderParameter(0, 0, 0, 0)] float coatRoughnessAnisotropy;
        [RenderParameter(0, 0, 0, 0)] float transmissionDepth;
        [RenderParameter(0, 0, 0, 0)] float transmissionDispersionScale;
        [RenderParameter(20, 0, 0, 0)] float transmissionDispersionAbbeNumber;
        [RenderParameter(0, 0, 0, 0)] float subsurfaceScatterAnisotropy;
        [RenderParameter(0, 0, 0, 0)] float geometryThinWalled;
        [RenderParameter(0, 0, 0, 0)] float geometryTangentRotation;
        [RenderParameter(0, 0, 0, 0)] float geometryCoatTangentRotation;
        [RenderParameter(0, 0, 0, 0)] float thickness;
        float _pad;
    };

    // A texture of each input, by the specification's names. The textures that matter most come first, so a device with
    // slots samples the base color, the normal, the roughness, the metalness and the opacity.
    struct Textures {
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture baseColor;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture geometryNormal;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture specularRoughness;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture baseMetalness;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture geometryOpacity;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture emissionColor;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture baseWeight;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture specularWeight;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture specularColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture coatWeight;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture coatRoughness;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture coatColor;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture geometryCoatNormal;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture transmissionWeight;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture transmissionColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture subsurfaceWeight;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture subsurfaceColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture fuzzWeight;
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture fuzzColor;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture fuzzRoughness;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture thinFilmWeight;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture thinFilmThickness;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture baseDiffuseRoughness;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture specularRoughnessAnisotropy;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture emissionLuminance;
    };

    // The attributes of glTF's types, so that a mesh's layout serves both.
    struct Attributes {
        RENDER_KHRONOS_PBR_ATTRIBUTES
    };

    // Returns a color input: the texture's value if the material names one, and else `value`.
    static float3 color(RenderTexture texture, RenderTexCoord uv[2], float3 value) {
        if (!renderMaterialHasTexture(texture))
            return value;
        return renderMaterialSample(texture, uv).rgb;
    }

    // Returns a scalar input: the channel of the texture that it reads if the material names one, and else `value`.
    static float scalar(RenderTexture texture, RenderTexCoord uv[2], float value) {
        if (!renderMaterialHasTexture(texture))
            return value;
        return renderMaterialScalar(texture, uv);
    }

    // Returns the unit normal that a normal texture gives in the frame of the mesh's tangent, or `normal` if the
    // material names no such texture or the mesh has no tangents.
    static float3 mapped(RenderTexture texture, RenderTexCoord uv[2], RenderSurfaceInputs inputs, float3 normal) {
        if (!renderMaterialHasTexture(texture))
            return normal;
        return renderMaterialFrameNormal(inputs, normal, renderMaterialSample(texture, uv).xyz);
    }

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexCoord uv[2] = {attributes.uv0, attributes.uv1};
        float3 baseColor = color(textures.baseColor, uv, parameters.baseColor.rgb);
        float opacity =
            scalar(textures.geometryOpacity, uv, parameters.geometryOpacity);
        baseColor *= attributes.color.rgb;
        opacity *= attributes.color.a;
        float3 normal = mapped(textures.geometryNormal, uv, inputs, inputs.normal);

        RenderOpenPbrSurface surface = renderOpenPbrSurfaceDefault(normal);
        surface.baseColor =
            baseColor * scalar(textures.baseWeight, uv, parameters.baseWeight);
        surface.alpha = saturate(opacity);
        surface.specularColor =
            color(textures.specularColor, uv, parameters.specularColor.rgb);
        surface.specularWeight =
            scalar(textures.specularWeight, uv, parameters.specularWeight);
        surface.coatColor = color(textures.coatColor, uv, parameters.coatColor.rgb);
        surface.coatWeight =
            saturate(scalar(textures.coatWeight, uv, parameters.coatWeight));
        surface.fuzzColor = color(textures.fuzzColor, uv, parameters.fuzzColor.rgb);
        surface.fuzzWeight =
            saturate(scalar(textures.fuzzWeight, uv, parameters.fuzzWeight));
        surface.emission = color(textures.emissionColor, uv, parameters.emissionColor.rgb) *
            scalar(textures.emissionLuminance, uv, parameters.emissionLuminance);
        surface.metalness =
            saturate(scalar(textures.baseMetalness, uv, parameters.baseMetalness));
        surface.specularRoughness = saturate(
            scalar(textures.specularRoughness, uv, parameters.specularRoughness));
        surface.specularIor = parameters.specularIor;
        surface.coatRoughness =
            saturate(scalar(textures.coatRoughness, uv, parameters.coatRoughness));
        surface.coatIor = parameters.coatIor;
        surface.coatDarkening = parameters.coatDarkening;
        surface.fuzzRoughness =
            saturate(scalar(textures.fuzzRoughness, uv, parameters.fuzzRoughness));
        surface.thinFilmWeight =
            saturate(scalar(textures.thinFilmWeight, uv, parameters.thinFilmWeight));
        surface.thinFilmThickness =
            scalar(textures.thinFilmThickness, uv, parameters.thinFilmThickness);
        surface.thinFilmIor = parameters.thinFilmIor;

        surface.diffuseRoughness = saturate(scalar(textures.baseDiffuseRoughness, uv,
            parameters.baseDiffuseRoughness));
        surface.transmissionWeight = saturate(
            scalar(textures.transmissionWeight, uv, parameters.transmissionWeight));
        surface.transmissionColor =
            color(textures.transmissionColor, uv, parameters.transmissionColor.rgb);
        surface.transmissionDepth = parameters.transmissionDepth;
        if (parameters.transmissionDispersionScale > 0) {
            surface.dispersion =
                parameters.transmissionDispersionScale / max(parameters.transmissionDispersionAbbeNumber, 1e-3);
        }
        surface.subsurfaceWeight = saturate(
            scalar(textures.subsurfaceWeight, uv, parameters.subsurfaceWeight));
        surface.subsurfaceColor =
            color(textures.subsurfaceColor, uv, parameters.subsurfaceColor.rgb);
        surface.thinWalled = parameters.geometryThinWalled != 0;
        surface.position = inputs.position;
        if (surface.transmissionWeight > 0)
            surface.thickness = parameters.thickness * renderWorldScale(renderInstance(inputs.instance));

        // The coat's normal is its own map over the mesh's normal, whatever the base's map says.
        if (surface.coatWeight > 0)
            surface.coatNormal = mapped(textures.geometryCoatNormal, uv, inputs, inputs.normal);

        // The tangents of the base and of the coat: the mesh's tangent, turned about each one's normal.
        surface.specularAnisotropy = saturate(scalar(textures.specularRoughnessAnisotropy, uv,
            parameters.specularRoughnessAnisotropy));
        surface.coatAnisotropy = parameters.coatRoughnessAnisotropy;
        if (surface.specularAnisotropy > 0)
            surface.tangent = renderMaterialTangent(inputs, normal, parameters.geometryTangentRotation);
        if (surface.coatAnisotropy > 0) {
            surface.coatTangent =
                renderMaterialTangent(inputs, surface.coatNormal, parameters.geometryCoatTangentRotation);
        }
        return surface;
    }
}
