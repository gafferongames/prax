// The model viewer's list: the model files that its paths name, in the order that the viewer steps through them.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace model_viewer {

using namespace pr;

// A model file of the list.
struct ModelEntry {
    String path;
    String name;        // the file's name without its extension
    String variant;     // the directory of the file if it names a variant of a model, such as glTF-Binary, or empty
};

// The index that names no entry of a list.
constexpr uint32_t kModelNone = UINT32_MAX;

// Adds to `list` the model that `path` names, or the models under the directory that it names, and keeps the list
// sorted by name. Returns the index of the first entry of the list that the path names, or kModelNone if it names no
// model. A path that the list already holds is not added again.
//
// A model file is a .gltf or .glb file. A directory is searched with the directories under it. Khronos's sample assets
// keep the variants of one model in sibling directories whose names begin with glTF. A directory with such directories
// in it is one model, and gives the files of its glTF-Binary variant, or else of its glTF variant, or else nothing.
uint32_t modelListAdd(Array<ModelEntry>& list, PrStringView path);

// Returns the index of the entry for the file at `path`, as the list spells it, or kModelNone.
uint32_t modelListFindPath(const Array<ModelEntry>& list, PrStringView path);

// Returns the index of the entry that `text` names: an entry's name, ignoring case, or else its number in the list,
// from one. Returns kModelNone if it names none.
uint32_t modelListFind(const Array<ModelEntry>& list, PrStringView text);

} // namespace model_viewer

PR_ASSUME_NONNULL_END
