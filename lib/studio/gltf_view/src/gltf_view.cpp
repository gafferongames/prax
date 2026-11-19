// Studio's glTF structure view: what an item of a glTF asset holds, what it names and what names it.

#include "pr/gltf_view.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/kit_box.h"
#include "pr/kit_collection.h"
#include "pr/kit_text.h"
#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// The height of a table's row, in px, and the most rows that a table shows before it scrolls.
constexpr float kRowHeight = 22;
constexpr uint32_t kTableRows = 14;

// The most numbers of an array that a property's row shows, and how deep the rows of an item's properties reach.
constexpr uint32_t kShownNumbers = 16;
constexpr uint32_t kPropertyDepth = 8;

// The most columns of the view's tables: an element's index and the four components of a vector, or the four columns of
// a matrix.
constexpr uint32_t kMaxColumns = 5;

// The absent index of an item or a collection.
constexpr uint32_t kNone = 0xffffffffu;

// A piece of the view's text: `size` bytes from `offset`.
struct GltfViewText {
    uint32_t offset;
    uint32_t size;
};

// A reference of one item to another, as pieces of the view's text (PrGltfViewReference), and the two items, by index
// of `GltfView::items`, or kNone for the asset.
struct GltfViewLink {
    GltfViewText from;
    GltfViewText to;
    GltfViewText path;
    uint32_t fromItem;
    uint32_t toItem;
};

// An array of the asset whose elements are items: its pointer, the name of its member, and its items, which are
// consecutive in `GltfView::items`.
struct GltfViewCollection {
    GltfViewText pointer;
    GltfViewText name;
    uint32_t firstItem;
    uint32_t count;
};

// A row of the properties of an item: the member's pointer relative to the item, and the value's text.
struct GltfViewProperty {
    GltfViewText path;
    GltfViewText value;
};

// A row of a mesh's primitives: the primitive, the member's pointer relative to it, and the item that the member names,
// or kNone with the value's text.
struct GltfViewPrimitiveRow {
    uint32_t primitive;
    GltfViewText member;
    uint32_t item;
    GltfViewText value;
};

// What the view shows: a collection and an item of it, by index of `GltfView::items`, a collection alone with kNone for
// the item, or the asset with kNone for both.
struct GltfViewPlace {
    uint32_t collection;
    uint32_t item;
};

// A column of one of the view's tables, and the role and tone of its cells' text.
struct GltfViewColumn {
    PrStringView label;
    float width;
    PrKitColumnSize size;
    PrKitTextRole role;
    PrKitTone tone;
};

// The cells of a row of a table as its fill writes them: the text of each cell, and the pointer that the row links to,
// if `linked`, which the empty pointer makes a link to the asset.
struct GltfViewCells {
    PrTextBuffer text[kMaxColumns];
    PrTextBuffer link;
    bool linked;
};

// The collections that members name items of (targetOf).
enum {
    GltfViewTarget_Accessors,
    GltfViewTarget_BufferViews,
    GltfViewTarget_Buffers,
    GltfViewTarget_Cameras,
    GltfViewTarget_Images,
    GltfViewTarget_Materials,
    GltfViewTarget_Meshes,
    GltfViewTarget_Nodes,
    GltfViewTarget_Samplers,
    GltfViewTarget_Scenes,
    GltfViewTarget_Skins,
    GltfViewTarget_Textures,
    GltfViewTarget_Lights,          // KHR_lights_punctual
    GltfViewTarget_Variants,        // KHR_materials_variants
    GltfViewTarget_Packets,         // KHR_xmp_json_ld
    GltfViewTarget_Count,
    GltfViewTarget_None = GltfViewTarget_Count,
};
typedef uint8_t GltfViewTarget;

// The pointer of each target's collection.
constexpr PrStringView kTargetPointers[GltfViewTarget_Count] = {
    "/accessors",
    "/bufferViews",
    "/buffers",
    "/cameras",
    "/images",
    "/materials",
    "/meshes",
    "/nodes",
    "/samplers",
    "/scenes",
    "/skins",
    "/textures",
    "/extensions/KHR_lights_punctual/lights",
    "/extensions/KHR_materials_variants/variants",
    "/extensions/KHR_xmp_json_ld/packets",
};

// The cursors of the view's tables.
struct GltfViewCursors {
    PrKitSelection properties;
    PrKitSelection extensions;
    PrKitSelection names;
    PrKitSelection namedBy;
    PrKitSelection collections;
    PrKitSelection items;
    PrKitSelection primitives;
    PrKitSelection facts;
    PrKitSelection bytes;
    PrKitSelection values;
};

struct GltfView {
    PrKit* kit;
    GltfViewCursors cursors;
    GltfViewPlace shown;            // what the last round showed, whose tables the cursors are of

    // What the view indexed of the asset whose JSON is `indexed`: its collections and their items, the references among
    // the items in the order of the items that make them, and the same references ordered by the pointers of the items
    // that make them and of the items that they name, by index of `links`.
    const PrJson* PR_NULLABLE indexed;
    RawArray<char> linkText;
    RawArray<GltfViewCollection> collections;
    uint32_t targets[GltfViewTarget_Count];     // the collection of each target, or kNone if the asset has none
    RawArray<PrJsonValueIndex> items;
    RawArray<GltfViewLink> links;
    RawArray<uint32_t> bySource;
    RawArray<uint32_t> byTarget;

    // The rows of what the round shows, whose text is `rowText`.
    RawArray<char> rowText;
    RawArray<GltfViewProperty> properties;
    RawArray<GltfViewPrimitiveRow> primitiveRows;
    RawArray<PrStringView> extensionNames;
    GltfViewCells cells;

    // The JSON Pointer of the link that the user followed in the last round, or null.
    PrRawString followed;
};

PrStringView textOf(const RawArray<char>& pool, GltfViewText text) {
    return PrStringView{pool.data + text.offset, text.size};
}

GltfViewText addText(RawArray<char>& pool, PrStringView text) {
    GltfViewText added{pool.count, uint32_t(text.size)};
    pool.append(text.data, uint32_t(text.size));
    return added;
}

// Appends the reference token of an object's member to a pointer, with `~` and `/` escaped (RFC 6901).
void appendToken(PrTextBuffer* pointer, PrStringView name) {
    prTextBufferAppendChar(pointer, '/');
    for (size_t i = 0; i < name.size; ++i) {
        if (name.data[i] == '~')
            prTextBufferAppend(pointer, "~0");
        else if (name.data[i] == '/')
            prTextBufferAppend(pointer, "~1");
        else
            prTextBufferAppendChar(pointer, name.data[i]);
    }
}

void appendIndex(PrTextBuffer* pointer, uint32_t index) {
    prTextBufferAppendChar(pointer, '/');
    prTextBufferAppendU64(pointer, index, 10);
}

void appendCount(PrTextBuffer* out, uint64_t count, PrStringView one, PrStringView many) {
    prTextBufferAppendU64(out, count, 10);
    prTextBufferAppendChar(out, ' ');
    prTextBufferAppend(out, count == 1 ? one : many);
}

// Returns the index of a nonnegative whole number that a JSON value holds, or kNone.
uint32_t indexOf(const PrJson* json, PrJsonValueIndex value) {
    if (prJsonKind(json, value) != PrJsonKind_Number)
        return kNone;
    double number = prJsonNumber(json, value, -1);
    if (number < 0 || number >= double(kNone) || number != double(uint32_t(number)))
        return kNone;
    return uint32_t(number);
}

// Returns the collection that holds item `item`.
uint32_t collectionOfItem(const GltfView* v, uint32_t item) {
    uint32_t after = partitionPoint(0, v->collections.count, [&](uint32_t i) {
        return v->collections.data[i].firstItem <= item;
    });
    return after - 1;
}

// Returns the collection whose pointer is `pointer`, or kNone.
uint32_t findCollection(const GltfView* v, PrStringView pointer) {
    for (uint32_t i = 0; i < v->collections.count; ++i) {
        if (textOf(v->linkText, v->collections.data[i].pointer) == pointer)
            return i;
    }
    return kNone;
}

// Returns item `index` of a collection, or kNone if the collection has no such item.
uint32_t itemOf(const GltfView* v, uint32_t collection, uint32_t index) {
    if (collection == kNone || index >= v->collections.data[collection].count)
        return kNone;
    return v->collections.data[collection].firstItem + index;
}

// Appends the pointer of an item.
void appendItemPointer(const GltfView* v, uint32_t item, PrTextBuffer* out) {
    const GltfViewCollection& collection = v->collections.data[collectionOfItem(v, item)];
    prTextBufferAppend(out, textOf(v->linkText, collection.pointer));
    appendIndex(out, item - collection.firstItem);
}

// Returns what a pointer names among the asset's collections: an item of a collection, for a pointer to it or into it,
// a collection, or the asset, for a pointer that names neither.
GltfViewPlace placeOf(const GltfView* v, PrStringView pointer) {
    for (uint32_t c = 0; c < v->collections.count; ++c) {
        PrStringView collection = textOf(v->linkText, v->collections.data[c].pointer);
        if (!prStringStartsWith(pointer, collection))
            continue;
        if (pointer.size == collection.size)
            return GltfViewPlace{c, kNone};
        if (pointer.data[collection.size] != '/')
            continue;
        PrStringView rest = prStringFrom(pointer, collection.size + 1);
        PrStringView digits = prStringHead(rest, prStringFindByte(rest, '/'));
        uint64_t index = 0;
        bool parsed = digits.size != 0 && (digits.size == 1 || digits.data[0] != '0') &&
            prU64Parse(digits.data, digits.size, &index, 10) && index < kNone;
        return GltfViewPlace{c, parsed ? itemOf(v, c, uint32_t(index)) : kNone};
    }
    return GltfViewPlace{kNone, kNone};
}

// Returns the target whose items a member named `key` names, of an item of the collection named `collection`, in the
// extension `extension` of the item or of one of its parts, or empty outside an extension, or None if the member names
// none (glTF-2.0/#properties-reference). A texture's `source` names an image in its extensions too, as
// KHR_texture_basisu's does, and a texture info's `index` names a texture. A Draco stream's `attributes` are the
// stream's own ids, not accessors.
GltfViewTarget targetOf(PrStringView collection, PrStringView extension, PrStringView key) {
    if (extension == "KHR_draco_mesh_compression")
        return key == "bufferView" ? GltfViewTarget_BufferViews : GltfViewTarget_None;
    if (extension == "KHR_lights_punctual" && key == "light")
        return GltfViewTarget_Lights;
    if (extension == "KHR_materials_variants" && key == "variants")
        return GltfViewTarget_Variants;
    if (extension == "KHR_xmp_json_ld" && key == "packet")
        return GltfViewTarget_Packets;
    if (extension == "MSFT_lod" && key == "ids") {
        if (collection == "nodes")
            return GltfViewTarget_Nodes;
        return collection == "materials" ? GltfViewTarget_Materials : GltfViewTarget_None;
    }
    if (collection == "animations") {
        if (key == "input" || key == "output")
            return GltfViewTarget_Accessors;
        return key == "node" ? GltfViewTarget_Nodes : GltfViewTarget_None;
    }
    if (collection == "textures") {
        if (key == "source")
            return GltfViewTarget_Images;
        return key == "sampler" ? GltfViewTarget_Samplers : GltfViewTarget_None;
    }
    if (key == "index" && collection == "materials")
        return GltfViewTarget_Textures;
    struct Rule {
        PrStringView key;
        GltfViewTarget target;
    };
    static const Rule kRules[] = {
        {"mesh", GltfViewTarget_Meshes},
        {"skin", GltfViewTarget_Skins},
        {"camera", GltfViewTarget_Cameras},
        {"children", GltfViewTarget_Nodes},
        {"nodes", GltfViewTarget_Nodes},
        {"joints", GltfViewTarget_Nodes},
        {"skeleton", GltfViewTarget_Nodes},
        {"material", GltfViewTarget_Materials},
        {"indices", GltfViewTarget_Accessors},
        {"inverseBindMatrices", GltfViewTarget_Accessors},
        {"attributes", GltfViewTarget_Accessors},
        {"targets", GltfViewTarget_Accessors},
        {"bufferView", GltfViewTarget_BufferViews},
        {"buffer", GltfViewTarget_Buffers},
    };
    for (const Rule& rule : kRules) {
        if (rule.key == key)
            return rule.target;
    }
    return GltfViewTarget_None;
}

// Returns the collection of a target, or kNone if the asset has none or the target is None.
uint32_t collectionOf(const GltfView* v, GltfViewTarget target) {
    return target < GltfViewTarget_Count ? v->targets[target] : kNone;
}

// Records a reference of `fromItem`, at `from`, to `toItem`, in the member at `path`.
void addLink(GltfView* v, PrStringView from, uint32_t fromItem, uint32_t toItem, PrStringView path) {
    PrTextBuffer to = {};
    appendItemPointer(v, toItem, &to);
    GltfViewLink link{};
    link.from = addText(v->linkText, from);
    link.to = addText(v->linkText, prTextBufferView(&to));
    link.path = addText(v->linkText, path);
    link.fromItem = fromItem;
    link.toItem = toItem;
    v->links.push(link);
    prTextBufferReset(&to);
}

// Records a reference of the item at `from` for each index of an item of the collection `target` in the value at
// `index`, whose pointer relative to the item is `path`. The value is an index, or arrays and objects of them.
void addReferences(GltfView* v, const PrJson* json, PrStringView from, uint32_t fromItem, uint32_t target,
        PrJsonValueIndex index, PrTextBuffer* path) {
    PrJsonKind kind = prJsonKind(json, index);
    if (kind == PrJsonKind_Number) {
        uint32_t item = itemOf(v, target, indexOf(json, index));
        if (item != kNone)
            addLink(v, from, fromItem, item, prTextBufferView(path));
        return;
    }
    uint32_t mark = path->size;
    uint32_t ordinal = 0;
    for (PrJsonValueIndex i = prJsonFirst(json, index); i != kPrJsonMissing; i = prJsonAfter(json, index, i)) {
        if (kind == PrJsonKind_Object) {
            appendToken(path, prJsonString(json, i));
            addReferences(v, json, from, fromItem, target, i + 1, path);
        } else {
            appendIndex(path, ordinal++);
            addReferences(v, json, from, fromItem, target, i, path);
        }
        prTextBufferResize(path, mark);
    }
}

// The item that a walk records the references of: its pointer, its index of `GltfView::items`, and the name of its
// collection.
struct GltfViewWalk {
    PrStringView from;
    uint32_t fromItem;
    PrStringView collection;
};

// Walks the value at `index` of an item, whose pointer relative to the item is `path`, in the extension `extension`, or
// empty outside one, and records the references of its members.
void walkItem(GltfView* v, const PrJson* json, const GltfViewWalk& walk, PrStringView extension,
        PrJsonValueIndex index, PrTextBuffer* path) {
    PrJsonKind kind = prJsonKind(json, index);
    if (kind != PrJsonKind_Object && kind != PrJsonKind_Array)
        return;
    uint32_t mark = path->size;
    uint32_t ordinal = 0;
    for (PrJsonValueIndex i = prJsonFirst(json, index); i != kPrJsonMissing; i = prJsonAfter(json, index, i)) {
        if (kind == PrJsonKind_Array) {
            appendIndex(path, ordinal++);
            walkItem(v, json, walk, extension, i, path);
            prTextBufferResize(path, mark);
            continue;
        }
        PrStringView key = prJsonString(json, i);
        PrJsonValueIndex value = i + 1;
        appendToken(path, key);
        if (key == "extensions" && prJsonKind(json, value) == PrJsonKind_Object) {
            // Each member is an extension, whose own members name items by its rules.
            uint32_t extensionsMark = path->size;
            for (PrJsonValueIndex e = prJsonFirst(json, value); e != kPrJsonMissing; e = prJsonAfter(json, value, e)) {
                PrStringView name = prJsonString(json, e);
                appendToken(path, name);
                walkItem(v, json, walk, name, e + 1, path);
                prTextBufferResize(path, extensionsMark);
            }
        } else if (extension == "KHR_animation_pointer" && key == "pointer") {
            // The pointer names a property, which the item that holds it answers for.
            GltfViewPlace place = placeOf(v, prJsonString(json, value));
            if (place.item != kNone)
                addLink(v, walk.from, walk.fromItem, place.item, prTextBufferView(path));
        } else if (uint32_t target = collectionOf(v, targetOf(walk.collection, extension, key)); target != kNone) {
            addReferences(v, json, walk.from, walk.fromItem, target, value, path);
        } else {
            walkItem(v, json, walk, extension, value, path);
        }
        prTextBufferResize(path, mark);
    }
}

// Adds the collection of the array at `items`, whose pointer is `pointer` and whose member is named `name`.
void addCollection(GltfView* v, const PrJson* json, PrStringView pointer, PrStringView name, PrJsonValueIndex items) {
    GltfViewCollection collection{};
    collection.pointer = addText(v->linkText, pointer);
    collection.name = addText(v->linkText, name);
    collection.firstItem = v->items.count;
    for (PrJsonValueIndex item = prJsonFirst(json, items); item != kPrJsonMissing; item = prJsonAfter(json, items, item))
        v->items.push(item);
    collection.count = v->items.count - collection.firstItem;
    v->collections.push(collection);
}

// Indexes the collections of the asset whose JSON is `json`, and the references among their items.
void indexAsset(GltfView* v, const PrJson* json) {
    v->indexed = json;
    v->linkText.clear();
    v->collections.clear();
    v->items.clear();
    v->links.clear();
    v->bySource.clear();
    v->byTarget.clear();
    for (uint32_t& target : v->targets)
        target = kNone;
    if (prJsonKind(json, 0) != PrJsonKind_Object)
        return;
    PrTextBuffer pointer = {};
    for (PrJsonValueIndex key = prJsonFirst(json, 0); key != kPrJsonMissing; key = prJsonAfter(json, 0, key)) {
        PrStringView name = prJsonString(json, key);
        if (prJsonKind(json, key + 1) == PrJsonKind_Array) {
            prTextBufferClear(&pointer);
            appendToken(&pointer, name);
            addCollection(v, json, prTextBufferView(&pointer), name, key + 1);
        }
    }
    // The arrays that the asset's extensions define, after the asset's own.
    PrJsonValueIndex extensions = prJsonMember(json, 0, "extensions");
    if (prJsonKind(json, extensions) == PrJsonKind_Object) {
        for (PrJsonValueIndex e = prJsonFirst(json, extensions); e != kPrJsonMissing; e = prJsonAfter(json, extensions, e)) {
            PrJsonValueIndex extension = e + 1;
            if (prJsonKind(json, extension) != PrJsonKind_Object)
                continue;
            for (PrJsonValueIndex key = prJsonFirst(json, extension); key != kPrJsonMissing;
                    key = prJsonAfter(json, extension, key)) {
                if (prJsonKind(json, key + 1) != PrJsonKind_Array)
                    continue;
                PrStringView name = prJsonString(json, key);
                prTextBufferClear(&pointer);
                appendToken(&pointer, "extensions");
                appendToken(&pointer, prJsonString(json, e));
                appendToken(&pointer, name);
                addCollection(v, json, prTextBufferView(&pointer), name, key + 1);
            }
        }
    }

    for (uint32_t t = 0; t < GltfViewTarget_Count; ++t)
        v->targets[t] = findCollection(v, kTargetPointers[t]);

    // The asset names its default scene.
    uint32_t scene = itemOf(v, v->targets[GltfViewTarget_Scenes], indexOf(json, prJsonMember(json, 0, "scene")));
    if (scene != kNone)
        addLink(v, "", kNone, scene, "/scene");
    PrTextBuffer path = {};
    PrTextBuffer name = {};
    for (uint32_t c = 0; c < v->collections.count; ++c) {
        const GltfViewCollection& collection = v->collections.data[c];
        // The walk adds to `linkText`, so it reads the collection's name from a copy.
        prTextBufferClear(&name);
        prTextBufferAppend(&name, textOf(v->linkText, collection.name));
        for (uint32_t i = 0; i < collection.count; ++i) {
            prTextBufferClear(&pointer);
            prTextBufferAppend(&pointer, textOf(v->linkText, collection.pointer));
            appendIndex(&pointer, i);
            prTextBufferClear(&path);
            GltfViewWalk walk{
                .from = prTextBufferView(&pointer),
                .fromItem = collection.firstItem + i,
                .collection = prTextBufferView(&name),
            };
            walkItem(v, json, walk, "", v->items.data[collection.firstItem + i], &path);
        }
    }
    prTextBufferReset(&pointer);
    prTextBufferReset(&path);
    prTextBufferReset(&name);
    // Each order keeps the references of one item in the order that the item makes them.
    for (uint32_t i = 0; i < v->links.count; ++i) {
        v->byTarget.push(i);
        v->bySource.push(i);
    }
    sortItems(v->byTarget.span(), [&](uint32_t a, uint32_t b) {
        int order = prStringCompare(textOf(v->linkText, v->links.data[a].to), textOf(v->linkText, v->links.data[b].to));
        return order != 0 ? order < 0 : a < b;
    });
    sortItems(v->bySource.span(), [&](uint32_t a, uint32_t b) {
        PrStringView from = textOf(v->linkText, v->links.data[a].from);
        int order = prStringCompare(from, textOf(v->linkText, v->links.data[b].from));
        return order != 0 ? order < 0 : a < b;
    });
}

// Returns the references of `order`, by index of `links`, whose item that `side` reads is `pointer`.
template <typename Side>
Span<const uint32_t> referencesOf(const GltfView* v, const RawArray<uint32_t>& order, PrStringView pointer, Side side) {
    auto at = [&](uint32_t i) { return textOf(v->linkText, side(v->links.data[order.data[i]])); };
    uint32_t first = partitionPoint(0, order.count, [&](uint32_t i) { return prStringCompare(at(i), pointer) < 0; });
    uint32_t end = partitionPoint(first, order.count, [&](uint32_t i) { return prStringCompare(at(i), pointer) <= 0; });
    return Span<const uint32_t>(order.data + first, end - first);
}

// Appends a value's text: a scalar as JSON writes it, an array of numbers as its first numbers, and any other array or
// object as the number of what it holds.
void appendValue(PrTextBuffer* out, const PrJson* json, PrJsonValueIndex index) {
    char digits[kPrF64TextMaxBytes];
    switch (prJsonKind(json, index)) {
        case PrJsonKind_Null:
            prTextBufferAppend(out, "null");
            return;
        case PrJsonKind_False:
            prTextBufferAppend(out, "false");
            return;
        case PrJsonKind_True:
            prTextBufferAppend(out, "true");
            return;
        case PrJsonKind_Number:
            prTextBufferAppend(out, PrStringView{digits, prF64Format(prJsonNumber(json, index, 0), digits)});
            return;
        case PrJsonKind_String:
            prTextBufferAppendChar(out, '"');
            prTextBufferAppend(out, prJsonString(json, index));
            prTextBufferAppendChar(out, '"');
            return;
        case PrJsonKind_Array: {
            prTextBufferAppendChar(out, '[');
            uint32_t shown = 0;
            for (PrJsonValueIndex i = prJsonFirst(json, index); i != kPrJsonMissing && shown < kShownNumbers;
                    i = prJsonAfter(json, index, i), ++shown) {
                if (shown != 0)
                    prTextBufferAppend(out, ", ");
                appendValue(out, json, i);
            }
            if (prJsonCount(json, index) > kShownNumbers)
                prTextBufferAppend(out, ", ...");
            prTextBufferAppendChar(out, ']');
            return;
        }
        case PrJsonKind_Object:
            appendCount(out, prJsonCount(json, index), "member", "members");
            return;
        default:
            return;
    }
}

// Returns true if a value shows on one row: a scalar, or an array of scalars.
bool showsInline(const PrJson* json, PrJsonValueIndex index) {
    PrJsonKind kind = prJsonKind(json, index);
    if (kind == PrJsonKind_Object)
        return false;
    if (kind != PrJsonKind_Array)
        return true;
    for (PrJsonValueIndex i = prJsonFirst(json, index); i != kPrJsonMissing; i = prJsonAfter(json, index, i)) {
        PrJsonKind element = prJsonKind(json, i);
        if (element == PrJsonKind_Array || element == PrJsonKind_Object)
            return false;
    }
    return true;
}

// Adds a row for each value under the value at `index` that shows on one row, whose pointer relative to the item is
// `path`.
void addProperties(GltfView* v, const PrJson* json, PrJsonValueIndex index, PrTextBuffer* path, uint32_t depth) {
    if (showsInline(json, index) || depth == kPropertyDepth) {
        PrTextBuffer value = {};
        appendValue(&value, json, index);
        GltfViewProperty row{};
        row.path = addText(v->rowText, prTextBufferView(path));
        row.value = addText(v->rowText, prTextBufferView(&value));
        v->properties.push(row);
        prTextBufferReset(&value);
        return;
    }
    PrJsonKind kind = prJsonKind(json, index);
    uint32_t mark = path->size;
    uint32_t ordinal = 0;
    for (PrJsonValueIndex i = prJsonFirst(json, index); i != kPrJsonMissing; i = prJsonAfter(json, index, i)) {
        if (kind == PrJsonKind_Object) {
            appendToken(path, prJsonString(json, i));
            addProperties(v, json, i + 1, path, depth + 1);
        } else {
            appendIndex(path, ordinal++);
            addProperties(v, json, i, path, depth + 1);
        }
        prTextBufferResize(path, mark);
    }
}

// Returns the name of a component type by its code in the JSON, or empty for a code that glTF does not define.
PrStringView componentTypeName(uint32_t code) {
    switch (code) {
        case 5120:
            return "BYTE";
        case 5121:
            return "UNSIGNED_BYTE";
        case 5122:
            return "SHORT";
        case 5123:
            return "UNSIGNED_SHORT";
        case 5125:
            return "UNSIGNED_INT";
        case 5126:
            return "FLOAT";
        default:
            return "";
    }
}

uint32_t componentTypeCode(PrGltfComponentType type) {
    switch (type) {
        case PrGltfComponentType_Byte:
            return 5120;
        case PrGltfComponentType_UnsignedByte:
            return 5121;
        case PrGltfComponentType_Short:
            return 5122;
        case PrGltfComponentType_UnsignedShort:
            return 5123;
        case PrGltfComponentType_UnsignedInt:
            return 5125;
        default:
            return 5126;
    }
}

PrStringView elementTypeName(PrGltfElementType type) {
    static const PrStringView kNames[] = {"SCALAR", "VEC2", "VEC3", "VEC4", "MAT2", "MAT3", "MAT4"};
    return type < 7 ? kNames[type] : PrStringView("");
}

// Returns the name of a primitive's mode, by its code (glTF-2.0/#meshes-overview).
PrStringView modeName(uint32_t mode) {
    static const PrStringView kNames[] = {
        "POINTS", "LINES", "LINE_LOOP", "LINE_STRIP", "TRIANGLES", "TRIANGLE_STRIP", "TRIANGLE_FAN",
    };
    return mode < 7 ? kNames[mode] : PrStringView("");
}

// Appends a summary of what an item holds, as its collection reads it, or nothing for a collection without one.
void appendSummary(const GltfView* v, const PrJson* json, uint32_t item, PrTextBuffer* out) {
    PrStringView collection = textOf(v->linkText, v->collections.data[collectionOfItem(v, item)].name);
    PrJsonValueIndex value = v->items.data[item];
    auto member = [&](PrStringView key) { return prJsonMember(json, value, key); };
    auto count = [&](PrStringView key) { return prJsonCount(json, member(key)); };
    if (collection == "accessors") {
        prTextBufferAppendU64(out, uint64_t(prJsonNumber(json, member("count"), 0)), 10);
        prTextBufferAppend(out, " x ");
        prTextBufferAppend(out, prJsonString(json, member("type")));
        prTextBufferAppend(out, " of ");
        prTextBufferAppend(out, componentTypeName(indexOf(json, member("componentType"))));
        if (prJsonKind(json, member("normalized")) == PrJsonKind_True)
            prTextBufferAppend(out, ", normalized");
        PrJsonValueIndex sparse = member("sparse");
        if (sparse != kPrJsonMissing) {
            prTextBufferAppend(out, ", ");
            prTextBufferAppendU64(out, uint64_t(prJsonNumber(json, prJsonMember(json, sparse, "count"), 0)), 10);
            prTextBufferAppend(out, " sparse");
        }
    } else if (collection == "meshes") {
        appendCount(out, count("primitives"), "primitive", "primitives");
        if (count("weights") != 0) {
            prTextBufferAppend(out, ", ");
            appendCount(out, count("weights"), "weight", "weights");
        }
    } else if (collection == "nodes") {
        const PrStringView kParts[] = {"mesh", "skin", "camera"};
        for (PrStringView part : kParts) {
            uint32_t index = indexOf(json, member(part));
            if (index == kNone)
                continue;
            if (out->size != 0)
                prTextBufferAppend(out, ", ");
            prTextBufferAppend(out, part);
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppendU64(out, index, 10);
        }
        if (count("children") != 0) {
            if (out->size != 0)
                prTextBufferAppend(out, ", ");
            appendCount(out, count("children"), "child", "children");
        }
    } else if (collection == "scenes") {
        appendCount(out, count("nodes"), "node", "nodes");
    } else if (collection == "bufferViews") {
        appendCount(out, uint64_t(prJsonNumber(json, member("byteLength"), 0)), "byte", "bytes");
        prTextBufferAppend(out, " of buffer ");
        prTextBufferAppendU64(out, uint64_t(prJsonNumber(json, member("buffer"), 0)), 10);
    } else if (collection == "buffers") {
        appendCount(out, uint64_t(prJsonNumber(json, member("byteLength"), 0)), "byte", "bytes");
        PrStringView uri = prJsonString(json, member("uri"));
        if (uri.size != 0 && !prStringStartsWith(uri, "data:")) {
            prTextBufferAppend(out, ", ");
            prTextBufferAppend(out, uri);
        }
    } else if (collection == "images") {
        PrStringView uri = prJsonString(json, member("uri"));
        if (uri.size == 0 || prStringStartsWith(uri, "data:"))
            uri = prJsonString(json, member("mimeType"));
        prTextBufferAppend(out, uri);
    } else if (collection == "textures") {
        uint32_t source = indexOf(json, member("source"));
        if (source != kNone) {
            prTextBufferAppend(out, "image ");
            prTextBufferAppendU64(out, source, 10);
        }
    } else if (collection == "animations") {
        appendCount(out, count("channels"), "channel", "channels");
    } else if (collection == "skins") {
        appendCount(out, count("joints"), "joint", "joints");
    } else if (collection == "cameras" || collection == "lights") {
        prTextBufferAppend(out, prJsonString(json, member("type")));
    } else if (collection == "materials") {
        PrStringView alpha = prJsonString(json, member("alphaMode"));
        prTextBufferAppend(out, alpha.size != 0 ? alpha : PrStringView("OPAQUE"));
        if (prJsonKind(json, member("doubleSided")) == PrJsonKind_True)
            prTextBufferAppend(out, ", double-sided");
    }
}

// Returns the name of an item, or empty.
PrStringView nameOf(const GltfView* v, const PrJson* json, uint32_t item) {
    return item != kNone ? prJsonString(json, prJsonMember(json, v->items.data[item], "name")) : PrStringView{};
}

void heading(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    PrKitTextStyle style{.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted};
    PrKitBoxStyle box{.padding = {.top = 10, .bottom = 4}};
    prKitLabel(kit, prKitBox(kit, parent, {}, box), {.text = text}, style);
}

// The style of a table of `rows` rows, which grows to kTableRows rows and then scrolls.
PrKitListViewStyle tableStyle(uint32_t rows) {
    uint32_t shown = rows < kTableRows ? rows : kTableRows;
    float height = kRowHeight * float(shown + 1) + 4;
    return PrKitListViewStyle{.rowHeight = kRowHeight, .place = {.height = prKitPx(height), .flex = PrKitFlex_Fixed}};
}

// Makes the link to `pointer`, or to the asset itself for the empty pointer, the one that the user followed.
void follow(GltfView* v, PrStringView pointer) {
    prRawStringReset(&v->followed);
    v->followed = prRawStringCopy(pointer);
}

// A table of `count` rows in `columns`, of which the round creates the rows in its window, each as `fill(row, cells)`
// writes it. A row that links shows the link's icon in `linkColumn`, and selecting it follows the link.
template <typename Fill>
void linkTable(GltfView* v, PrLayoutNode parent, Span<const GltfViewColumn> columns,
        uint32_t linkColumn, uint32_t count, PrKitSelection* cursor, Fill fill) {
    PrKit* kit = v->kit;
    PR_ASSERT(columns.count() <= kMaxColumns);
    PrKitTableColumn tableColumns[kMaxColumns] = {};
    for (uint32_t c = 0; c < columns.count(); ++c) {
        tableColumns[c] = PrKitTableColumn{
            .label = columns[c].label,
            .width = columns[c].width,
            .maxWidth = columns[c].size == PrKitColumnSize_Fit ? 320.0f : 0.0f,
            .size = columns[c].size,
        };
    }
    PrKitTable content{.count = count, .selection = cursor, .columns = tableColumns, .columnCount = columns.count()};
    PrLayoutNode table = prKitTable(kit, parent, content, tableStyle(count));
    PrKitWindow window = prKitListWindow(kit, table);
    GltfViewCells& cells = v->cells;
    auto fillRow = [&](uint32_t row) {
        for (PrTextBuffer& text : cells.text)
            prTextBufferClear(&text);
        prTextBufferClear(&cells.link);
        cells.linked = false;
        fill(row, cells);
    };
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        fillRow(i);
        PrLayoutNode row = prKitTableRow(kit, table, {.index = i, .item = i, .selected = cursor->lead == i});
        for (uint32_t c = 0; c < columns.count(); ++c) {
            PrKitTableCell cell{
                .column = c,
                .text = prTextBufferView(&cells.text[c]),
                .role = columns[c].role,
                .tone = columns[c].tone,
            };
            if (c == linkColumn && cells.linked)
                cell.icon = PrStudioIconKind_Link;
            prKitTableCell(kit, row, cell);
        }
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, table, &edit) && cursor->lead < count) {
        fillRow(cursor->lead);
        if (cells.linked)
            follow(v, prTextBufferView(&cells.link));
    }
}

// Which item of a reference a table of references shows and links to.
enum {
    GltfViewToward_Named,       // the item that the reference names
    GltfViewToward_Naming,      // the item that makes the reference
};
typedef uint8_t GltfViewToward;

// A table of references, by index of `links`: the item of each that `toward` chooses, with its name, and the member
// that makes the reference.
void referencesTable(GltfView* v, PrLayoutNode parent, const PrGltfViewContent& content,
        Span<const uint32_t> references, GltfViewToward toward, PrKitSelection* cursor) {
    bool named = toward == GltfViewToward_Named;
    const GltfViewColumn columns[] = {
        {"Item", 120, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Name", 100, PrKitColumnSize_Fit, PrKitTextRole_Body, PrKitTone_Primary},
        {"Member", 160, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Secondary},
    };
    linkTable(v, parent, columns, 0, references.count(), cursor, [&](uint32_t row, GltfViewCells& cells) {
        const GltfViewLink& link = v->links.data[references[row]];
        PrStringView pointer = textOf(v->linkText, named ? link.to : link.from);
        prTextBufferAppend(&cells.text[0], pointer.size != 0 ? pointer : PrStringView("asset"));
        prTextBufferAppend(&cells.text[1], nameOf(v, content.json, named ? link.toItem : link.fromItem));
        prTextBufferAppend(&cells.text[2], textOf(v->linkText, link.path));
        prTextBufferAppend(&cells.link, pointer);
        cells.linked = true;
    });
}

PrStringView extensionStateName(PrGltfViewExtension state) {
    switch (state) {
        case PrGltfViewExtension_Drawn:
            return "drawn";
        case PrGltfViewExtension_Read:
            return "read, not drawn";
        default:
            return "not read";
    }
}

// A table of the extensions `extensionNames`, how the engine treats each, and, with `required`, an array of names,
// which of them the asset requires.
void extensionsTable(GltfView* v, PrLayoutNode parent, const PrGltfViewContent& content, PrJsonValueIndex required) {
    const PrJson* json = content.json;
    bool withRequired = required != kPrJsonMissing;
    const GltfViewColumn columns[] = {
        {"Extension", 200, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Engine", 100, PrKitColumnSize_Fit, PrKitTextRole_Body, PrKitTone_Primary},
        {"Required", 70, PrKitColumnSize_Fit, PrKitTextRole_Body, PrKitTone_Secondary},
    };
    Span<const GltfViewColumn> shown(columns, withRequired ? 3 : 2);
    uint32_t count = v->extensionNames.count;
    linkTable(v, parent, shown, kNone, count, &v->cursors.extensions, [&](uint32_t row, GltfViewCells& cells) {
        PrStringView name = v->extensionNames.data[row];
        prTextBufferAppend(&cells.text[0], name);
        prTextBufferAppend(&cells.text[1], extensionStateName(prGltfViewExtensionState(name, content.unshaded)));
        if (!withRequired)
            return;
        for (PrJsonValueIndex i = prJsonFirst(json, required); i != kPrJsonMissing; i = prJsonAfter(json, required, i)) {
            if (prJsonString(json, i) == name)
                prTextBufferAppend(&cells.text[2], "required");
        }
    });
}

// The references that the item at `pointer` makes and those that name it.
void referenceSections(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, PrStringView pointer) {
    Span<const uint32_t> names = referencesOf(v, v->bySource, pointer, [](const GltfViewLink& link) { return link.from; });
    if (names.count() != 0) {
        heading(v->kit, box, "NAMES");
        referencesTable(v, box, content, names, GltfViewToward_Named, &v->cursors.names);
    }
    Span<const uint32_t> namedBy = referencesOf(v, v->byTarget, pointer, [](const GltfViewLink& link) { return link.to; });
    if (namedBy.count() != 0) {
        heading(v->kit, box, "NAMED BY");
        referencesTable(v, box, content, namedBy, GltfViewToward_Naming, &v->cursors.namedBy);
    }
}

// The asset: its `asset` object, the items that it names, its collections, and the extensions that it uses.
void assetPage(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content) {
    PrKit* kit = v->kit;
    const PrJson* json = content.json;
    prKitLabel(kit, box, {.text = "Asset"}, {.role = PrKitTextRole_Heading});
    v->rowText.clear();
    v->properties.clear();
    PrTextBuffer text = {};
    PrJsonValueIndex asset = prJsonMember(json, 0, "asset");
    if (asset != kPrJsonMissing)
        addProperties(v, json, asset, &text, 0);
    heading(kit, box, "ASSET");
    for (const GltfViewProperty& row : Span<const GltfViewProperty>(v->properties.data, v->properties.count)) {
        prTextBufferClear(&text);
        prTextBufferAppend(&text, prStringFrom(textOf(v->rowText, row.path), 1));
        prTextBufferAppend(&text, " = ");
        prTextBufferAppend(&text, textOf(v->rowText, row.value));
        prKitLabel(kit, box, {.text = prTextBufferView(&text)}, {.role = PrKitTextRole_Mono});
    }
    prTextBufferReset(&text);
    referenceSections(v, box, content, "");

    heading(kit, box, "COLLECTIONS");
    const GltfViewColumn columns[] = {
        {"Collection", 160, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Items", 60, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Secondary},
    };
    linkTable(v, box, columns, 0, v->collections.count, &v->cursors.collections, [&](uint32_t row, GltfViewCells& cells) {
        const GltfViewCollection& collection = v->collections.data[row];
        prTextBufferAppend(&cells.text[0], textOf(v->linkText, collection.pointer));
        prTextBufferAppendU64(&cells.text[1], collection.count, 10);
        prTextBufferAppend(&cells.link, textOf(v->linkText, collection.pointer));
        cells.linked = true;
    });

    PrJsonValueIndex used = prJsonMember(json, 0, "extensionsUsed");
    v->extensionNames.clear();
    for (PrJsonValueIndex i = prJsonFirst(json, used); i != kPrJsonMissing; i = prJsonAfter(json, used, i))
        v->extensionNames.push(prJsonString(json, i));
    if (v->extensionNames.count != 0) {
        heading(kit, box, "EXTENSIONS USED");
        extensionsTable(v, box, content, prJsonMember(json, 0, "extensionsRequired"));
    }
}

// A collection: each of its items, with its name and a summary.
void collectionPage(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, uint32_t index) {
    PrKit* kit = v->kit;
    const GltfViewCollection& collection = v->collections.data[index];
    PrTextBuffer title = {};
    prTextBufferAppend(&title, textOf(v->linkText, collection.pointer));
    prTextBufferAppend(&title, "  ");
    appendCount(&title, collection.count, "item", "items");
    prKitLabel(kit, box, {.text = prTextBufferView(&title)}, {.role = PrKitTextRole_Heading});
    prTextBufferReset(&title);
    heading(kit, box, "ITEMS");
    const GltfViewColumn columns[] = {
        {"Item", 120, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Name", 120, PrKitColumnSize_Fit, PrKitTextRole_Body, PrKitTone_Primary},
        {"Summary", 160, PrKitColumnSize_Fill, PrKitTextRole_Body, PrKitTone_Secondary},
    };
    linkTable(v, box, columns, 0, collection.count, &v->cursors.items, [&](uint32_t row, GltfViewCells& cells) {
        uint32_t item = collection.firstItem + row;
        appendItemPointer(v, item, &cells.text[0]);
        prTextBufferAppend(&cells.text[1], nameOf(v, content.json, item));
        appendSummary(v, content.json, item, &cells.text[2]);
        appendItemPointer(v, item, &cells.link);
        cells.linked = true;
    });
}

// Adds a row of a mesh's primitives for the member at `path` of primitive `primitive`, whose value names an item of
// `collection`.
void addPrimitiveRow(GltfView* v, const PrJson* json, uint32_t primitive, PrStringView path, uint32_t collection,
        PrJsonValueIndex value) {
    GltfViewPrimitiveRow row{.primitive = primitive, .item = itemOf(v, collection, indexOf(json, value))};
    row.member = addText(v->rowText, path);
    if (row.item == kNone) {
        PrTextBuffer text = {};
        appendValue(&text, json, value);
        row.value = addText(v->rowText, prTextBufferView(&text));
        prTextBufferReset(&text);
    }
    v->primitiveRows.push(row);
}

// Adds a row for each member of the object at `object`, a primitive's attributes or a morph target, whose pointer
// relative to the primitive is `path`.
void addAttributeRows(GltfView* v, const PrJson* json, uint32_t primitive, PrJsonValueIndex object, PrTextBuffer* path,
        uint32_t accessors) {
    uint32_t mark = path->size;
    for (PrJsonValueIndex i = prJsonFirst(json, object); i != kPrJsonMissing; i = prJsonAfter(json, object, i)) {
        appendToken(path, prJsonString(json, i));
        addPrimitiveRow(v, json, primitive, prTextBufferView(path), accessors, i + 1);
        prTextBufferResize(path, mark);
    }
}

// A mesh's primitives: of each, its attributes, its indices, its mode, its material, its morph targets and its Draco
// stream, each linked to the item that it names.
void primitivesSection(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, PrJsonValueIndex mesh) {
    const PrJson* json = content.json;
    PrJsonValueIndex primitives = prJsonMember(json, mesh, "primitives");
    if (prJsonCount(json, primitives) == 0)
        return;
    uint32_t accessors = v->targets[GltfViewTarget_Accessors];
    uint32_t materials = v->targets[GltfViewTarget_Materials];
    uint32_t bufferViews = v->targets[GltfViewTarget_BufferViews];
    v->primitiveRows.clear();
    PrTextBuffer path = {};
    uint32_t primitive = 0;
    for (PrJsonValueIndex p = prJsonFirst(json, primitives); p != kPrJsonMissing;
            p = prJsonAfter(json, primitives, p), ++primitive) {
        prTextBufferClear(&path);
        prTextBufferAppend(&path, "/attributes");
        addAttributeRows(v, json, primitive, prJsonMember(json, p, "attributes"), &path, accessors);
        PrJsonValueIndex indices = prJsonMember(json, p, "indices");
        if (indices != kPrJsonMissing)
            addPrimitiveRow(v, json, primitive, "/indices", accessors, indices);
        // A primitive without a mode draws triangles (glTF-2.0/#meshes-overview).
        PrJsonValueIndex mode = prJsonMember(json, p, "mode");
        uint32_t code = mode != kPrJsonMissing ? indexOf(json, mode) : 4;
        GltfViewPrimitiveRow modeRow{.primitive = primitive, .item = kNone};
        modeRow.member = addText(v->rowText, "/mode");
        PrTextBuffer value = {};
        prTextBufferAppend(&value, modeName(code));
        if (mode == kPrJsonMissing)
            prTextBufferAppend(&value, " (the default)");
        modeRow.value = addText(v->rowText, prTextBufferView(&value));
        prTextBufferReset(&value);
        v->primitiveRows.push(modeRow);
        PrJsonValueIndex material = prJsonMember(json, p, "material");
        if (material != kPrJsonMissing)
            addPrimitiveRow(v, json, primitive, "/material", materials, material);
        PrJsonValueIndex targets = prJsonMember(json, p, "targets");
        uint32_t target = 0;
        for (PrJsonValueIndex t = prJsonFirst(json, targets); t != kPrJsonMissing;
                t = prJsonAfter(json, targets, t), ++target) {
            prTextBufferClear(&path);
            prTextBufferAppend(&path, "/targets");
            appendIndex(&path, target);
            addAttributeRows(v, json, primitive, t, &path, accessors);
        }
        PrJsonValueIndex draco = prJsonMember(json, prJsonMember(json, p, "extensions"), "KHR_draco_mesh_compression");
        PrJsonValueIndex stream = prJsonMember(json, draco, "bufferView");
        if (stream != kPrJsonMissing) {
            addPrimitiveRow(v, json, primitive, "/extensions/KHR_draco_mesh_compression/bufferView", bufferViews, stream);
        }
    }
    prTextBufferReset(&path);

    heading(v->kit, box, "PRIMITIVES");
    const GltfViewColumn columns[] = {
        {"#", 24, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Secondary},
        {"Member", 120, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Item", 100, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Summary", 160, PrKitColumnSize_Fill, PrKitTextRole_Body, PrKitTone_Secondary},
    };
    linkTable(v, box, columns, 2, v->primitiveRows.count, &v->cursors.primitives, [&](uint32_t index, GltfViewCells& cells) {
        const GltfViewPrimitiveRow& row = v->primitiveRows.data[index];
        prTextBufferAppendU64(&cells.text[0], row.primitive, 10);
        prTextBufferAppend(&cells.text[1], prStringFrom(textOf(v->rowText, row.member), 1));
        if (row.item == kNone) {
            prTextBufferAppend(&cells.text[2], textOf(v->rowText, row.value));
            return;
        }
        appendItemPointer(v, row.item, &cells.text[2]);
        PrStringView name = nameOf(v, json, row.item);
        if (name.size != 0) {
            prTextBufferAppend(&cells.text[3], name);
            prTextBufferAppend(&cells.text[3], ": ");
        }
        appendSummary(v, json, row.item, &cells.text[3]);
        appendItemPointer(v, row.item, &cells.link);
        cells.linked = true;
    });
}

// Appends the byte range [`begin`, `end`) of a run of `size` bytes.
void appendRange(PrTextBuffer* out, uint64_t begin, uint64_t end, uint64_t size) {
    prTextBufferAppendChar(out, '[');
    prTextBufferAppendU64(out, begin, 10);
    prTextBufferAppend(out, ", ");
    prTextBufferAppendU64(out, end, 10);
    prTextBufferAppend(out, ") of ");
    prTextBufferAppendU64(out, size, 10);
}

void appendFloats(PrTextBuffer* out, const float* values, uint32_t count) {
    char digits[kPrF64TextMaxBytes];
    prTextBufferAppendChar(out, '[');
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppend(out, ", ");
        prTextBufferAppend(out, PrStringView{digits, prF32Format(values[i], digits)});
    }
    prTextBufferAppendChar(out, ']');
}

// A run of bytes that an accessor or a buffer view reads: what reads it, the item that holds it, which is kNone for
// elements that hold no bytes, and the range.
struct GltfViewBytes {
    PrStringView source;
    uint32_t item;
    uint64_t begin;
    uint64_t end;
    uint64_t size;              // of the item
    PrStringView note;          // for a run of no item
};

// A table of the runs of bytes that an item reads, each linked to the buffer view or buffer that holds it.
void bytesTable(GltfView* v, PrLayoutNode box, Span<const GltfViewBytes> runs) {
    heading(v->kit, box, "BYTES");
    const GltfViewColumn columns[] = {
        {"Source", 120, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Secondary},
        {"Item", 100, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Bytes", 160, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary},
    };
    linkTable(v, box, columns, 1, runs.count(), &v->cursors.bytes, [&](uint32_t row, GltfViewCells& cells) {
        const GltfViewBytes& run = runs[row];
        prTextBufferAppend(&cells.text[0], run.source);
        if (run.item == kNone) {
            prTextBufferAppend(&cells.text[2], run.note);
            return;
        }
        appendItemPointer(v, run.item, &cells.text[1]);
        appendRange(&cells.text[2], run.begin, run.end, run.size);
        appendItemPointer(v, run.item, &cells.link);
        cells.linked = true;
    });
}

// Adds the run of bytes [`begin`, `end`) of buffer view `view` and the run of its buffer that holds it.
void addViewRuns(GltfView* v, const PrGltf* gltf, RawArray<GltfViewBytes>* runs, PrStringView source,
        PrGltfBufferViewIndex view, uint64_t begin, uint64_t end) {
    if (view >= prGltfBufferViewCount(gltf))
        return;
    const PrGltfBufferView& record = prGltfBufferViews(gltf)[view];
    uint32_t viewItem = itemOf(v, v->targets[GltfViewTarget_BufferViews], view);
    if (viewItem != kNone)
        runs->push(GltfViewBytes{source, viewItem, begin, end, record.byteLength, ""});
    if (record.buffer >= prGltfBufferCount(gltf))
        return;
    uint32_t bufferItem = itemOf(v, v->targets[GltfViewTarget_Buffers], record.buffer);
    uint64_t bufferSize = prGltfBuffers(gltf)[record.buffer].byteLength;
    if (bufferItem != kNone) {
        runs->push(GltfViewBytes{
            "buffer", bufferItem, record.byteOffset + begin, record.byteOffset + end, bufferSize,
            "",
        });
    }
}

// An accessor: what its elements are, the bytes that hold them, and their values.
void accessorSections(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, PrGltfAccessorIndex index) {
    const PrGltf* gltf = content.gltf;
    if (gltf == nullptr || index >= prGltfAccessorCount(gltf))
        return;
    const PrGltfAccessor& a = prGltfAccessors(gltf)[index];
    uint32_t components = prGltfElementTypeComponentCount(a.type);
    uint32_t elementSize = prGltfElementSize(a.type, a.componentType);

    heading(v->kit, box, "ACCESSOR");
    const GltfViewColumn factColumns[] = {
        {"Fact", 100, PrKitColumnSize_Fit, PrKitTextRole_Body, PrKitTone_Secondary},
        {"Value", 160, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary},
    };
    float min[16] = {};
    float max[16] = {};
    bool bounded = prGltfAccessorBounds(gltf, index, min, max);
    const PrStringView kFacts[] = {"element", "count", "normalized", "min", "max", "sparse"};
    linkTable(v, box, factColumns, kNone, 6, &v->cursors.facts, [&](uint32_t row, GltfViewCells& cells) {
        PrTextBuffer* value = &cells.text[1];
        prTextBufferAppend(&cells.text[0], kFacts[row]);
        switch (row) {
            case 0:
                prTextBufferAppend(value, elementTypeName(a.type));
                prTextBufferAppend(value, " of ");
                prTextBufferAppend(value, componentTypeName(componentTypeCode(a.componentType)));
                prTextBufferAppend(value, " (");
                prTextBufferAppendU64(value, componentTypeCode(a.componentType), 10);
                prTextBufferAppend(value, "), ");
                appendCount(value, elementSize, "byte", "bytes");
                return;
            case 1:
                prTextBufferAppendU64(value, a.count, 10);
                return;
            case 2:
                prTextBufferAppend(value, a.normalized ? "true" : "false");
                return;
            case 3:
            case 4:
                if (bounded)
                    appendFloats(value, row == 3 ? min : max, components);
                else
                    prTextBufferAppend(value, "not stated");
                return;
            default:
                if (a.sparse.count == 0) {
                    prTextBufferAppend(value, "none");
                    return;
                }
                appendCount(value, a.sparse.count, "element replaces", "elements replace");
                prTextBufferAppend(value, " the source's, by indices of ");
                prTextBufferAppend(value, componentTypeName(componentTypeCode(a.sparse.indicesComponentType)));
                return;
        }
    });

    RawArray<GltfViewBytes> runs;
    uint32_t stride = prGltfAccessorStride(gltf, index);
    if (a.draco) {
        runs.push(GltfViewBytes{"KHR_draco_mesh_compression", kNone, 0, 0, 0, "decoded from a primitive's stream"});
    } else if (a.bufferView != kPrGltfNone) {
        uint64_t end = a.byteOffset + uint64_t(stride) * (a.count - 1) + elementSize;
        addViewRuns(v, gltf, &runs, "bufferView", a.bufferView, a.byteOffset, end);
    } else {
        runs.push(GltfViewBytes{"bufferView", kNone, 0, 0, 0, "none: the elements are zeros"});
    }
    if (a.sparse.count != 0) {
        uint64_t indexBytes = uint64_t(a.sparse.count) * prGltfComponentTypeSize(a.sparse.indicesComponentType);
        uint64_t valueBytes = uint64_t(a.sparse.count) * prGltfElementSize(a.type, a.componentType);
        addViewRuns(v, gltf, &runs, "sparse/indices", a.sparse.indicesBufferView, a.sparse.indicesByteOffset,
            a.sparse.indicesByteOffset + indexBytes);
        addViewRuns(v, gltf, &runs, "sparse/values", a.sparse.valuesBufferView, a.sparse.valuesByteOffset,
            a.sparse.valuesByteOffset + valueBytes);
    }
    bytesTable(v, box, runs.span());
    runs.reset();

    // The values, one column for each component of a vector, or for each column of a matrix.
    bool matrix = a.type >= PrGltfElementType_Mat2;
    // A matrix of n columns follows MAT2 by n - 2.
    uint32_t columnCount = components;
    if (matrix)
        columnCount = uint32_t(a.type - PrGltfElementType_Mat2) + 2;
    static const PrStringView kVectorLabels[] = {"x", "y", "z", "w"};
    static const PrStringView kMatrixLabels[] = {"column 0", "column 1", "column 2", "column 3"};
    GltfViewColumn columns[kMaxColumns] = {{"#", 40, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Secondary}};
    for (uint32_t c = 0; c < columnCount; ++c) {
        PrStringView label = matrix ? kMatrixLabels[c] : components == 1 ? PrStringView("value") : kVectorLabels[c];
        columns[c + 1] = GltfViewColumn{label, 60, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary};
    }
    bool floats = a.componentType == PrGltfComponentType_Float;
    bool signedType = a.componentType == PrGltfComponentType_Byte || a.componentType == PrGltfComponentType_Short;
    heading(v->kit, box, "VALUES");
    Span<const GltfViewColumn> shown(columns, columnCount + 1);
    linkTable(v, box, shown, kNone, a.count, &v->cursors.values, [&](uint32_t row, GltfViewCells& cells) {
        float values[16] = {};
        uint32_t integers[16] = {};
        bool read = prGltfAccessorReadFloats(gltf, index, row, 1, values);
        if (!floats)
            read = prGltfAccessorReadIntegers(gltf, index, row, 1, integers) && read;
        prTextBufferAppendU64(&cells.text[0], row, 10);
        uint32_t perColumn = components / columnCount;
        char digits[kPrF64TextMaxBytes];
        for (uint32_t c = 0; c < columnCount; ++c) {
            PrTextBuffer* cell = &cells.text[c + 1];
            if (!read) {
                prTextBufferAppend(cell, "no bytes");
                continue;
            }
            for (uint32_t k = c * perColumn; k < (c + 1) * perColumn; ++k) {
                if (k != c * perColumn)
                    prTextBufferAppend(cell, ", ");
                if (!floats) {
                        // An integer shows as it is stored, and a normalized one with the number that it stands for.
                    int64_t stored = signedType ? int64_t(int32_t(integers[k])) : int64_t(integers[k]);
                    if (stored < 0)
                        prTextBufferAppendChar(cell, '-');
                    prTextBufferAppendU64(cell, uint64_t(stored < 0 ? -stored : stored), 10);
                    if (!a.normalized)
                        continue;
                    prTextBufferAppend(cell, " (");
                    prTextBufferAppend(cell, PrStringView{digits, prF32Format(values[k], digits)});
                    prTextBufferAppendChar(cell, ')');
                    continue;
                }
                prTextBufferAppend(cell, PrStringView{digits, prF32Format(values[k], digits)});
            }
        }
    });
}

// A buffer view: the bytes of its buffer that it holds, and those of a compressed form.
void bufferViewSections(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, PrGltfBufferViewIndex index) {
    const PrGltf* gltf = content.gltf;
    if (gltf == nullptr || index >= prGltfBufferViewCount(gltf))
        return;
    const PrGltfBufferView& view = prGltfBufferViews(gltf)[index];
    RawArray<GltfViewBytes> runs;
    if (view.buffer < prGltfBufferCount(gltf)) {
        uint32_t buffer = itemOf(v, v->targets[GltfViewTarget_Buffers], view.buffer);
        uint64_t size = prGltfBuffers(gltf)[view.buffer].byteLength;
        if (buffer != kNone)
            runs.push(GltfViewBytes{"buffer", buffer, view.byteOffset, view.byteOffset + view.byteLength, size, ""});
    }
    if (view.meshopt.buffer != kPrGltfNone && view.meshopt.buffer < prGltfBufferCount(gltf)) {
        uint32_t buffer = itemOf(v, v->targets[GltfViewTarget_Buffers], view.meshopt.buffer);
        uint64_t size = prGltfBuffers(gltf)[view.meshopt.buffer].byteLength;
        uint64_t end = view.meshopt.byteOffset + view.meshopt.byteLength;
        if (buffer != kNone)
            runs.push(GltfViewBytes{"meshopt", buffer, view.meshopt.byteOffset, end, size, ""});
    }
    if (runs.count != 0)
        bytesTable(v, box, runs.span());
    runs.reset();
}

// An item: its properties, its extensions, what its collection shows of it, what it names and what names it.
void itemPage(GltfView* v, PrLayoutNode box, const PrGltfViewContent& content, GltfViewPlace place) {
    PrKit* kit = v->kit;
    const PrJson* json = content.json;
    PrJsonValueIndex item = v->items.data[place.item];
    const GltfViewCollection& collection = v->collections.data[place.collection];
    uint32_t index = place.item - collection.firstItem;
    PrTextBuffer pointer = {};
    appendItemPointer(v, place.item, &pointer);
    PrTextBuffer title = {};
    prTextBufferAppend(&title, prTextBufferView(&pointer));
    PrStringView name = nameOf(v, json, place.item);
    if (name.size != 0) {
        prTextBufferAppend(&title, "  ");
        prTextBufferAppend(&title, name);
    }
    prKitLabel(kit, box, {.text = prTextBufferView(&title)}, {.role = PrKitTextRole_Heading});
    prTextBufferReset(&title);

    v->rowText.clear();
    v->properties.clear();
    PrTextBuffer path = {};
    addProperties(v, json, item, &path, 0);
    prTextBufferReset(&path);
    heading(kit, box, "PROPERTIES");
    const GltfViewColumn columns[] = {
        {"Member", 140, PrKitColumnSize_Fit, PrKitTextRole_Mono, PrKitTone_Primary},
        {"Value", 160, PrKitColumnSize_Fill, PrKitTextRole_Mono, PrKitTone_Primary},
    };
    linkTable(v, box, columns, kNone, v->properties.count, &v->cursors.properties, [&](uint32_t row, GltfViewCells& cells) {
        prTextBufferAppend(&cells.text[0], textOf(v->rowText, v->properties.data[row].path));
        prTextBufferAppend(&cells.text[1], textOf(v->rowText, v->properties.data[row].value));
    });

    PrJsonValueIndex extensions = prJsonMember(json, item, "extensions");
    v->extensionNames.clear();
    for (PrJsonValueIndex key = prJsonFirst(json, extensions); key != kPrJsonMissing;
            key = prJsonAfter(json, extensions, key))
        v->extensionNames.push(prJsonString(json, key));
    if (v->extensionNames.count != 0) {
        heading(kit, box, "EXTENSIONS");
        extensionsTable(v, box, content, kPrJsonMissing);
    }

    PrStringView collectionPointer = textOf(v->linkText, collection.pointer);
    if (collectionPointer == "/meshes")
        primitivesSection(v, box, content, item);
    else if (collectionPointer == "/accessors")
        accessorSections(v, box, content, index);
    else if (collectionPointer == "/bufferViews")
        bufferViewSections(v, box, content, index);
    referenceSections(v, box, content, prTextBufferView(&pointer));
    prTextBufferReset(&pointer);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrGltfView : GltfView {};

PrGltfView* prGltfViewCreate(PrKit* kit) {
    PrGltfView* view = allocZeroed<PrGltfView>();
    view->kit = kit;
    PrKitSelection none{kPrKitRowNone, kPrKitRowNone};
    view->cursors = GltfViewCursors{none, none, none, none, none, none, none, none, none, none};
    view->shown = GltfViewPlace{kNone, kNone};
    view->followed = prRawStringNull();
    return view;
}

void prGltfViewDestroy(PrGltfView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    view->linkText.reset();
    view->collections.reset();
    view->items.reset();
    view->links.reset();
    view->bySource.reset();
    view->byTarget.reset();
    view->rowText.reset();
    view->properties.reset();
    view->primitiveRows.reset();
    view->extensionNames.reset();
    for (PrTextBuffer& text : view->cells.text)
        prTextBufferReset(&text);
    prTextBufferReset(&view->cells.link);
    prRawStringReset(&view->followed);
    prMemoryFree(view);
}

void prGltfViewFrame(PrGltfView* view, PrLayoutNode parent, PrGltfViewContent content) {
    GltfView* v = view;
    if (v->indexed != content.json) {
        indexAsset(v, content.json);
        v->shown = GltfViewPlace{kNone, kNone};
    }
    GltfViewPlace place = placeOf(v, content.selection);
    // The cursors are rows of the tables of what the view shows, so another item starts them afresh.
    if (place.collection != v->shown.collection || place.item != v->shown.item) {
        PrKitSelection none{kPrKitRowNone, kPrKitRowNone};
        v->cursors = GltfViewCursors{none, none, none, none, none, none, none, none, none, none};
        v->shown = place;
    }
    PrKitBoxStyle column{
        .overflow = PrKitOverflow_ScrollY,
        .padding = prKitEdgesAll(8),
        .place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode box = prKitBox(v->kit, parent, {}, column);
    if (place.item != kNone)
        itemPage(v, box, content, place);
    else if (place.collection != kNone)
        collectionPage(v, box, content, place.collection);
    else
        assetPage(v, box, content);
}

bool prGltfViewFollowed(PrGltfView* view, PrRawString* out) {
    if (prRawStringIsNull(&view->followed))
        return false;
    *out = view->followed;
    view->followed = prRawStringNull();
    return true;
}

PrGltfViewExtension prGltfViewExtensionState(PrStringView name, PrGltfMaterialExtensionFlags unshaded) {
    if (!prGltfExtensionIsSupported(name))
        return PrGltfViewExtension_Ignored;
    if ((prGltfMaterialExtensionFromName(name) & unshaded) != 0)
        return PrGltfViewExtension_Read;
    return PrGltfViewExtension_Drawn;
}

uint32_t prGltfViewReferenceCount(const PrGltfView* view) {
    return view->links.count;
}

PrGltfViewReference prGltfViewReference(const PrGltfView* view, uint32_t index) {
    PR_ASSERT(index < view->links.count);
    const GltfViewLink& link = view->links.data[index];
    return PrGltfViewReference{
        .from = textOf(view->linkText, link.from),
        .to = textOf(view->linkText, link.to),
        .path = textOf(view->linkText, link.path),
    };
}
