// The layer that the readers of each format build, and the functions that read it.

#include "usd/src/usd_layer.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// What the module knows of a type of value.
struct TypeInfo {
    PrStringView name;
    uint8_t size;
    uint8_t alignment;
};

// The types, by PrUsdType.
constexpr TypeInfo kTypes[PrUsdType_Count] = {
    {"none", 0, 1},
    {"bool", sizeof(bool), alignof(bool)},
    {"uchar", 1, 1},
    {"int", 4, 4},
    {"uint", 4, 4},
    {"int64", 8, 8},
    {"uint64", 8, 8},
    {"half", sizeof(PrFloat16), alignof(PrFloat16)},
    {"float", 4, 4},
    {"double", 8, 8},
    {"timecode", 8, 8},
    {"string", sizeof(PrStringView), alignof(PrStringView)},
    {"token", sizeof(PrStringView), alignof(PrStringView)},
    {"asset", sizeof(PrStringView), alignof(PrStringView)},
    {"path", sizeof(PrStringView), alignof(PrStringView)},
    {"pathExpression", sizeof(PrStringView), alignof(PrStringView)},
    {"int2", sizeof(PrIVec2), alignof(PrIVec2)},
    {"int3", sizeof(PrIVec3), alignof(PrIVec3)},
    {"int4", sizeof(PrIVec4), alignof(PrIVec4)},
    {"half2", 2 * sizeof(PrFloat16), alignof(PrFloat16)},
    {"half3", 3 * sizeof(PrFloat16), alignof(PrFloat16)},
    {"half4", 4 * sizeof(PrFloat16), alignof(PrFloat16)},
    {"float2", sizeof(PrVec2), alignof(PrVec2)},
    {"float3", sizeof(PrVec3), alignof(PrVec3)},
    {"float4", sizeof(PrVec4), alignof(PrVec4)},
    {"double2", sizeof(PrDVec2), alignof(PrDVec2)},
    {"double3", sizeof(PrDVec3), alignof(PrDVec3)},
    {"double4", sizeof(PrDVec4), alignof(PrDVec4)},
    {"quath", 4 * sizeof(PrFloat16), alignof(PrFloat16)},
    {"quatf", sizeof(PrQuat), alignof(PrQuat)},
    {"quatd", sizeof(PrDQuat), alignof(PrDQuat)},
    {"matrix2d", sizeof(PrDMatrix2), alignof(PrDMatrix2)},
    {"matrix3d", sizeof(PrDMatrix3), alignof(PrDMatrix3)},
    {"matrix4d", sizeof(PrDMatrix4), alignof(PrDMatrix4)},
    {"dictionary", sizeof(PrUsdDictionary), alignof(PrUsdDictionary)},
    {"listOp", sizeof(PrUsdListOp), alignof(PrUsdListOp)},
    {"timeSamples", sizeof(PrUsdTimeSamples), alignof(PrUsdTimeSamples)},
    {"specifier", 1, 1},
    {"variability", 1, 1},
    {"permission", 1, 1},
    {"layerOffset", sizeof(PrUsdLayerOffset), alignof(PrUsdLayerOffset)},
    {"reference", sizeof(PrUsdReference), alignof(PrUsdReference)},
    {"payload", sizeof(PrUsdReference), alignof(PrUsdReference)},
    {"relocate", sizeof(PrUsdRelocate), alignof(PrUsdRelocate)},
    {"variantSelection", sizeof(PrUsdVariantSelection), alignof(PrUsdVariantSelection)},
    {"unregistered", sizeof(PrStringView), alignof(PrStringView)},
};

// A semantic alias and the type that underlies it (usd-core/#semantic-aliases-for-types).
struct TypeAlias {
    PrStringView name;
    PrUsdType type;
};

constexpr TypeAlias kAliases[] = {
    {"color3d", PrUsdType_Double3},    {"color3f", PrUsdType_Float3},     {"color3h", PrUsdType_Half3},
    {"color4d", PrUsdType_Double4},    {"color4f", PrUsdType_Float4},     {"color4h", PrUsdType_Half4},
    {"normal3d", PrUsdType_Double3},   {"normal3f", PrUsdType_Float3},    {"normal3h", PrUsdType_Half3},
    {"point3d", PrUsdType_Double3},    {"point3f", PrUsdType_Float3},     {"point3h", PrUsdType_Half3},
    {"vector3d", PrUsdType_Double3},   {"vector3f", PrUsdType_Float3},    {"vector3h", PrUsdType_Half3},
    {"frame4d", PrUsdType_Matrix4d},   {"texCoord2d", PrUsdType_Double2}, {"texCoord2f", PrUsdType_Float2},
    {"texCoord2h", PrUsdType_Half2},   {"texCoord3d", PrUsdType_Double3}, {"texCoord3f", PrUsdType_Float3},
    {"texCoord3h", PrUsdType_Half3},
};

// A field that the module knows the type of, by name.
struct KnownField {
    PrStringView name;
    UsdFieldType type;
};

constexpr UsdFieldType kBool = {PrUsdType_Bool, false, false};
constexpr UsdFieldType kInt = {PrUsdType_Int, false, false};
constexpr UsdFieldType kInt64 = {PrUsdType_Int64, false, false};
constexpr UsdFieldType kDouble = {PrUsdType_Double, false, false};
constexpr UsdFieldType kString = {PrUsdType_String, false, false};
constexpr UsdFieldType kToken = {PrUsdType_Token, false, false};
constexpr UsdFieldType kAsset = {PrUsdType_Asset, false, false};
constexpr UsdFieldType kDictionary = {PrUsdType_Dictionary, false, false};
constexpr UsdFieldType kStrings = {PrUsdType_String, true, false};
constexpr UsdFieldType kTokens = {PrUsdType_Token, true, false};
constexpr UsdFieldType kTokenListOp = {PrUsdType_Token, false, true};
constexpr UsdFieldType kStringListOp = {PrUsdType_String, false, true};
constexpr UsdFieldType kInt64ListOp = {PrUsdType_Int64, false, true};
constexpr UsdFieldType kPathListOp = {PrUsdType_Path, false, true};

// The fields that a text layer writes as `name = value`, by name. They are the fields of the specification's data model
// (usd-core/#core-metadata-fields), the fields that it reserves, and the fields that the schemas of OpenUSD add to the
// registry, which the specification leaves to schemas (usd-core/#extension-metadata-fields). Sorted by name.
constexpr KnownField kFields[] = {
    {"active", kBool},
    {"allowedTokens", kTokens},
    {"apiSchemas", kTokenListOp},
    {"arraySizeConstraint", kInt64},
    {"assetInfo", kDictionary},
    {"bindMaterialAs", kToken},
    {"clipSets", kStringListOp},
    {"clips", kDictionary},
    {"colorConfiguration", kAsset},
    {"colorManagementSystem", kToken},
    {"colorSpace", kToken},
    {"comment", kString},
    {"connectability", kToken},
    {"connectionPaths", kPathListOp},
    {"constraintTargetIdentifier", kToken},
    {"customData", kDictionary},
    {"customLayerData", kDictionary},
    {"defaultPrim", kToken},
    {"displayGroup", kString},
    {"displayGroupOrder", kStrings},
    {"displayName", kString},
    {"documentation", kString},
    {"elementSize", kInt},
    {"endFrame", kDouble},
    {"endTimeCode", kDouble},
    {"expressionVariables", kDictionary},
    {"fallbackPrimTypes", kDictionary},
    {"framePrecision", kInt},
    {"framesPerSecond", kDouble},
    {"hasOwnedSubLayers", kBool},
    {"hidden", kBool},
    {"inactiveIds", kInt64ListOp},
    {"inheritPaths", kPathListOp},
    {"instanceable", kBool},
    {"interpolation", kToken},
    {"kilogramsPerUnit", kDouble},
    {"kind", kToken},
    {"limits", kDictionary},
    {"metersPerUnit", kDouble},
    {"noLoadHint", kBool},
    {"outputName", kToken},
    {"owner", kString},
    {"prefix", kString},
    {"prefixSubstitutions", kDictionary},
    {"primOrder", kTokens},
    {"propertyOrder", kTokens},
    {"renderSettingsPrimPath", kString},
    {"renderType", kToken},
    {"sdrMetadata", kDictionary},
    {"sessionOwner", kString},
    {"specializes", kPathListOp},
    {"startFrame", kDouble},
    {"startTimeCode", kDouble},
    {"suffix", kString},
    {"suffixSubstitutions", kDictionary},
    {"symmetricPeer", kString},
    {"symmetryArguments", kDictionary},
    {"symmetryFunction", kToken},
    {"targetPaths", kPathListOp},
    {"timeCodesPerSecond", kDouble},
    {"typeName", kToken},
    {"uiHints", kDictionary},
    {"unauthoredValuesIndex", kInt},
    {"upAxis", kToken},
    {"variantSetNames", kStringListOp},
};

bool isAsciiStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || uint8_t(c) >= 0x80;
}

bool isAsciiContinue(char c) {
    return isAsciiStart(c) || (c >= '0' && c <= '9');
}

// Reads an identifier at `at`, and returns its size, or zero if there is none.
size_t identifierSize(PrStringView text, size_t at) {
    if (at >= text.size || !isAsciiStart(text.data[at]))
        return 0;
    size_t end = at + 1;
    while (end < text.size && isAsciiContinue(text.data[end]))
        ++end;
    return end - at;
}

// Reads a property's name with its namespaces at `at`, and returns its size, or zero if there is none.
size_t propertyNameSize(PrStringView text, size_t at) {
    size_t end = at;
    for (;;) {
        size_t size = identifierSize(text, end);
        if (size == 0)
            return 0;
        end += size;
        if (end >= text.size || text.data[end] != ':' || identifierSize(text, end + 1) == 0)
            return end - at;
        ++end;
    }
}

bool isVariantCharacter(char c) {
    return isAsciiContinue(c) || c == '|' || c == '-';
}

bool pathAt(PrStringView text, size_t& at, UsdPathKind kind, uint32_t depth);

// Reads the property part of a path at `at`: a name, and for legacy content a target in brackets and what follows it.
bool propertyPartAt(PrStringView text, size_t& at, uint32_t depth) {
    while (at < text.size && text.data[at] == '.') {
        size_t size = propertyNameSize(text, at + 1);
        if (size == 0)
            return false;
        at += 1 + size;
        if (at < text.size && text.data[at] == '[') {
            ++at;
            if (depth > 8 || !pathAt(text, at, UsdPathKind_Any, depth + 1) || at >= text.size || text.data[at] != ']')
                return false;
            ++at;
        }
    }
    return true;
}

// Reads the prim part of a path at `at`: names divided by slashes, and variant selections, which a name may follow.
bool primPartAt(PrStringView text, size_t& at, UsdPathKind kind) {
    for (;;) {
        size_t size = identifierSize(text, at);
        if (size == 0)
            return false;
        at += size;
        bool selected = false;
        while (at < text.size && text.data[at] == '{') {
            if (kind == UsdPathKind_Prim)
                return false;
            ++at;
            while (at < text.size && text.data[at] == ' ')
                ++at;
            size = identifierSize(text, at);
            // Legacy content has variant sets with hyphens in their names.
            while (size != 0 && at + size < text.size && (text.data[at + size] == '-' || isAsciiContinue(text.data[at + size])))
                ++size;
            if (size == 0)
                return false;
            at += size;
            while (at < text.size && text.data[at] == ' ')
                ++at;
            if (at >= text.size || text.data[at] != '=')
                return false;
            ++at;
            while (at < text.size && text.data[at] == ' ')
                ++at;
            if (at < text.size && text.data[at] == '.')
                ++at;
            while (at < text.size && isVariantCharacter(text.data[at]))
                ++at;
            while (at < text.size && text.data[at] == ' ')
                ++at;
            if (at >= text.size || text.data[at] != '}')
                return false;
            ++at;
            selected = true;
        }
        if (selected) {
            // A prim of the variant follows its selection directly.
            if (identifierSize(text, at) != 0)
                continue;
            return true;
        }
        if (at < text.size && text.data[at] == '/' && identifierSize(text, at + 1) != 0) {
            ++at;
            continue;
        }
        return true;
    }
}

bool pathAt(PrStringView text, size_t& at, UsdPathKind kind, uint32_t depth) {
    if (at < text.size && text.data[at] == '/') {
        ++at;
        if (identifierSize(text, at) == 0)
            return kind == UsdPathKind_Any;
        if (!primPartAt(text, at, kind))
            return false;
        return kind == UsdPathKind_Prim || propertyPartAt(text, at, depth);
    }
    if (kind == UsdPathKind_Prim)
        return false;
    bool parents = false;
    while (at + 1 < text.size && text.data[at] == '.' && text.data[at + 1] == '.') {
        at += 2;
        parents = true;
        if (at < text.size && text.data[at] == '/' && at + 1 < text.size && text.data[at + 1] != ']')
            ++at;
        else
            return true;
    }
    if (identifierSize(text, at) != 0) {
        if (!primPartAt(text, at, kind))
            return false;
        return propertyPartAt(text, at, depth);
    }
    if (at < text.size && text.data[at] == '.') {
        // The reflexive path, or a property of the anchor.
        if (propertyNameSize(text, at + 1) == 0) {
            if (parents)
                return false;
            ++at;
            return true;
        }
        return propertyPartAt(text, at, depth);
    }
    return false;
}

// Appends `text` to a path being made.
void append(RawArray<char>& out, PrStringView text) {
    out.append(text.data, uint32_t(text.size));
}

PrUsdValue elementValue(PrUsdType type, const void* data) {
    return PrUsdValue{type, false, 1, data};
}

// Returns the positions of `count` records in the order of `compare`, and among equal records in order of position.
template <typename Compare>
RawArray<uint32_t> orderOf(uint32_t count, Compare compare) {
    RawArray<uint32_t> order{};
    order.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        order.data[i] = i;
    sortItems(Span<uint32_t>(order.data, count), [&](uint32_t a, uint32_t b) {
        int compared = compare(a, b);
        return compared != 0 ? compared < 0 : a < b;
    });
    return order;
}

} // namespace

PrStringView usdLayerIntern(UsdLayer* layer, PrStringView text) {
    Atom atom = atomTableAdd(&layer->strings, text);
    return atomTableText(&layer->strings, atom);
}

void* usdLayerAllocElements(UsdLayer* layer, PrUsdType type, uint32_t count) {
    const TypeInfo& info = kTypes[type];
    return layer->arena.allocBytesZeroed(size_t(info.size) * count, info.alignment);
}

PrUsdSpecIndex usdLayerAddSpec(UsdLayer* layer, PrUsdSpecForm form, PrUsdSpecIndex parent, PrStringView name, bool* existed) {
    *existed = false;
    RawArray<char> path{};
    if (form == PrUsdSpecForm_Layer) {
        append(path, "/");
    } else {
        const PrUsdSpec& above = layer->specs.data[parent];
        switch (form) {
            case PrUsdSpecForm_Prim:
                if (above.form == PrUsdSpecForm_Layer) {
                    append(path, "/");
                } else {
                    append(path, above.path);
                    if (above.form == PrUsdSpecForm_Prim)
                        append(path, "/");
                }
                append(path, name);
                break;
            case PrUsdSpecForm_Attribute:
            case PrUsdSpecForm_Relationship:
                append(path, above.path);
                append(path, ".");
                append(path, name);
                break;
            case PrUsdSpecForm_VariantSet:
                append(path, above.path);
                append(path, "{");
                append(path, name);
                append(path, "=}");
                break;
            default:
                // A variant set's path ends with "=}", which the variant's name goes inside.
                append(path, PrStringView{above.path.data, above.path.size - 1});
                append(path, name);
                append(path, "}");
                break;
        }
    }
    Atom atom = atomTableAdd(&layer->strings, PrStringView{path.data, path.count});
    path.reset();
    while (layer->specOfAtom.count <= atom)
        layer->specOfAtom.push(kPrUsdNone);
    if (layer->specOfAtom.data[atom] != kPrUsdNone) {
        *existed = true;
        return layer->specOfAtom.data[atom];
    }
    PrUsdSpecIndex index = layer->specs.count;
    layer->specOfAtom.data[atom] = index;
    PrUsdSpec spec{};
    spec.form = form;
    spec.parent = form == PrUsdSpecForm_Layer ? kPrUsdNone : parent;
    spec.name = usdLayerIntern(layer, name);
    spec.path = atomTableText(&layer->strings, atom);
    layer->specs.push(spec);
    layer->fieldHead.push(kPrUsdNone);
    return index;
}

PrUsdField* PR_NULLABLE usdLayerBuildField(UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name) {
    for (uint32_t at = layer->fieldHead.data[spec]; at != kPrUsdNone; at = layer->buildFields.data[at].next) {
        if (layer->buildFields.data[at].field.name == name)
            return &layer->buildFields.data[at].field;
    }
    return nullptr;
}

void usdLayerSetField(UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name, PrUsdValue value) {
    if (PrUsdField* field = usdLayerBuildField(layer, spec, name)) {
        field->value = value;
        return;
    }
    UsdBuildField added{};
    added.field.name = usdLayerIntern(layer, name);
    added.field.value = value;
    added.next = layer->fieldHead.data[spec];
    layer->fieldHead.data[spec] = layer->buildFields.count;
    layer->buildFields.push(added);
}

PrUsdValue usdLayerValue(UsdLayer* layer, PrUsdType type, bool isArray, uint32_t count, const void* PR_NULLABLE data) {
    const TypeInfo& info = kTypes[type];
    size_t size = size_t(info.size) * count;
    void* copy = layer->arena.allocBytes(size, info.alignment);
    if (size != 0)
        prMemCopy(copy, data, size);
    return PrUsdValue{type, isArray, count, copy};
}

uint32_t usdListRemoveRepeats(PrUsdType type, void* PR_NULLABLE data, uint32_t count) {
    size_t size = kTypes[type].size;
    if (count < 2 || size == 0)
        return count;
    uint8_t* bytes = static_cast<uint8_t*>(data);
    // Each run of equal elements begins with the one to keep.
    RawArray<uint32_t> order = orderOf(count, [&](uint32_t a, uint32_t b) {
        return prMemCompare(bytes + a * size, bytes + b * size, size);
    });
    RawArray<bool> repeats{};
    repeats.resizeZeroed(count);
    bool any = false;
    for (uint32_t i = 1; i < count; ++i) {
        if (prMemCompare(bytes + order.data[i] * size, bytes + order.data[i - 1] * size, size) == 0) {
            repeats.data[order.data[i]] = true;
            any = true;
        }
    }
    uint32_t kept = count;
    if (any) {
        kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (repeats.data[i])
                continue;
            if (kept != i)
                prMemMove(bytes + kept * size, bytes + i * size, size);
            ++kept;
        }
    }
    order.reset();
    repeats.reset();
    return kept;
}

PrUsdValue usdLayerDictionary(UsdLayer* layer, RawArray<PrUsdDictionaryEntry>& entries) {
    RawArray<uint32_t> order = orderOf(entries.count, [&](uint32_t a, uint32_t b) {
        return prStringCompare(entries.data[a].key, entries.data[b].key);
    });
    PrUsdDictionaryEntry* sorted = layer->arena.allocZeroed<PrUsdDictionaryEntry>(entries.count);
    uint32_t count = 0;
    for (uint32_t i = 0; i < entries.count; ++i) {
        const PrUsdDictionaryEntry& entry = entries.data[order.data[i]];
        // The last of the entries with one key is the one that the file leaves in force.
        if (count != 0 && sorted[count - 1].key == entry.key)
            sorted[count - 1] = entry;
        else
            sorted[count++] = entry;
    }
    order.reset();
    PrUsdDictionary* dictionary = layer->arena.allocZeroed<PrUsdDictionary>();
    dictionary->count = count;
    dictionary->entries = sorted;
    return PrUsdValue{PrUsdType_Dictionary, false, 1, dictionary};
}

PrUsdValue usdLayerTimeSamples(UsdLayer* layer, RawArray<double>& times, RawArray<PrUsdValue>& values) {
    RawArray<uint32_t> order = orderOf(times.count, [&](uint32_t a, uint32_t b) {
        return times.data[a] < times.data[b] ? -1 : times.data[a] > times.data[b] ? 1 : 0;
    });
    double* sortedTimes = layer->arena.allocZeroed<double>(times.count);
    PrUsdValue* sortedValues = layer->arena.allocZeroed<PrUsdValue>(times.count);
    uint32_t count = 0;
    for (uint32_t i = 0; i < times.count; ++i) {
        uint32_t from = order.data[i];
        if (count != 0 && sortedTimes[count - 1] == times.data[from]) {
            sortedValues[count - 1] = values.data[from];
        } else {
            sortedTimes[count] = times.data[from];
            sortedValues[count++] = values.data[from];
        }
    }
    order.reset();
    PrUsdTimeSamples* samples = layer->arena.allocZeroed<PrUsdTimeSamples>();
    samples->count = count;
    samples->times = sortedTimes;
    samples->values = sortedValues;
    return PrUsdValue{PrUsdType_TimeSamples, false, 1, samples};
}

PrUsdValue usdLayerVariantSelections(UsdLayer* layer, RawArray<PrUsdVariantSelection>& selections) {
    RawArray<uint32_t> order = orderOf(selections.count, [&](uint32_t a, uint32_t b) {
        return prStringCompare(selections.data[a].variantSet, selections.data[b].variantSet);
    });
    PrUsdVariantSelection* sorted = layer->arena.allocZeroed<PrUsdVariantSelection>(selections.count);
    uint32_t count = 0;
    for (uint32_t i = 0; i < selections.count; ++i) {
        const PrUsdVariantSelection& selection = selections.data[order.data[i]];
        if (count != 0 && sorted[count - 1].variantSet == selection.variantSet)
            sorted[count - 1] = selection;
        else
            sorted[count++] = selection;
    }
    order.reset();
    return PrUsdValue{PrUsdType_VariantSelection, true, count, sorted};
}

const PrUsdField* PR_NULLABLE usdLayerFindField(const UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name) {
    const PrUsdSpec& s = layer->specs.data[spec];
    uint32_t low = 0;
    uint32_t high = s.fieldCount;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        int order = prStringCompare(s.fields[middle].name, name);
        if (order == 0)
            return &s.fields[middle];
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

bool usdLayerFinish(UsdLayer* layer, PrTextBuffer* PR_NULLABLE diagnostics) {
    // The fields that the specification requires of each form, which a format may leave to their fallbacks.
    static const PrUsdSpecifier kOver = PrUsdSpecifier_Over;
    static const PrUsdVariability kVarying = PrUsdVariability_Varying;
    static const bool kNotCustom = false;
    static const PrStringView kNoType = "";
    for (uint32_t s = 0; s < layer->specs.count; ++s) {
        PrUsdSpecForm form = layer->specs.data[s].form;
        if (form == PrUsdSpecForm_Prim || form == PrUsdSpecForm_Variant) {
            if (usdLayerBuildField(layer, s, "specifier") == nullptr)
                usdLayerSetField(layer, s, "specifier", elementValue(PrUsdType_Specifier, &kOver));
        }
        if (form == PrUsdSpecForm_Attribute || form == PrUsdSpecForm_Relationship) {
            if (usdLayerBuildField(layer, s, "custom") == nullptr)
                usdLayerSetField(layer, s, "custom", elementValue(PrUsdType_Bool, &kNotCustom));
        }
        if (form == PrUsdSpecForm_Attribute) {
            if (usdLayerBuildField(layer, s, "typeName") == nullptr)
                usdLayerSetField(layer, s, "typeName", elementValue(PrUsdType_Token, &kNoType));
            if (usdLayerBuildField(layer, s, "variability") == nullptr)
                usdLayerSetField(layer, s, "variability", elementValue(PrUsdType_Variability, &kVarying));
        }
    }

    // The fields that the specification requires have the types that it gives them, and an attribute's values have the
    // type that it declares, so that a consumer reads each as that type.
    for (uint32_t s = 0; s < layer->specs.count; ++s) {
        PrUsdSpecForm form = layer->specs.data[s].form;
        struct Required {
            PrStringView name;
            PrUsdType type;
        };
        static const Required kRequired[] = {
            {"specifier", PrUsdType_Specifier}, {"custom", PrUsdType_Bool},
            {"typeName", PrUsdType_Token}, {"variability", PrUsdType_Variability},
        };
        for (const Required& required : kRequired) {
            const PrUsdField* field = usdLayerBuildField(layer, s, required.name);
            // A prim's type is a token too, and the layer spec has none of these fields.
            if (field != nullptr && (field->value.type != required.type || field->value.isArray)) {
                usdDiagnose(diagnostics, "a field that the specification requires has another type than it gives");
                return false;
            }
        }
        if (form != PrUsdSpecForm_Attribute)
            continue;
        PrUsdType type = PrUsdType_Block;
        bool isArray = false;
        PrStringView typeName = *static_cast<const PrStringView*>(usdLayerBuildField(layer, s, "typeName")->value.data);
        if (!usdTypeFromName(typeName, &type, &isArray))
            continue;
        auto agrees = [&](const PrUsdValue& value) {
            return value.type == PrUsdType_Block || (value.type == type && value.isArray == isArray);
        };
        const PrUsdField* held = usdLayerBuildField(layer, s, "default");
        bool ok = held == nullptr || agrees(held->value);
        const PrUsdField* sampled = usdLayerBuildField(layer, s, "timeSamples");
        if (sampled != nullptr && sampled->value.type == PrUsdType_TimeSamples) {
            const PrUsdTimeSamples* samples = static_cast<const PrUsdTimeSamples*>(sampled->value.data);
            for (uint32_t i = 0; i < samples->count; ++i)
                ok = ok && agrees(samples->values[i]);
        } else if (sampled != nullptr) {
            ok = false;
        }
        if (!ok) {
            usdDiagnose(diagnostics, "an attribute's value is not of the type that the attribute declares");
            return false;
        }
    }

    // The children of each spec, by the kind of list: a spec's index is greater than its parent's, so one pass counts
    // and one pass places.
    uint32_t specCount = layer->specs.count;
    auto listOf = [](PrUsdSpec& parent, PrUsdSpecForm form) -> PrUsdList& {
        switch (form) {
            case PrUsdSpecForm_Prim:
                return parent.prims;
            case PrUsdSpecForm_VariantSet:
                return parent.variantSets;
            case PrUsdSpecForm_Variant:
                return parent.variants;
            default:
                return parent.properties;
        }
    };
    for (uint32_t s = 1; s < specCount; ++s)
        ++listOf(layer->specs.data[layer->specs.data[s].parent], layer->specs.data[s].form).count;
    uint32_t total = 0;
    for (uint32_t s = 0; s < specCount; ++s) {
        PrUsdSpec& spec = layer->specs.data[s];
        PrUsdList* lists[] = {&spec.prims, &spec.properties, &spec.variantSets, &spec.variants};
        for (PrUsdList* list : lists) {
            list->first = total;
            total += list->count;
            list->count = 0;
        }
    }
    layer->lists.resize(total);
    for (uint32_t s = 1; s < specCount; ++s) {
        PrUsdList& list = listOf(layer->specs.data[layer->specs.data[s].parent], layer->specs.data[s].form);
        layer->lists.data[list.first + list.count++] = s;
    }

    // The fields of each spec, together and sorted by name.
    layer->fields.resize(layer->buildFields.count);
    uint32_t placed = 0;
    for (uint32_t s = 0; s < specCount; ++s) {
        uint32_t first = placed;
        for (uint32_t at = layer->fieldHead.data[s]; at != kPrUsdNone; at = layer->buildFields.data[at].next)
            layer->fields.data[placed++] = layer->buildFields.data[at].field;
        sortItems(Span<PrUsdField>(layer->fields.data + first, placed - first),
            [](const PrUsdField& a, const PrUsdField& b) { return prStringCompare(a.name, b.name) < 0; });
        layer->specs.data[s].fieldCount = placed - first;
    }
    placed = 0;
    for (uint32_t s = 0; s < specCount; ++s) {
        layer->specs.data[s].fields = layer->fields.data + placed;
        placed += layer->specs.data[s].fieldCount;
    }
    layer->buildFields.reset();
    layer->fieldHead.reset();

    // The one range that the specification gives a field of its data model (usd-core/#framespersecond-double).
    const PrUsdField* rate = usdLayerFindField(layer, kPrUsdLayerSpec, "framesPerSecond");
    if (rate != nullptr) {
        bool positive = rate->value.type == PrUsdType_Double && !rate->value.isArray &&
            *static_cast<const double*>(rate->value.data) > 0;
        if (!positive) {
            usdDiagnose(diagnostics, "the layer's framesPerSecond is not a positive number");
            return false;
        }
    }

    // The sublayers, from the two fields that hold them.
    const PrUsdField* paths = usdLayerFindField(layer, kPrUsdLayerSpec, "subLayers");
    const PrUsdField* offsets = usdLayerFindField(layer, kPrUsdLayerSpec, "subLayerOffsets");
    if (paths != nullptr) {
        bool strings = paths->value.isArray && (paths->value.type == PrUsdType_String ||
            paths->value.type == PrUsdType_Asset);
        if (!strings) {
            usdDiagnose(diagnostics, "the layer's subLayers is not an array of asset paths");
            return false;
        }
        const PrStringView* assets = static_cast<const PrStringView*>(paths->value.data);
        for (uint32_t i = 0; i < paths->value.count; ++i)
            layer->sublayers.push(PrUsdSublayer{assets[i], PrUsdLayerOffset{0, 1}});
    }
    if (offsets != nullptr) {
        if (!offsets->value.isArray || offsets->value.type != PrUsdType_LayerOffset) {
            usdDiagnose(diagnostics, "the layer's subLayerOffsets is not an array of layer offsets");
            return false;
        }
        // The specification leaves lists of different lengths to the format: the offsets apply by position.
        const PrUsdLayerOffset* values = static_cast<const PrUsdLayerOffset*>(offsets->value.data);
        for (uint32_t i = 0; i < offsets->value.count && i < layer->sublayers.count; ++i)
            layer->sublayers.data[i].layerOffset = values[i];
    }
    return true;
}

void usdLayerDestroy(UsdLayer* PR_NULLABLE layer) {
    if (layer == nullptr)
        return;
    prBlobRelease(layer->file);
    atomTableReset(&layer->strings);
    layer->arena.reset();
    layer->specs.reset();
    layer->specOfAtom.reset();
    layer->lists.reset();
    layer->fields.reset();
    layer->sublayers.reset();
    layer->buildFields.reset();
    layer->fieldHead.reset();
    layer->textSpans.reset();
    layer->textOrder.reset();
    prMemoryFree(layer);
}

bool usdNameIsIdentifier(PrStringView name) {
    return name.size != 0 && identifierSize(name, 0) == name.size;
}

bool usdNameIsProperty(PrStringView name) {
    return name.size != 0 && propertyNameSize(name, 0) == name.size;
}

bool usdNameIsVariant(PrStringView name) {
    for (size_t i = 0; i < name.size; ++i) {
        if (!isVariantCharacter(name.data[i]) && !(i == 0 && name.data[i] == '.'))
            return false;
    }
    return true;
}

bool usdPathIsValid(PrStringView path, UsdPathKind kind) {
    size_t at = 0;
    return path.size != 0 && pathAt(path, at, kind, 0) && at == path.size;
}

bool usdFieldTypeFromName(PrStringView name, UsdFieldType* out) {
    uint32_t low = 0;
    uint32_t high = uint32_t(countOf(kFields));
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        int order = prStringCompare(kFields[middle].name, name);
        if (order == 0) {
            *out = kFields[middle].type;
            return true;
        }
        if (order < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return false;
}

bool usdFieldIsChildren(PrStringView name) {
    return name == "primChildren" || name == "properties" || name == "variantSetChildren" ||
        name == "variantChildren" || name == "propertyChildren";
}

void usdDiagnose(PrTextBuffer* PR_NULLABLE diagnostics, PrStringView sentence) {
    if (diagnostics != nullptr)
        prTextBufferAppend(diagnostics, sentence);
}

PrStringView usdTypeName(PrUsdType type) {
    PR_ASSERT(type < PrUsdType_Count);
    return kTypes[type].name;
}

uint32_t usdTypeSize(PrUsdType type) {
    PR_ASSERT(type < PrUsdType_Count);
    return kTypes[type].size;
}

bool usdTypeFromName(PrStringView typeName, PrUsdType* type, bool* isArray) {
    *isArray = typeName.size > 2 && typeName.data[typeName.size - 2] == '[' && typeName.data[typeName.size - 1] == ']';
    PrStringView name = *isArray ? PrStringView{typeName.data, typeName.size - 2} : typeName;
    for (PrUsdType t = PrUsdType_Bool; t <= PrUsdType_Matrix4d; ++t) {
        if (t != PrUsdType_Path && kTypes[t].name == name) {
            *type = t;
            return true;
        }
    }
    for (const TypeAlias& alias : kAliases) {
        if (alias.name == name) {
            *type = alias.type;
            return true;
        }
    }
    return false;
}

} // namespace pr_impl
