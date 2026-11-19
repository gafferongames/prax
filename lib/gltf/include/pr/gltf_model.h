// glTF's model source (pr/model_source.h): a glTF document described for a model, and the model format of glTF assets.
//
// The source's terms are glTF's mapped onto a model's: a node is a node of the document, a draw is a node that has a
// mesh, with its skin and its instances (EXT_mesh_gpu_instancing), a part is a primitive that forms triangles, and a
// material is of the Khronos PBR model, or unlit (KHR_materials_unlit), whose inputs are named by the paths of their
// properties within the material, as KHR_animation_pointer names them: `pbrMetallicRoughness/baseColorFactor`,
// `normalTexture`, `extensions/KHR_materials_clearcoat/clearcoatFactor`. A texture input's texture is the factor's
// multiplier. A material has the inputs of the extensions that it has, and of those that an animation's pointers reach.
// An animation's channels are its tracks. A node's address is its JSON Pointer, `/nodes/3`.

#pragma once

#include "pr/base.h"
#include "pr/gltf.h"
#include "pr/model_source.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a source of a glTF document draws: the scene whose nodes are its roots. kPrGltfNone means the document's default
// scene, or its first if it names no default, or every node without a parent if it has no scene.
typedef struct PrGltfModelOptions {
    PrGltfSceneIndex scene;
} PrGltfModelOptions;

// Returns options of the document's default scene.
PrGltfModelOptions prGltfModelOptionsDefault(void);

// Creates a source of `gltf`, which outlives it. The source reads the document's accessors of animations, skins and
// instances when it is created, and its meshes and images when they are read. Null `options` means the defaults.
PrModelSource* prGltfModelSourceCreate(const PrGltf* gltf, const PrGltfModelOptions* PR_NULLABLE options);

// Returns the model format of glTF assets, JSON and binary (model/gltf+json and model/gltf-binary), which reads the
// buffers and images that an asset names by URI through its opening's resolver, at the URI relative to the asset's
// location, when it opens the asset, and whose sources own their documents.
PrModelFormat prGltfModelFormat(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
