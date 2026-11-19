// The shader sets of a renderer (pr/render.h): the material types and the shading models that a renderer draws with,
// compiled at build time with the passes that call them.
//
// A shading model and a material type are each a Slang structure that implements an interface
// (pr/render_surface.slang). A shading model's structure is its surface, with the methods that compute the radiance
// that leaves such a surface (IRenderSurface). A material type's structure declares everything that the type reads: its
// parameter record, its textures, and the attributes of a mesh's vertices, each with its semantic, its format and its
// value where a vertex lacks it. It names its shading model, and computes that model's surface (IRenderMaterial). One
// scene mixes models, since each material's type names its own. An application adds types and models of its own without
// changing the library's sources: the CMake function pr_render_shaders compiles the library's pass sources with the
// application's Slang sources into a set of the application's own, and names the set's material types.
//
//     pr_render_shaders(<target> <Name>
//         SOURCES my_materials.slang
//         TABLE Sheen EXTENT 32 32
//         MATERIAL Checker STRUCT MyChecker
//         MATERIAL Paint STRUCT MyPaint)
//
// The function adds the static library <target>, which is the set, and generates "render_shaders/<Name>Shaders.h" for
// the sources of the targets that link it. The header declares the set, as the function <name>ShaderSet, an id for each
// material type, k<Name>Material<Type>, which a material's description names (PrRenderMaterialDesc::type), the index of
// each parameter, each texture and each attribute that a type declares, and an id for each table, k<Name>Table<Table>.
// Every set has the library's four types, KhronosPbr, Unlit, KhronosPbrLayered and UsdPreview, which the Khronos PBR
// material model lights (RenderPbrSurface, pr/render_brdf.slang). A set may list the library's OpenPBR type (OpenPbr,
// pr/render_material_openpbr.slang) as it lists any type. A loader that does not know a renderer's set finds a type by
// its name (prRenderMaterialTypeFromName).
//
// The shader is in charge of what it reads. A type's attributes are a layout of a vertex (PrRenderVertexLayout), which
// types of equal declarations share, and a mesh holds its attributes in the layouts of the types that draw it
// (PrRenderMeshStream). An optional attribute is in the layout of a variant of the type alone, so a mesh carries it
// only where a material binds it. A producer converts what its source holds into a layout when it sets a mesh's data
// (prMeshAttributeConvert), so no shader decodes a layout that is described at run time.
//
// A parameter record has the size of its structure, in a multiple of 16 bytes. It holds floats, 32-bit integers, and
// vectors of two or four of them, each at a multiple of its alignment without help. Under those rules C and each
// shading language read the same bytes (pr/gpu.slang). Each field is a parameter, marked with its value where a
// material does not set it, or padding, so a loader fills a record by its parameters' names
// (PrRenderMaterialType::parameters). A type declares as many textures as its function samples. A material's record on
// the GPU has the size of its type's textures and parameters, so a material of a small type does not pay for a large
// one (PrRenderMaterialRecord).
//
// What is wrong with a set fails the build. The Slang compiler reports a structure that does not implement its
// interface, where the source declares it: a method that is missing or of another form, a material type whose function
// does not return its model's surface, and a surface without an alpha. The build reports a MATERIAL whose structure the
// sources lack, a parameter record that breaks the rules above, a texture or an attribute that is not marked as the
// interface asks, a source that declares slots of its own, and a member that is named as a method with a default and
// does not replace it.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/render_records.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Fills a table of a shader set: writes four floats for each texel of a table of `extent`, row after row from the row
// at v = 0, each row from the texel at u = 0. A texel holds what the table's function is at its place: the first texel
// of an axis is at 0 and the last at 1 (renderTableSample, pr/render_table.slang). The renderer stores the floats as
// half floats, and a shader reads the table between its texels by interpolating the four about a place. `context` is
// the caller's. Called once when a renderer is created, on the thread that creates it.
typedef void (*PrRenderTableFillFn)(void* PR_NULLABLE context, PrUVec2 extent, float* texels);

// A lookup table of a shader set, which the set's shading models and material types sample (pr/render_table.slang). A
// renderer keeps the tables of its set in one texture, each at its `origin` there.
typedef struct PrRenderTable {
    PrStringView name;
    PrUVec2 origin;                 // of the table's first texel in the texture of the set's tables
    PrUVec2 extent;                 // the table's width and height in texels

    // What fills the table, for a table of the library's own. Null for a table of an application's, which gives the
    // function when it creates a renderer (PrRenderOptions::tables).
    PrRenderTableFillFn PR_NULLABLE fill;
} PrRenderTable;

// What fills one table of a renderer's shader set (PrRenderOptions::tables). `table` is the table's id, which the set's
// header names: k<Name>Table<Table>.
typedef struct PrRenderTableSource {
    uint32_t table;
    PrRenderTableFillFn fill;
    void* PR_NULLABLE context;
} PrRenderTableSource;

// Fills the library's own table, which every shader set has first: the split sum of the library's specular lobe, by the
// roughness along u and the square root of the cosine of the eye's angle to the normal along v, with the scale in red
// and the bias in green (renderSplitSum, pr/render_shading.slang). It reads no context.
void prRenderSplitSumTable(void* PR_NULLABLE context, PrUVec2 extent, float* texels);

// The passes whose shaders call the materials and the shading models, and which a shader set therefore compiles.
enum {
    PrRenderShaderPass_DepthMask,   // the depth of masked materials, of every type in one shader
    PrRenderShaderPass_Forward,     // a fragment shader for each material type
    PrRenderShaderPass_Visibility,  // every type in one shader
};
typedef uint8_t PrRenderShaderPass;
enum { kPrRenderShaderPassCount = 3 };

// What an attribute of a mesh's vertices means.
enum {
    PrRenderSemantic_Uv,        // a set of texture coordinates, which a material's textures choose among
    PrRenderSemantic_Color,     // a color in linear light, with alpha
    PrRenderSemantic_Custom,    // a value that only the material types that declare it know
};
typedef uint8_t PrRenderSemantic;

// An attribute of a layout of a vertex, as a material type declares it.
typedef struct PrRenderVertexAttribute {
    PrStringView name;              // the field's name in the type's declaration
    PrRenderSemantic semantic;
    uint8_t semanticIndex;          // among the layout's attributes of the semantic, in their order: a set of a uv
    PrMeshFormat format;            // as a vertex stores it, which is a vertex format of lib/gpu
    uint32_t offset;                // in bytes into a vertex of the layout

    // The value of a vertex that lacks the attribute, as the shader reads it, of which the attribute has as many
    // components as its format.
    PrRenderVec4 fallback;
} PrRenderVertexAttribute;

// A layout of a vertex's attributes: what a material type reads of a mesh's vertices beside their geometry, in its
// order, packed. A shader set holds one layout for each declaration of attributes that its types make, so types whose
// declarations are equal share one, and two layouts of one set are equal only if they are one. A layout is constant
// data of the program. The names of entry points are those of the set's libraries that draw with the layout.
typedef struct PrRenderVertexLayout {
    const PrRenderVertexAttribute* PR_NULLABLE attributes;
    uint32_t attributeCount;
    uint32_t stride;                // the size in bytes of a vertex of the layout
    PrStringView forwardVertex;
    PrStringView depthMaskVertex;
    PrStringView depthMaskFragment;
} PrRenderVertexLayout;

// Returns the attribute of `layout` that `name` names, or kPrRenderVertexAttributeMissing.
uint32_t prRenderVertexAttributeFromName(const PrRenderVertexLayout* layout, PrStringView name);
PR_CONSTANT uint32_t kPrRenderVertexAttributeMissing = 0xffffffffu;

// What a producer holds of one attribute of its vertices, in whatever format it holds it, and what it is: a semantic
// and its index, such as glTF's TEXCOORD_1, or a name, which a custom attribute of a layout matches.
typedef struct PrRenderAttributeSource {
    PrRenderSemantic semantic;
    uint8_t semanticIndex;
    PrStringView name;          // for PrRenderSemantic_Custom
    PrMeshAttributeView values;
} PrRenderAttributeSource;

// Writes `vertexCount` vertices of `layout` to `out`, `layout->stride` bytes each: each attribute converted from the
// source that matches it, by its semantic and index or, for a custom attribute, by its name, or its fallback where no
// source matches or a source lacks a vertex (prMeshAttributeConvert). Sets `*missing`, if it is given, to the number of
// the layout's attributes that no source matches. Returns InvalidData for a source whose values cannot be read.
PrStatus prRenderVertexLayoutFill(const PrRenderVertexLayout* layout, const PrRenderAttributeSource* PR_NULLABLE sources,
    uint32_t sourceCount, uint32_t vertexCount, void* out, uint32_t* PR_NULLABLE missing);

// What a material type's texture holds, which decides how a loader encodes its texels: color, encoded as sRGB, or
// values that a shader reads as they are stored, such as normals and roughness.
enum {
    PrRenderTextureKind_Color,
    PrRenderTextureKind_Data,
};
typedef uint8_t PrRenderTextureKind;

// The channels of a texture's value that a material type reads: red to alpha, or the one channel that a material
// chooses for a scalar input (PrRenderTextureUse::channel).
enum {
    PrRenderChannelFlag_R = 1u << 0,
    PrRenderChannelFlag_G = 1u << 1,
    PrRenderChannelFlag_B = 1u << 2,
    PrRenderChannelFlag_A = 1u << 3,
    PrRenderChannelFlag_Rgb = PrRenderChannelFlag_R | PrRenderChannelFlag_G | PrRenderChannelFlag_B,
    PrRenderChannelFlag_Rgba = PrRenderChannelFlag_Rgb | PrRenderChannelFlag_A,
    PrRenderChannelFlag_Chosen = 1u << 4,
};
typedef uint8_t PrRenderChannelFlags;

// A texture of a material type, as the type declares it.
typedef struct PrRenderMaterialTexture {
    PrStringView name;              // the field's name in the type's declaration
    PrRenderTextureKind kind;
    PrRenderChannelFlags channels;  // the channels of its value that the type reads

    // The value that the type reads while a material names the texture and its texels have not arrived, or the texture
    // was destroyed. On a device without bindless, a texture that a material names past the textures that one draw
    // samples reads as this value too (prRenderMaterialTextureLimit). A texture that a material does not name reads as
    // white.
    PrRenderVec4 standIn;
} PrRenderMaterialTexture;

// What a parameter of a material type holds: floats, or 32-bit integers, signed or not.
enum {
    PrRenderParameterKind_Float,
    PrRenderParameterKind_Int,
    PrRenderParameterKind_Uint,
};
typedef uint8_t PrRenderParameterKind;

// A parameter of a material type, as the type declares it: a field of its parameter record, of one to four components,
// or an array of `elementCount` such elements, one after another, and its value where a material does not set it, of
// which each element has as many components.
typedef struct PrRenderMaterialParameter {
    PrStringView name;              // the field's name in the type's declaration
    uint32_t offset;                // in bytes into the record
    uint8_t componentCount;
    uint8_t elementCount;           // one for a field that is not an array
    PrRenderParameterKind kind;
    PrRenderVec4 value;
} PrRenderMaterialParameter;

// A material type of a shader set.
//
// A type's attributes may be optional (RenderOptional, pr/render_surface.slang). Each set of the optional attributes
// that a mesh carries is a variant of the type, whose layout holds the attributes that are not optional and those of
// the set, and whose shaders read each optional attribute outside the set as its value where a vertex lacks it. A
// material names the set (PrRenderMaterialDesc::attributes), so a mesh carries only the attributes that its materials
// bind.
typedef struct PrRenderMaterialType {
    PrStringView name;
    PrStringView shadingModel;      // the shading model that lights the type's surfaces: its structure's name
    uint32_t parameterBytes;        // the size of the type's parameter record

    // The parameters of the type's record, by the type's parameter index, or null for none.
    const PrRenderMaterialParameter* PR_NULLABLE parameters;
    uint32_t parameterCount;

    // The textures that a material of the type names, by the type's texture index, or null for none.
    const PrRenderMaterialTexture* PR_NULLABLE textures;
    uint32_t textureCount;

    // The names of the type's optional attributes, by their index among them, which is their bit in a set of them, or
    // null for none.
    const PrStringView* PR_NULLABLE optionalAttributes;
    uint32_t optionalAttributeCount;

    // The type's variants among the set's, one for each set of its optional attributes, in the order of the sets' bits:
    // the variant of the set `attributes` is `firstVariant + attributes`.
    uint32_t firstVariant;
    uint32_t variantCount;
} PrRenderMaterialType;

// Returns the texture of `type` that `name` names, or kPrRenderMaterialTextureMissing.
uint32_t prRenderMaterialTextureFromName(const PrRenderMaterialType* type, PrStringView name);
PR_CONSTANT uint32_t kPrRenderMaterialTextureMissing = 0xffffffffu;

// Returns the parameter of `type` that `name` names, or kPrRenderMaterialParameterMissing.
uint32_t prRenderMaterialParameterFromName(const PrRenderMaterialType* type, PrStringView name);
PR_CONSTANT uint32_t kPrRenderMaterialParameterMissing = 0xffffffffu;

// Writes a parameter record of `type` at `record`, `type->parameterBytes` bytes, that holds each parameter's value
// where a material does not set it, and zero in its padding.
void prRenderMaterialParametersDefault(const PrRenderMaterialType* type, void* record);

// Writes `values`, `count` numbers, as the first components of parameter `parameter` of `type` in the record at
// `record`, converted to the parameter's kind: an array's elements one after another. Components past `count`, or past
// the parameter's, are left.
void prRenderMaterialParameterSet(const PrRenderMaterialType* type, void* record, uint32_t parameter, const float* values,
    uint32_t count);

// A variant of a material type: the type, the set of its optional attributes that the variant reads, the fragment entry
// point of the forward pass's library that shades it, and the layout of its attributes, by its index in the set.
typedef struct PrRenderMaterialVariant {
    uint32_t type;
    uint32_t attributes;
    PrStringView forwardFragment;
    uint32_t layout;
} PrRenderMaterialVariant;

// A shader set: the shader libraries of the passes that call the set's materials and shading models, compiled for a
// device with bindless (`handles`) and for the slots alone (`slots`), the set's material types, by id, the layouts of
// their attributes, and their variants. A pass that needs bindless has no library for the slots. The set that
// pr_render_shaders generates is constant data of the program.
typedef struct PrRenderShaderSet {
    PrStringView name;
    PrByteView handles[kPrRenderShaderPassCount];   // by PrRenderShaderPass
    PrByteView slots[kPrRenderShaderPassCount];
    const PrRenderMaterialType* materialTypes;
    uint32_t materialTypeCount;
    const PrRenderVertexLayout* layouts;
    uint32_t layoutCount;
    const PrRenderMaterialVariant* variants;
    uint32_t variantCount;

    // The set's tables, and the width and height of the texture that holds them all.
    const PrRenderTable* tables;
    uint32_t tableCount;
    PrUVec2 tablesExtent;
} PrRenderShaderSet;

// Returns the library's own shader set, which has the library's material types and the shading model that it ships
// alone. A renderer that is created with no set draws with it. pr_render_shaders builds it as it builds an
// application's.
const PrRenderShaderSet* prRenderShaderSetLibrary(void);

// Returns the id of the material type of `set` that `name` names, or kPrRenderMaterialTypeMissing: a loader that does
// not know a renderer's set finds a type by its name, such as "OpenPbr" for the library's OpenPBR type.
uint32_t prRenderMaterialTypeFromName(const PrRenderShaderSet* set, PrStringView name);
PR_CONSTANT uint32_t kPrRenderMaterialTypeMissing = 0xffffffffu;

// Returns the layout of the attributes that a material of `type` of `set` reads of a mesh that carries `attributes`,
// the type's optional attributes that the material names (PrRenderMaterialDesc::attributes).
const PrRenderVertexLayout* prRenderMaterialTypeLayout(const PrRenderShaderSet* set, uint32_t type,
    uint32_t attributes);

// Returns the layout of the geometry that every mesh has, which every pass reads and deformation writes, as a layout of
// attributes: a position of three floats, and a normal and a tangent with its handedness in four signed normalized
// 16-bit values each (PrRenderGeometryVertex). A producer converts its positions, normals and tangents into it as it
// converts its other attributes into a type's layout (prMeshAttributeConvert). It holds no entry points.
const PrRenderVertexLayout* prRenderGeometryLayout(void);

#if defined(__cplusplus)
}

// The most bytes of a parameter record that PrRenderParameterRecord holds.
constexpr uint32_t kPrRenderMaxParameterBytes = 1024;

// A parameter record of a material type, which C++ fills by the names of the type's parameters. It begins with each
// parameter's value where a material does not set it. A name that the type lacks sets nothing. The type's record is at
// most kPrRenderMaxParameterBytes.
struct PrRenderParameterRecord {
    const PrRenderMaterialType* type;
    alignas(16) uint8_t bytes[kPrRenderMaxParameterBytes];

    explicit PrRenderParameterRecord(const PrRenderMaterialType* recordType) : type(recordType), bytes{} {
        prRenderMaterialParametersDefault(type, bytes);
    }

    // Sets the first components of the parameter `name` to `values`.
    template <uint32_t N>
    PrRenderParameterRecord& set(PrStringView name, const float (&values)[N]) {
        uint32_t parameter = prRenderMaterialParameterFromName(type, name);
        if (parameter != kPrRenderMaterialParameterMissing)
            prRenderMaterialParameterSet(type, bytes, parameter, values, N);
        return *this;
    }

    // Returns component `component` of the float parameter `name`, or zero for a name that the type lacks.
    float value(PrStringView name, uint32_t component = 0) const {
        uint32_t parameter = prRenderMaterialParameterFromName(type, name);
        if (parameter == kPrRenderMaterialParameterMissing)
            return 0;
        float out = 0;
        prMemCopy(&out, bytes + type->parameters[parameter].offset + component * 4, 4);
        return out;
    }
};
#endif

PR_ASSUME_NONNULL_END
