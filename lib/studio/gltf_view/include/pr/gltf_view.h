// Studio's glTF structure view: what an item of a glTF asset holds, what it names and what names it.
//
// An item is an element of a collection, which is one of the asset's top-level arrays, as `/nodes`, or an array that an
// extension of the asset defines, as `/extensions/KHR_lights_punctual/lights`. A JSON Pointer names an item, as
// `/nodes/3`. The view shows what the selection names:
//
// - The asset, for an empty pointer: its `asset` object, the items that it names (its default scene), each collection
//   with the number of its items, and the extensions that it uses, with whether the engine draws each and whether the
//   asset requires it.
// - A collection: each of its items, with its name and a summary of what it holds.
// - An item, for a pointer to it or into it: its properties as the JSON gives them, the extensions that it has and
//   whether the engine draws each, the items that it names, and the items that name it. A mesh shows each primitive's
//   attributes, indices, mode, material and morph targets. An accessor shows its elements' type, their count, bounds
//   and sparse substitution, the byte ranges of the buffer views and buffers that hold them, and its elements' values,
//   as the asset's document reads them. A buffer view shows the byte range of its buffer.
//
// Each item and collection that the view shows is a link to its JSON Pointer, which the host opens as it names its own
// places: as another selection of the asset, or, for an image, a buffer view or a buffer, as the bytes that it holds
// (prGltfViewFollowed). Each table shows only the rows in its window, so an accessor of any number of elements costs a
// window's reads a round. One thread uses a view.

#pragma once

#include "pr/gltf.h"
#include "pr/json.h"
#include "pr/kit.h"
#include "pr/raw_string.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The cursors of the view's tables, and what the view knows of the asset that it last showed.
typedef struct PrGltfView PrGltfView;

// Creates a view that draws with `kit`, which outlives it, or frees one.
PrGltfView* prGltfViewCreate(PrKit* kit);
void prGltfViewDestroy(PrGltfView* PR_NULLABLE view);

// What the view shows: an item of an asset, or a collection, by its JSON Pointer, or the asset itself with an empty
// pointer. A pointer into an item shows the item. `json` is the asset's JSON, and `gltf` its document, which the view
// reads accessors' elements and byte ranges from, or null to show neither. Both outlive the round. `unshaded` is the
// material extensions that the renderer reads and does not shade, as a model reports them (prModelDiagnostic), which
// the view counts as not drawn.
typedef struct PrGltfViewContent {
    const PrJson* json;
    const PrGltf* PR_NULLABLE gltf;
    PrStringView selection;
    PrGltfMaterialExtensionFlags unshaded;
} PrGltfViewContent;

// Declares the view in a round under `parent`, which it fills, scrolling what does not fit.
void prGltfViewFrame(PrGltfView* view, PrLayoutNode parent, PrGltfViewContent content);

// Takes the JSON Pointer of the link that the user followed, which the view holds until the host takes it, and writes
// it to `*out`, which the caller releases with prRawStringReset, and returns true, or returns false if the user
// followed none since. The empty pointer is the asset itself.
bool prGltfViewFollowed(PrGltfView* view, PrRawString* out);

// How the engine treats an extension of an item.
enum {
    PrGltfViewExtension_Drawn,          // read and drawn
    PrGltfViewExtension_Read,           // read and not drawn: the item draws as if it lacked the extension
    PrGltfViewExtension_Ignored,        // not read: the document holds nothing of it
};
typedef uint8_t PrGltfViewExtension;

// Returns how the engine treats the extension `name` of an item, given the material extensions that are not shaded.
PrGltfViewExtension prGltfViewExtensionState(PrStringView name, PrGltfMaterialExtensionFlags unshaded);

// A reference of one item to another: the item at `from` names the item at `to` in the member at `path`, a JSON Pointer
// relative to `from`. Each item is a pointer such as `/meshes/0`, and `from` is the empty pointer for the asset, whose
// `scene` names its default scene. A member that holds a pointer (KHR_animation_pointer) names the item that holds what
// the pointer names.
typedef struct PrGltfViewReference {
    PrStringView from;
    PrStringView to;
    PrStringView path;
} PrGltfViewReference;

// Returns the number of references among the items of the asset that the view last showed, and the reference at
// `index`, in the order of the asset's collections, of their items, and of the members of each item, after the asset's
// own. Each view is valid until the view shows another asset.
uint32_t prGltfViewReferenceCount(const PrGltfView* view);
PrGltfViewReference prGltfViewReference(const PrGltfView* view, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
