// Material types and shading models that each break one rule of a shader set. Each of the tests' broken sets names one
// of the mistakes, which the build of that set reports (test_render_shaders.cmake). A structure that does not implement
// its interface fails where it is declared, so each of those is compiled only by the set that defines its macro.

#pragma once

#include "pr/render_material.slang"
#include "pr/render_shading.slang"

struct GoodParameters {
    [RenderParameter(1, 1, 1, 1)] float4 color;
};

// The textures and the attributes of a type that reads neither.
#define BAD_NOTHING_READ    \
    struct Textures {};     \
    struct Attributes {};

// A shading model of this file's own.
struct GoodSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance(float3 toEye) {
        return color;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Parameter records that C and the shaders would not read alike. Their types implement the interface.

// A record with a 3-vector, which C and the shaders lay out differently.
struct BadVectorParameters {
    [RenderParameter(0, 0, 0, 0)] float3 color;
    [RenderParameter(0, 0, 0, 0)] float strength;
};

// A record with a field that is not marked with its value, which a loader would not know.
struct BadMarkParameters {
    float4 color;
};

// A record whose 4-vector follows 4 bytes, which C moves to byte 16.
struct BadOrderParameters {
    [RenderParameter(0, 0, 0, 0)] float strength;
    [RenderParameter(0, 0, 0, 0)] float4 color;
    float _pad[3];
};

// A record of 12 bytes, which is not a multiple of 16.
struct BadSizeParameters {
    [RenderParameter(0, 0, 0, 0)] float values[3];
};

// A material type of the library's model with the parameter record P.
struct RecordMaterial<P> : IRenderMaterial {
    typedef P Parameters;
    typedef RenderPbrSurface Surface;
    BAD_NOTHING_READ

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        return renderPbrSurfaceDefault(inputs.normal);
    }
}
typedef RecordMaterial<BadVectorParameters> VectorMaterial;
typedef RecordMaterial<BadMarkParameters> MarkMaterial;
typedef RecordMaterial<BadOrderParameters> OrderMaterial;
typedef RecordMaterial<BadSizeParameters> SizeMaterial;

// ---------------------------------------------------------------------------------------------------------------------
// Material types that do not implement IRenderMaterial

#if defined(TEST_BAD_MISSING_SURFACE)

// A material type without the function that computes its surface.
struct LostMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef RenderPbrSurface Surface;
    BAD_NOTHING_READ
}

#elif defined(TEST_BAD_ARGUMENT_ORDER)

// A material type whose function takes its arguments in another order than a pass calls it with.
struct SwappedMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef RenderPbrSurface Surface;
    BAD_NOTHING_READ

    static Surface surface(Parameters parameters, RenderSurfaceInputs inputs, Attributes attributes, Textures textures) {
        return renderPbrSurfaceDefault(inputs.normal);
    }
}

#elif defined(TEST_BAD_RETURN)

// A material type whose function returns a color and not a surface.
struct ColoredMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef RenderPbrSurface Surface;
    BAD_NOTHING_READ

    static float4 surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        return parameters.color;
    }
}

#elif defined(TEST_BAD_WRONG_SURFACE)

// A material type whose function returns the surface of another model than the one that the type names.
struct CrossedMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef GoodSurface Surface;
    BAD_NOTHING_READ

    static RenderPbrSurface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
            Textures textures) {
        return renderPbrSurfaceDefault(inputs.normal);
    }
}

#elif defined(TEST_BAD_MISSING_PARAMETERS)

// A material type that names no parameter record.
struct BareMaterial : IRenderMaterial {
    typedef RenderPbrSurface Surface;
    BAD_NOTHING_READ
    BAD_NOTHING_READ

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, GoodParameters parameters,
        Textures textures) {
        return renderPbrSurfaceDefault(inputs.normal);
    }
}

#elif defined(TEST_BAD_LOST_SURFACE)

// A material type whose model is a structure that nothing defines.
struct DeepMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef LostSurface Surface;
    BAD_NOTHING_READ

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        Surface surface = {};
        return surface;
    }
}

#elif defined(TEST_BAD_PLAIN_SURFACE)

// A material type whose surface is a structure that is no shading model.
struct PlainMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef GoodParameters Surface;
    BAD_NOTHING_READ

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        return parameters;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Shading models that do not implement IRenderSimpleSurface or IRenderSurface

#elif defined(TEST_BAD_INDIRECT)

// A shading model whose indirect light takes no direction to the eye.
struct DimSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance() {
        return color;
    }
}

#elif defined(TEST_BAD_EMISSION)

// A shading model whose emission takes no direction to the eye.
struct LampSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance(float3 toEye) {
        return color;
    }

    override float3 emittedRadiance() {
        return color;
    }
}

#elif defined(TEST_BAD_SILENT_EMISSION)

// A shading model whose emission has another form than the interface's and is not marked `override`. The compiler takes
// it for a method of the model's own, and would call the interface's default, which emits nothing.
struct QuietSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance(float3 toEye) {
        return color;
    }

    float3 emittedRadiance() {
        return color;
    }
}

#elif defined(TEST_BAD_EMISSION_FIELD)

// A shading model with a field named as the interface's method, which is therefore not the model's emission.
struct GlowSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;
    float3 emittedRadiance;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance(float3 toEye) {
        return color;
    }
}

#elif defined(TEST_BAD_ALPHA)

// A shading model without an alpha, which no pass could test against a mask's cutoff.
struct SolidSurface : IRenderSimpleSurface {
    float3 color;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        return color;
    }

    float3 indirectRadiance(float3 toEye) {
        return color;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and attributes that are not declared as a shader set reads them

#elif defined(TEST_BAD_UNMARKED_TEXTURE)

// A texture without its kind and its stand-in.
struct UnmarkedMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {
        RenderTexture color;
    };
    struct Attributes {};

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        return renderPbrSurfaceDefault(inputs.normal);
    }
}

#elif defined(TEST_BAD_UNMARKED_ATTRIBUTE) || defined(TEST_BAD_FORMAT) || defined(TEST_BAD_SEMANTIC) || \
    defined(TEST_BAD_ATTRIBUTE_TYPE)

// An attribute without the semantic, format and fallback that a vertex stores it by, or with one that lib/render does
// not know or does not read as the attribute's type.
struct AttributedMaterial : IRenderMaterial {
    typedef GoodParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {};
    struct Attributes {
#if defined(TEST_BAD_UNMARKED_ATTRIBUTE)
        float heat;
#elif defined(TEST_BAD_FORMAT)
        [RenderVertex("custom", "float5", 0, 0, 0, 0)] float heat;
#elif defined(TEST_BAD_SEMANTIC)
        [RenderVertex("warmth", "float", 0, 0, 0, 0)] float heat;
#else
        [RenderVertex("custom", "float2", 0, 0, 0, 0)] float heat;
#endif
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = float3(attributes.heat, 0, 0);
        return surface;
    }
}

#endif
