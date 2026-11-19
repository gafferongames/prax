// USD's model source (pr/model_source.h): a composed stage reified for a model, and the model format of USD layers and
// packages.
//
// The source maps the stage's schemas (pr/usd_geom.h) onto a model's terms. A node is a prim of the stage's namespace
// with its instances expanded, so that every instance of a prototype has nodes of its own, and a node's address is the
// path of the prim that it shows, through instances, as `/World/Car`. A prim that is not active, not loaded or not
// defined, a class, a material and its network, and a prim of a purpose that the context does not draw, with what is
// under it, are no nodes. The roots take the stage's `metersPerUnit` and `upAxis`, so the source's scene is in meters
// with +y up.
//
// A mesh is a Mesh prim, or an implicit surface's mesh, with the binding that its ancestors give it, and its parts are
// its subsets of the `materialBind` family, then the faces that no subset holds. Its vertices are its faces' corners,
// with what each primitive variable gives each corner at its interpolation, and t of texture coordinates turned to v
// down the image. A draw is a mesh's node, or a point instancer's mesh of a prototype at every visible instance.
//
// A material is a preview surface, of UsdPreviewSurface's inputs by their names, or a MaterialX network, of OpenPBR's
// inputs as lib/materialx maps its surface's (pr/materialx_openpbr.h), each folded to a value, a texture or a primitive
// variable (prUsdMtlxInputRead). A prim that binds no material draws its display color and opacity. A light is what
// UsdLux describes: its kind, its shape and sizes in meters, its shaping cone and its luminance, and the first dome
// light is the environment.
//
// Reification. A source reifies its stage in one composition context (PrUsdModelContext): its description is what the
// stage composes in that context, resolved when the source is made, and a model that draws it resolves no value of the
// stage again. The context's time codes are one animation, of tracks keyed at the time samples that decide each value
// that varies and at each time code between them: the transforms, visibility, the lights' color, intensity and cones,
// the cameras' projections, and the values of the materials' inputs. A value that varies and that no track holds, such
// as a mesh's points, is reified at the context's start, and a diagnostic names it. A source of the same stage in the
// same context has the same description, and a mesh or an image reads the same each time.
//
// A stage is immutable, so a change of the stage is a new stage, and a change of the context is a new source: what
// shows a stage makes a new source of it, and a new model of the source, for either.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/model_source.h"
#include "pr/usd_stage.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a source reifies of a stage. Zero is the stage's own context: its variant selections, the default and render
// purposes, and the stage's `startTimeCode` and `endTimeCode`.
typedef struct PrUsdModelContext {
    // The session's variant selections, which the stage composes (PrUsdStageOptions::variantSelections). A source of a
    // stage that a caller composed reports a selection that the stage does not make (`variant-selections`).
    uint32_t variantSelectionCount;
    const PrUsdPrimVariantSelection* PR_NULLABLE variantSelections;

    // The purposes drawn beside `default` (UsdGeomImageable's `purpose`): `proxy` in place of `render`, as a preview
    // draws, and `guide` too if `guides` is set.
    bool proxy;
    bool guides;

    // The time codes that the animation spans, if `hasTimeRange`, and else the stage's. The rest pose is at the start,
    // and a range that does not end after its start is no animation.
    bool hasTimeRange;
    double startTimeCode;
    double endTimeCode;
} PrUsdModelContext;

// What a source of a stage reifies, and what it opens the images that its layers name with: a provider of the resource
// at a location that lib/usd resolves (prUsdResourceSource), which it opens when it is created. A null `open` is
// prUsdFileOpen.
typedef struct PrUsdModelOptions {
    PrUsdModelContext context;
    PrUsdOpenFn PR_NULLABLE open;
    void* PR_NULLABLE openContext;
    const PrCompressCodecSet* PR_NULLABLE codec;     // decodes the packages' files, as PrUsdOptions::codec does
} PrUsdModelOptions;

// Returns options of the stage's own context that open images from the file system.
PrUsdModelOptions prUsdModelOptionsDefault(void);

// Creates a source of `stage`, which outlives it, in the options' context, whose variant selections the stage was
// composed with. The source reads the stage's schemas when it is created, and its meshes when they are read. Null
// `options` means the defaults.
PrModelSource* prUsdModelSourceCreate(const PrUsdStage* stage, const PrUsdModelOptions* PR_NULLABLE options);

// What the model format of USD composes and reifies each stage with: the codec that decodes crate files' blocks and
// packages' files, without which a crate file does not read, and the context of each source, whose variant selections
// each stage composes.
typedef struct PrUsdModelFormatDesc {
    const PrCompressCodecSet* PR_NULLABLE codec;
    PrUsdModelContext context;
} PrUsdModelFormatDesc;

// Returns the model format of USD layers and packages (model/vnd.usda, model/x-usdc and model/vnd.usdz+zip), which
// composes a stage of the resource as its root layer, opens what its layers name through its opening's resolver, at the
// locations that lib/usd resolves against the root's location, and whose sources own their stages. `desc`, with what it
// points to, outlives the format.
PrModelFormat prUsdModelFormat(const PrUsdModelFormatDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
