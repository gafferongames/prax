// A model's material system: how the source's materials, textures and attribute streams become the renderer's.
//
// The binder chooses the renderer's material type of each material by its shading model, maps the material's named
// inputs onto the type's textures and parameters, binds the source's attribute streams by name to the attributes that
// the type declares, and realizes the source's images as the renderer's textures. It is the only part of the model that
// uses lib/texture.

#pragma once

#include "pr/base.h"
#include "pr/model_source.h"
#include "pr/raw_array.h"
#include "pr/render_resource.h"

#include "model/src/model.h"
#include "model/src/model_mesh.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Where one component of a layout's attribute comes from, for the vertices of one part: the sum of the request's stream
// `stream`'s channels times `weights`, plus `offset`, or the attribute's fallback for a stream of none.
struct ModelComponentSource {
    uint32_t stream;            // of the mesh's request, or kModelStreamNone
    float weights[4];
    float offset;
};

// The streams that a mesh's request reads, by name, which a binding adds to.
struct ModelStreamNames {
    RawArray<PrStringView> names;

    // Returns the index of the stream `name`, adding it if the request lacks it.
    uint32_t add(PrStringView name);
};

// Reads what the binder needs of the description once it arrives: each texture's image and the renderer's textures that
// show them, which images are used, and each part's preparation.
void modelBindDescribe(Model* m);

// Chooses each material's type in the model's renderer, creates the renderer's textures, materials and environment, and
// sets the materials. Sets the layouts of the types that the model's materials draw with.
void modelBindPopulate(Model* m);

// Sets a material of the renderer to the source's material `material` with `inputs`, the inputs of every material as
// the source gives them or as a pose changes them, or to the default material for kPrModelMaterialNone. Returns the
// number of its textures that the renderer's device does not sample.
uint32_t modelBindMaterial(Model* m, PrRenderMaterial target, PrModelMaterialIndex material,
    const PrModelSourceInput* PR_NULLABLE inputs);

// Returns true if a material of the source emits, or is unlit.
bool modelBindEmits(const Model* m, PrModelMaterialIndex material);
bool modelBindIsUnlit(const Model* m, PrModelMaterialIndex material);

// Writes to `out`, by component, where each component of the layout's attribute `attribute` comes from for the vertices
// of the source's part `part`, adding the streams that it reads to `names`.
void modelBindAttribute(const Model* m, PrModelPartIndex part, const PrRenderVertexAttribute& attribute,
    ModelStreamNames* names, ModelComponentSource* out);

// Returns the stream of texture coordinates that a part's tangents follow, adding it to `names`, or kModelStreamNone.
uint32_t modelBindTangentStream(const Model* m, PrModelPartIndex part, ModelStreamNames* names);

// Starts loading an image's job into the model's renderer, which finishes the job once its steps complete.
void modelBindLoadImage(ModelJob* job);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
