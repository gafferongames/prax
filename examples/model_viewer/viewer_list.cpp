// The model viewer's list: the model files that its paths name, in the order that the viewer steps through them.

#include "examples/model_viewer/viewer_list.h"

#include "pr/ascii.h"
#include "pr/file.h"
#include "pr/integer.h"
#include "pr/sort.h"
#include "pr/span.h"
#include "pr/string_ops.h"

namespace model_viewer {

namespace {

// The variants of a model that the list takes, in the order of preference.
constexpr PrStringView kVariants[] = {"glTF-Binary", "glTF"};

// The entries of one directory that the list reads: its model files and its directories, by name.
struct Folder {
    Array<String> models;
    Array<String> directories;
};

bool isModelFile(PrStringView name) {
    PrStringView extension = prFilePathExtension(name);
    constexpr PrStringView kExtensions[] = {".gltf", ".glb", ".usd", ".usda", ".usdc", ".usdz"};
    for (PrStringView model : kExtensions) {
        if (prAsciiEqualsFold(extension, model))
            return true;
    }
    return false;
}

bool isVariant(PrStringView name) {
    return prStringStartsWith(name, "glTF");
}

void collectEntry(void* PR_NULLABLE context, const PrFileEntry* entry) {
    Folder& folder = *static_cast<Folder*>(context);
    if (entry->info.kind == PrFileKind_Directory)
        folder.directories.push(String(entry->name));
    else if (entry->info.kind == PrFileKind_Regular && isModelFile(entry->name))
        folder.models.push(String(entry->name));
}

String joined(PrStringView folder, PrStringView name) {
    String path(folder);
    if (path.count() != 0 && path.last() != '/')
        path.append("/");
    path.append(name);
    return path;
}

// Adds the model file at `path`, unless the list holds it.
void addFile(Array<ModelEntry>& list, PrStringView path) {
    if (modelListFindPath(list, path) != kModelNone)
        return;
    PrStringView file = prFilePathName(path);
    PrStringView folder = prFilePathName(prFilePathParent(path));
    ModelEntry& entry = list.emplace();
    entry.path = path;
    entry.name = PrStringView{file.data, file.size - prFilePathExtension(file).size};
    if (isVariant(folder))
        entry.variant = folder;
}

void addDirectory(Array<ModelEntry>& list, PrStringView path) {
    Folder folder;
    prFileEachEntry(path, collectEntry, &folder);
    bool hasVariants = false;
    for (const String& directory : folder.directories)
        hasVariants = hasVariants || isVariant(directory);
    if (!hasVariants) {
        for (const String& model : folder.models)
            addFile(list, joined(path, model));
        for (const String& directory : folder.directories)
            addDirectory(list, joined(path, directory));
        return;
    }
    for (PrStringView variant : kVariants) {
        Folder files;
        String variantPath = joined(path, variant);
        prFileEachEntry(variantPath, collectEntry, &files);
        for (const String& model : files.models)
            addFile(list, joined(variantPath, model));
        if (files.models.count() != 0)
            return;
    }
}

// Orders entries by name, and entries of one name by path, so that the order does not depend on the file system.
bool entryLess(const ModelEntry& a, const ModelEntry& b) {
    int order = prAsciiCompareFold(a.name, b.name);
    if (order != 0)
        return order < 0;
    return prAsciiCompareFold(a.path, b.path) < 0;
}

// Returns true if `entry` is the file at `path` or a file under the directory at `path`.
bool isUnder(const ModelEntry& entry, PrStringView path) {
    PrStringView file = entry.path;
    if (file == path)
        return true;
    return prStringStartsWith(file, path) && (path.data[path.size - 1] == '/' || file.data[path.size] == '/');
}

} // namespace

uint32_t modelListAdd(Array<ModelEntry>& list, PrStringView path) {
    // A directory's path may end in separators, which the entries' paths do not repeat.
    while (path.size > 1 && path.data[path.size - 1] == '/')
        --path.size;
    PrFileInfo info{};
    if (path.size == 0 || !prFileInfo(path, &info))
        return kModelNone;
    if (info.kind == PrFileKind_Directory)
        addDirectory(list, path);
    else if (info.kind == PrFileKind_Regular && isModelFile(path))
        addFile(list, path);
    sortItems(Span<ModelEntry>(list.data(), list.count()), entryLess);
    for (uint32_t i = 0; i < list.count(); ++i) {
        if (isUnder(list[i], path))
            return i;
    }
    return kModelNone;
}

uint32_t modelListFindPath(const Array<ModelEntry>& list, PrStringView path) {
    for (uint32_t i = 0; i < list.count(); ++i) {
        if (list[i].path.view() == path)
            return i;
    }
    return kModelNone;
}

uint32_t modelListFind(const Array<ModelEntry>& list, PrStringView text) {
    for (uint32_t i = 0; i < list.count(); ++i) {
        if (prAsciiEqualsFold(list[i].name, text))
            return i;
    }
    uint64_t number = 0;
    if (prU64Parse(text.data, text.size, &number, 10) && number >= 1 && number <= list.count())
        return uint32_t(number - 1);
    return kModelNone;
}

} // namespace model_viewer
