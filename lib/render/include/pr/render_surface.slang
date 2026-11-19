// The contract between lib/render's materials, shading models and techniques, as interfaces.
//
// A shading model is a structure that implements IRenderSurface, or IRenderSimpleSurface for a model that prepares
// nothing. Its fields are the model's surface: what the model reads at a point. Its methods compute the radiance that
// leaves such a surface (pr/render_shading.slang). A material type is a structure that implements IRenderMaterial: it
// declares everything that it reads, which are its parameter record, its textures and the attributes of a mesh's
// vertices, names the shading model that lights it, and computes that model's surface (pr/render_material.slang). A
// technique decides where each one runs, and every technique calls the same materials and the same models, through
// generic functions that the library writes once.
//
// A technique reads one thing of a surface, whatever its model: `alpha`, the part of the point that the surface covers,
// which the passes that draw depth test against a masked material's cutoff. Every other field is between a material and
// its model.
//
// The compiler checks the interfaces where a structure declares that it implements one, and reports a member that is
// missing or of another form at that structure, in the source that defines it.

#pragma once

#include "pr/render_records.h"

// What a point of any surface offers a material: its geometry, which every mesh has. What else a material reads at the
// point is in the attributes that its type declares.
struct RenderSurfaceInputs {
    float3 position;    // in the world

    // The surface's frame in the world, as the mesh's vertices give it: the unit normal, and the tangent with the
    // handedness of the bitangent in w. A mesh without tangents gives a tangent of zero. On the back face of a
    // double-sided material, the normal and the tangent are reversed, so that the frame faces the viewer.
    float3 normal;
    float4 tangent;

    // The unit normal of the triangle that holds the point, in the world, on the side of `normal`. It is the same over
    // a triangle, whatever the vertices' normals are.
    float3 geometricNormal;

    uint instance;      // the id of the instance
    uint material;      // the id of the material
};

// A set of texture coordinates at a point, with their derivatives across one pixel to the right and one pixel down. An
// attribute of the semantic "uv" is one (RenderVertex). Every technique gives the derivatives: a draw's fragment shader
// from its neighboring pixels, and a technique that shades away from the triangle's draw from the triangle itself. Two
// techniques need not give the same numbers.
struct RenderTexCoord {
    float2 value;
    float2 dx;
    float2 dy;
};

// ---------------------------------------------------------------------------------------------------------------------
// Declarations
//
// A material type declares its parameters, its textures and the attributes that it reads of a mesh's vertices as the
// fields of three structures, each field marked with what the build reads of it. The build reflects them into the
// shader set (pr/render_shaders.h), with an index for each in the set's header, and generates the code that fills them
// for each technique. A field's name is its name in C.

// Marks a texture of a material type that holds color, encoded as sRGB, and gives the stand-in value that the type
// reads while a texture that a material names has no texels: four numbers, red to alpha, as the shader reads them.
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderColorAttribute {
    float r;
    float g;
    float b;
    float a;
};

// Marks a texture of a material type that holds values that a shader reads as they are stored, such as normals and
// roughness, with its stand-in value.
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderDataAttribute {
    float r;
    float g;
    float b;
    float a;
};

// Marks a texture of a material type with the channels of its value that the type reads, which a loader may store
// alone: some of "r", "g", "b" and "a", in that order, or "chosen" for the one channel that a material chooses for a
// scalar input (renderMaterialScalar, pr/render_material.slang). A texture without the mark reads all four.
//
//     [RenderData(1, 1, 1, 1)] [RenderReads("gb")] RenderTexture metallicRoughness;
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderReadsAttribute {
    string channels;
};

// Marks an attribute of a mesh's vertices that a material type reads: its semantic, the format that a vertex stores it
// in, and the value that a vertex without it has, four numbers of which the field reads as many as it has components.
//
// The semantic is "uv", a set of texture coordinates, whose field is a RenderTexCoord and whose sets a material's
// textures choose among; "color", a color in linear light, whose field is a float4; or "custom", an attribute that only
// the type knows, whose field is of the format's components. The format is a vertex format of lib/gpu
// (PrGpuVertexFormat), by its name in lowercase: "float2", "half4", "unorm8x4", "uint" and the others. A producer
// converts its attributes into the format when it sets a mesh's data (prMeshAttributeConvert), so no shader decodes a
// format.
//
//     struct Attributes {
//         [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv0;
//         [RenderVertex("custom", "float", 0, 0, 0, 0)] float temperature;
//     };
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderVertexAttribute {
    string semantic;
    string format;
    float x;
    float y;
    float z;
    float w;
};

// Marks an attribute of a mesh's vertices that a material type reads only where a material binds it. Each set of a
// type's optional attributes is a variant of the type, whose layout holds those of the set, and whose shaders read each
// of the others as its value where a vertex lacks it.
//
//     [RenderVertex("custom", "half4", 1, 1, 1, 1)] [RenderOptional] float4 emissiveColor;
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderOptionalAttribute {
};

// Marks a parameter of a material type, a field of its parameter record, with its value where a material does not set
// it: four numbers, of which the field reads as many as it has components. A field named _pad, or with that prefix, is
// padding and is not marked.
//
//     struct Parameters {
//         [RenderParameter(1, 1, 1, 1)] float4 baseColor;
//         [RenderParameter(0.5, 0, 0, 0)] float roughness;
//     };
[__AttributeUsage(_AttributeTargets.Var)]
struct RenderParameterAttribute {
    float x;
    float y;
    float z;
    float w;
};

// A texture that a material type declares, as the type's function samples it (pr/render_material.slang). The build
// fills each field of the type's Textures with one, so the index of each is a constant where the type names it.
struct RenderTexture {
    uint record;        // the offset of the texture's record in the materials' buffer
    uint index;         // the texture's place in its type's declaration

    // True if the type declares no more textures than a draw binds on a device without bindless. The texture is then in
    // the slot of its index, which the compiler knows. A larger type's textures are in the slots that their records
    // name.
    bool slotIsIndex;
};

// A light of the scene as it arrives at a point of a surface.
struct RenderShadingLight {
    float3 toLight;     // the unit direction from the point toward the middle of the light's source
    float3 irradiance;  // what the light gives a plane that faces it at the point, before any shadow

    // The part of the light that reaches the point past what shadows it, from 0 to 1. It is 1 for a light that casts no
    // shadows.
    float visibility;

    // The angle from the middle of the light's source to its edge, as the point sees it, in radians. Zero is a source
    // without size. A model may widen a smooth lobe's highlight by it, in place of a least roughness.
    float angularRadius;
};

// What every surface has, which the passes read: its coverage.
interface IRenderCoverage {
    // The part of the point that the surface covers, from 0 to 1. A float field of this name provides it. A model whose
    // surfaces are always opaque sets it to 1.
    property float alpha { get; }
}

// A shading model: the surface that it reads at a point, as the fields of the structure that implements this, and the
// radiance that leaves such a surface toward the eye.
//
// The library shades a pixel in two stages (renderShade, pr/render_shading.slang). It calls `prepare` once, with the
// direction to the eye. The result is the model's Context: what the model computes of the surface and the eye alone,
// such as a lobe's albedo toward the eye or a Fresnel term at the eye's angle. It then passes that context to the three
// methods that return radiance, and sums them: the environment's light, each of the scene's lights, and what the
// surface emits. A term that `prepare` computes is therefore computed once for a pixel, not once for each light.
//
//     struct MyContext {
//         float nv;
//     };
//
//     struct MySurface : IRenderSurface {
//         typedef MyContext Context;
//         float3 color;
//         float alpha;
//         float3 normal;
//         Context prepare(float3 toEye) { ... }
//         float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) { ... }
//         float3 indirectRadiance(Context context, float3 toEye) { ... }
//     }
//
// A model that prepares nothing implements IRenderSimpleSurface, which has no context.
//
// A model declares no slots and no root data. It reads the lights that it is given, the scene's environment through
// pr/render_environment.slang, and the tables that its shader set declares through pr/render_table.slang.
interface IRenderSurface : IRenderCoverage {
    // What `prepare` computes for a pixel. A structure of the model's own.
    associatedtype Context;

    // Returns what the model computes once for a pixel, from the surface and `toEye`, the unit direction toward the
    // eye.
    Context prepare(float3 toEye);

    // Returns the radiance that leaves the surface toward `toEye` under one light.
    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light);

    // Returns the radiance that leaves the surface toward `toEye` under the scene's environment. A model that has
    // occlusion dims the environment's light by it here.
    float3 indirectRadiance(Context context, float3 toEye);

    // Returns the radiance that the surface emits toward `toEye`. A model whose surfaces emit defines it, with
    // `override`.
    float3 emittedRadiance(Context context, float3 toEye) {
        return float3(0, 0, 0);
    }
}

// A shading model that prepares nothing: IRenderSurface without a context. The library makes every structure that
// implements this an IRenderSurface, so a material type names such a model as it names any other.
//
//     struct MySurface : IRenderSimpleSurface {
//         float3 color;
//         float alpha;
//         float3 normal;
//         float3 directRadiance(float3 toEye, RenderShadingLight light) { ... }
//         float3 indirectRadiance(float3 toEye) { ... }
//     }
interface IRenderSimpleSurface : IRenderCoverage {
    // Returns the radiance that leaves the surface toward `toEye`, a unit direction, under one light.
    float3 directRadiance(float3 toEye, RenderShadingLight light);

    // Returns the radiance that leaves the surface toward `toEye` under the scene's environment.
    float3 indirectRadiance(float3 toEye);

    // Returns the radiance that the surface emits toward `toEye`. A model whose surfaces emit defines it, with
    // `override`.
    float3 emittedRadiance(float3 toEye) {
        return float3(0, 0, 0);
    }
}

// The context of a model that prepares nothing.
struct RenderNoContext {};

extension<S : IRenderSimpleSurface> S : IRenderSurface {
    typedef RenderNoContext Context;

    Context prepare(float3 toEye) {
        return RenderNoContext();
    }

    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) {
        return this.directRadiance(toEye, light);
    }

    float3 indirectRadiance(Context context, float3 toEye) {
        return this.indirectRadiance(toEye);
    }

    override float3 emittedRadiance(Context context, float3 toEye) {
        return this.emittedRadiance(toEye);
    }
}

// A material type: a function from a point of a mesh, the attributes of its vertices there, a parameter record and a
// material's textures to the surface of the shading model that lights the type. The structure that implements this has
// no fields. A shader set names it (pr_render_shaders, pr/render_shaders.h).
//
//     struct MyMaterial : IRenderMaterial {
//         typedef MyParameters Parameters;
//         typedef MySurface Surface;
//         struct Textures {
//             [RenderColor(1, 1, 1, 1)] RenderTexture color;
//         };
//         struct Attributes {
//             [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv;
//         };
//         static MySurface surface(RenderSurfaceInputs inputs, Attributes attributes, MyParameters parameters,
//                 Textures textures) { ... }
//     }
//
// A material type serves one model. A material of one model's that should be lit by another is a second type, whose
// `surface` calls the first's and fills the other model's surface from what it returns.
interface IRenderMaterial {
    // The structure of the type's parameter record, which C fills and a shader loads (pr/render_shaders.h states what a
    // record may hold).
    associatedtype Parameters;

    // The shading model that lights the type's surfaces: a structure that implements IRenderSurface or
    // IRenderSimpleSurface.
    associatedtype Surface : IRenderSurface;

    // The type's textures: a structure of RenderTexture fields, each marked RenderColor or RenderData. A type without
    // textures declares an empty structure.
    associatedtype Textures;

    // The attributes of a mesh's vertices that the type reads: a structure of fields, each marked RenderVertex. The
    // type's layout of a vertex is theirs, in their order (PrRenderVertexLayout). A type that reads none declares an
    // empty structure.
    associatedtype Attributes;

    // Returns the surface at a point of a mesh. It samples textures only through pr/render_material.slang's functions,
    // reads nothing but its arguments, the library's functions and the scene's time (renderSceneTime), and so computes
    // the same surface in every pass. It is the one source of a material's alpha.
    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures);
}
