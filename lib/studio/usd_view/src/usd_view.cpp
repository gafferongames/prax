// Studio's USD structure view: what a composed stage holds and where it came from.

#include "pr/usd_view.h"

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/kit_box.h"
#include "pr/kit_collection.h"
#include "pr/kit_text.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// The height of a table's row, in px, and the most rows that a table shows before it scrolls.
constexpr float kRowHeight = 22;
constexpr uint32_t kTableRows = 12;

// The most elements of an array that a value's text shows.
constexpr uint32_t kShownElements = 4;

struct UsdView {
    PrKit* kit;
    PrKitSelection properties;
    PrKitSelection opinions;
    PrKitSelection layers;
};

// What one element of a value holds: `count` components of one kind.
enum {
    UsdComponent_None,
    UsdComponent_Bool,
    UsdComponent_U8,
    UsdComponent_I32,
    UsdComponent_U32,
    UsdComponent_I64,
    UsdComponent_U64,
    UsdComponent_F16,
    UsdComponent_F32,
    UsdComponent_F64,
    UsdComponent_Text,
};
typedef uint8_t UsdComponent;

struct UsdElement {
    UsdComponent component;
    uint32_t count;
};

UsdElement elementOf(PrUsdType type) {
    switch (type) {
        case PrUsdType_Bool:
            return {UsdComponent_Bool, 1};
        case PrUsdType_UChar:
            return {UsdComponent_U8, 1};
        case PrUsdType_Int:
            return {UsdComponent_I32, 1};
        case PrUsdType_UInt:
            return {UsdComponent_U32, 1};
        case PrUsdType_Int64:
            return {UsdComponent_I64, 1};
        case PrUsdType_UInt64:
            return {UsdComponent_U64, 1};
        case PrUsdType_Half:
            return {UsdComponent_F16, 1};
        case PrUsdType_Float:
            return {UsdComponent_F32, 1};
        case PrUsdType_Double:
        case PrUsdType_TimeCode:
            return {UsdComponent_F64, 1};
        case PrUsdType_String:
        case PrUsdType_Token:
        case PrUsdType_Asset:
        case PrUsdType_Path:
        case PrUsdType_PathExpression:
        case PrUsdType_Unregistered:
            return {UsdComponent_Text, 1};
        case PrUsdType_Int2:
            return {UsdComponent_I32, 2};
        case PrUsdType_Int3:
            return {UsdComponent_I32, 3};
        case PrUsdType_Int4:
            return {UsdComponent_I32, 4};
        case PrUsdType_Half2:
            return {UsdComponent_F16, 2};
        case PrUsdType_Half3:
            return {UsdComponent_F16, 3};
        case PrUsdType_Half4:
        case PrUsdType_Quath:
            return {UsdComponent_F16, 4};
        case PrUsdType_Float2:
            return {UsdComponent_F32, 2};
        case PrUsdType_Float3:
            return {UsdComponent_F32, 3};
        case PrUsdType_Float4:
        case PrUsdType_Quatf:
            return {UsdComponent_F32, 4};
        case PrUsdType_Double2:
            return {UsdComponent_F64, 2};
        case PrUsdType_Double3:
            return {UsdComponent_F64, 3};
        case PrUsdType_Double4:
        case PrUsdType_Quatd:
            return {UsdComponent_F64, 4};
        case PrUsdType_Matrix2d:
            return {UsdComponent_F64, 4};
        case PrUsdType_Matrix3d:
            return {UsdComponent_F64, 9};
        case PrUsdType_Matrix4d:
            return {UsdComponent_F64, 16};
        default:
            break;
    }
    return {UsdComponent_None, 0};
}

void appendNumber(PrTextBuffer* out, double value) {
    if (value == double(int64_t(value)) && value > -1e15 && value < 1e15) {
        if (value < 0)
            prTextBufferAppendChar(out, '-');
        prTextBufferAppendU64(out, uint64_t(value < 0 ? -value : value), 10);
        return;
    }
    prTextBufferAppendF64Fixed(out, value, 4);
}

// Appends component `index` of the element at `data`.
void appendComponent(PrTextBuffer* out, UsdComponent component, const uint8_t* data, uint32_t index) {
    switch (component) {
        case UsdComponent_Bool:
            prTextBufferAppend(out, reinterpret_cast<const bool*>(data)[index] ? "true" : "false");
            break;
        case UsdComponent_U8:
            prTextBufferAppendU64(out, data[index], 10);
            break;
        case UsdComponent_I32: {
            int32_t value = 0;
            prMemCopy(&value, data + 4 * index, 4);
            appendNumber(out, value);
            break;
        }
        case UsdComponent_U32: {
            uint32_t value = 0;
            prMemCopy(&value, data + 4 * index, 4);
            prTextBufferAppendU64(out, value, 10);
            break;
        }
        case UsdComponent_I64: {
            int64_t value = 0;
            prMemCopy(&value, data + 8 * index, 8);
            appendNumber(out, double(value));
            break;
        }
        case UsdComponent_U64: {
            uint64_t value = 0;
            prMemCopy(&value, data + 8 * index, 8);
            prTextBufferAppendU64(out, value, 10);
            break;
        }
        case UsdComponent_F16: {
            PrFloat16 value{};
            prMemCopy(&value, data + 2 * index, 2);
            appendNumber(out, double(prFloat16ToFloat(value)));
            break;
        }
        case UsdComponent_F32: {
            float value = 0;
            prMemCopy(&value, data + 4 * index, 4);
            appendNumber(out, double(value));
            break;
        }
        case UsdComponent_F64: {
            double value = 0;
            prMemCopy(&value, data + 8 * index, 8);
            appendNumber(out, value);
            break;
        }
        case UsdComponent_Text: {
            PrStringView text = reinterpret_cast<const PrStringView*>(data)[index];
            prTextBufferAppendChar(out, '"');
            prTextBufferAppend(out, text);
            prTextBufferAppendChar(out, '"');
            break;
        }
        default:
            break;
    }
}

// Returns the size of an element's component.
uint32_t componentSize(UsdComponent component) {
    switch (component) {
        case UsdComponent_Bool:
        case UsdComponent_U8:
            return 1;
        case UsdComponent_F16:
            return 2;
        case UsdComponent_I32:
        case UsdComponent_U32:
        case UsdComponent_F32:
            return 4;
        case UsdComponent_I64:
        case UsdComponent_U64:
        case UsdComponent_F64:
            return 8;
        case UsdComponent_Text:
            return uint32_t(sizeof(PrStringView));
        default:
            break;
    }
    return 0;
}

void appendElement(PrTextBuffer* out, UsdElement element, const uint8_t* data) {
    if (element.count > 1)
        prTextBufferAppendChar(out, '(');
    for (uint32_t i = 0; i < element.count; ++i) {
        if (i != 0)
            prTextBufferAppend(out, ", ");
        appendComponent(out, element.component, data, i);
    }
    if (element.count > 1)
        prTextBufferAppendChar(out, ')');
}

PrStringView arcName(PrUsdArc arc) {
    constexpr PrStringView kNames[] = {"root", "inherit", "variant", "relocate", "reference", "payload", "specialize"};
    if (arc >= countOf(kNames))
        return "arc";
    return kNames[arc];
}

PrStringView specifierName(PrUsdSpecifier specifier) {
    constexpr PrStringView kNames[] = {"def", "over", "class"};
    return specifier < 3 ? kNames[specifier] : PrStringView{""};
}

// A section's heading.
void heading(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    PrKitTextStyle style{.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted};
    PrKitBoxStyle box{.padding = {.top = 10, .bottom = 4}};
    prKitLabel(kit, prKitBox(kit, parent, {}, box), {.text = text}, style);
}

void line(PrKit* kit, PrLayoutNode parent, PrStringView text, PrKitTextRole role, PrKitTone tone) {
    prKitLabel(kit, parent, {.text = text}, {.role = role, .tone = tone});
}

// The style of a table of `rows` rows, which grows to kTableRows rows and then scrolls.
PrKitListViewStyle tableStyle(uint32_t rows) {
    uint32_t shown = rows < kTableRows ? rows : kTableRows;
    float height = kRowHeight * float(shown + 1) + 4;
    return PrKitListViewStyle{.rowHeight = kRowHeight, .place = {.height = prKitPx(height), .flex = PrKitFlex_Fixed}};
}

// Returns where a property's part of its spec path starts: at the dot after the path's last prim, or at its end for a
// prim's path.
size_t propertyDot(PrStringView path) {
    size_t slash = 0;
    for (size_t i = 0; i < path.size; ++i) {
        if (path.data[i] == '/')
            slash = i;
    }
    for (size_t i = slash; i < path.size; ++i) {
        if (path.data[i] == '.')
            return i;
    }
    return path.size;
}

// Returns how a layer is named in a table: its location relative to the folder of the stage's root layer, if it is in
// that folder, and its whole location otherwise.
PrStringView layerName(const PrUsdStage* stage, uint32_t layer) {
    PrStringView location = prUsdStageLayerLocation(stage, layer);
    PrStringView root = prUsdStageLayerLocation(stage, 0);
    size_t folder = 0;
    for (size_t i = 0; i < root.size; ++i) {
        if (root.data[i] == '/')
            folder = i + 1;
    }
    if (folder == 0 || location.size <= folder || prMemCompare(location.data, root.data, folder) != 0)
        return location;
    return PrStringView{location.data + folder, location.size - folder};
}

// The opinions of a prim, or of one of its properties, strongest first.
void opinionsTable(UsdView* view, PrLayoutNode parent, const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property) {
    PrKit* kit = view->kit;
    RawArray<PrUsdOpinion> opinions{};
    if (property == kPrUsdNone) {
        const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
        for (uint32_t i = 0; i < record.opinionCount; ++i)
            opinions.push(prUsdStageOpinion(stage, record.firstOpinion + i));
    } else {
        uint32_t count = prUsdStagePropertyOpinions(stage, prim, property, nullptr, 0);
        opinions.resize(count);
        prUsdStagePropertyOpinions(stage, prim, property, opinions.data, count);
    }
    // The offset's column shows only when an opinion has an offset.
    bool offsets = false;
    for (uint32_t i = 0; i < opinions.count; ++i)
        offsets = offsets || opinions.data[i].offset.offset != 0 || opinions.data[i].offset.scale != 1;
    const PrKitTableColumn columns[] = {
        {.label = "#", .width = 24},
        {.label = "Arc", .width = 48, .size = PrKitColumnSize_Fit},
        {.label = "Layer", .width = 80, .maxWidth = 240, .size = PrKitColumnSize_Fit},
        {.label = "Spec", .width = 160, .size = PrKitColumnSize_Fill},
        {.label = "Offset", .width = 40, .size = PrKitColumnSize_Fit},
    };
    PrKitTable content{
        .count = opinions.count,
        .selection = &view->opinions,
        .columns = columns,
        .columnCount = offsets ? 5u : 4u,
    };
    PrLayoutNode table = prKitTable(kit, parent, content, tableStyle(opinions.count));
    PrKitWindow window = prKitListWindow(kit, table);
    PrTextBuffer text = {};
    PrTextBuffer primPath = {};
    prUsdStagePrimPath(stage, prim, &primPath);
    for (uint32_t i = window.first; i < window.first + window.count && i < opinions.count; ++i) {
        const PrUsdOpinion& opinion = opinions.data[i];
        PrLayoutNode row = prKitTableRow(kit, table, {.index = i, .item = i, .selected = view->opinions.lead == i});
        prTextBufferClear(&text);
        prTextBufferAppendU64(&text, i, 10);
        prKitTableCell(kit, row, {.column = 0, .text = prTextBufferView(&text), .role = PrKitTextRole_Mono});
        prKitTableCell(kit, row, {.column = 1, .text = arcName(opinion.arc)});
        prKitTableCell(kit, row, {.column = 2, .text = layerName(stage, opinion.layer), .role = PrKitTextRole_Mono});
        // A property's spec is its name under its prim's spec in the layer. Its name leads, and its prim's spec
        // follows, faint, where it is not the composed prim's own path, as across a reference.
        const PrUsdLayer* layer = prUsdStageLayer(stage, opinion.layer);
        PrStringView spec = layer != nullptr ? prUsdLayerSpecs(layer)[opinion.spec].path : PrStringView{""};
        size_t dot = property != kPrUsdNone ? propertyDot(spec) : spec.size;
        PrStringView primPart{spec.data, dot};
        if (dot == spec.size) {
            PrKitTableCell cell{.column = 3, .text = spec, .role = PrKitTextRole_Mono, .tone = PrKitTone_Secondary};
            prKitTableCell(kit, row, cell);
        } else {
            PrKitTableCell cell{.column = 3, .text = prStringFrom(spec, dot), .role = PrKitTextRole_Mono};
            PrLayoutNode node = prKitTableCell(kit, row, cell);
            if (primPart != prTextBufferView(&primPath)) {
                PrKitTextStyle faint{.role = PrKitTextRole_Mono, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Nowrap};
                prKitLabel(kit, node, {.text = primPart}, faint);
            }
        }
        if (!offsets)
            continue;
        prTextBufferClear(&text);
        if (opinion.offset.offset != 0 || opinion.offset.scale != 1) {
            appendNumber(&text, opinion.offset.offset);
            prTextBufferAppend(&text, " x");
            appendNumber(&text, opinion.offset.scale);
        }
        prKitTableCell(kit, row, {.column = 4, .text = prTextBufferView(&text), .tone = PrKitTone_Muted});
    }
    prTextBufferReset(&text);
    prTextBufferReset(&primPath);
    opinions.reset();
}

// The resolved properties of a prim, with their types and values, which choose the property whose opinions show.
void propertiesTable(UsdView* view, PrLayoutNode parent, const PrUsdStage* stage, PrUsdPrimIndex prim,
        uint32_t* property, PrUsdScratch* scratch) {
    PrKit* kit = view->kit;
    const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
    const PrUsdProperty* properties = prUsdStageProperties(stage);
    const PrKitTableColumn columns[] = {
        {.label = "Property", .width = 120, .maxWidth = 320, .size = PrKitColumnSize_Fit},
        {.label = "Type", .width = 48, .maxWidth = 160, .size = PrKitColumnSize_Fit},
        {.label = "Value", .width = 160, .size = PrKitColumnSize_Fill},
    };
    PrKitTable content{
        .count = record.propertyCount,
        .selection = &view->properties,
        .columns = columns,
        .columnCount = 3,
    };
    PrLayoutNode table = prKitTable(kit, parent, content, tableStyle(record.propertyCount));
    PrKitWindow window = prKitListWindow(kit, table);
    PrTextBuffer text = {};
    for (uint32_t i = window.first; i < window.first + window.count && i < record.propertyCount; ++i) {
        uint32_t index = record.firstProperty + i;
        const PrUsdProperty& shown = properties[index];
        PrLayoutNode row = prKitTableRow(kit, table, {.index = i, .item = index, .selected = *property == index});
        prKitTableCell(kit, row, {.column = 0, .text = shown.name, .role = PrKitTextRole_Mono});
        PrUsdValue value{};
        PrStringView type = "rel";
        if (shown.form == PrUsdSpecForm_Attribute) {
            type = "";
            if (prUsdStagePropertyField(stage, prim, index, "typeName", scratch, &value) &&
                    value.type == PrUsdType_Token && value.count == 1)
                type = *static_cast<const PrStringView*>(value.data);
        }
        prKitTableCell(kit, row, {.column = 1, .text = type, .tone = PrKitTone_Secondary});
        prTextBufferClear(&text);
        bool has = false;
        if (shown.form == PrUsdSpecForm_Attribute)
            has = prUsdStageAttributeValue(stage, prim, index, kPrF64Nan, scratch, &value);
        else
            has = prUsdStageTargets(stage, prim, index, false, scratch, &value, nullptr);
        if (has)
            prUsdViewValueText(&value, &text);
        else if (shown.form == PrUsdSpecForm_Attribute && prUsdStageAttributeVaries(stage, prim, index))
            prTextBufferAppend(&text, "time samples");
        prKitTableCell(kit, row, {.column = 2, .text = prTextBufferView(&text), .role = PrKitTextRole_Mono});
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, table, &edit) && view->properties.lead < record.propertyCount)
        *property = record.firstProperty + view->properties.lead;
    prTextBufferReset(&text);
}

// The variant sets of a prim, each with the variant that composition selected.
void variantSets(PrKit* kit, PrLayoutNode parent, const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch) {
    PrUsdValue sets{};
    if (!prUsdStagePrimVariantSets(stage, prim, scratch, &sets) || sets.count == 0)
        return;
    heading(kit, parent, "VARIANT SETS");
    PrTextBuffer text = {};
    const PrStringView* names = static_cast<const PrStringView*>(sets.data);
    for (uint32_t i = 0; i < sets.count; ++i) {
        prTextBufferClear(&text);
        prTextBufferAppend(&text, names[i]);
        prTextBufferAppend(&text, " = ");
        PrStringView selection = prUsdStagePrimVariantSelection(stage, prim, names[i]);
        prTextBufferAppend(&text, selection.size != 0 ? selection : PrStringView{"(none)"});
        line(kit, parent, prTextBufferView(&text), PrKitTextRole_Mono, PrKitTone_Primary);
    }
    prTextBufferReset(&text);
}

// The stage's metadata, its layers and its composition errors.
void stageSections(UsdView* view, PrLayoutNode parent, const PrUsdStage* stage) {
    PrKit* kit = view->kit;
    heading(kit, parent, "STAGE");
    PrTextBuffer text = {};
    const PrStringView fields[] = {"defaultPrim", "upAxis", "metersPerUnit", "startTimeCode", "endTimeCode"};
    for (PrStringView field : fields) {
        PrUsdValue value{};
        if (!prUsdStageField(stage, field, &value))
            continue;
        prTextBufferClear(&text);
        prTextBufferAppend(&text, field);
        prTextBufferAppend(&text, " = ");
        prUsdViewValueText(&value, &text);
        line(kit, parent, prTextBufferView(&text), PrKitTextRole_Mono, PrKitTone_Primary);
    }
    prTextBufferClear(&text);
    prTextBufferAppend(&text, "time codes per second = ");
    appendNumber(&text, prUsdStageTimeCodesPerSecond(stage));
    line(kit, parent, prTextBufferView(&text), PrKitTextRole_Mono, PrKitTone_Secondary);

    heading(kit, parent, "LAYERS");
    uint32_t count = prUsdStageLayerCount(stage);
    const PrKitTableColumn columns[] = {
        {.label = "#", .width = 24},
        {.label = "Location", .width = 240, .size = PrKitColumnSize_Fill},
        {.label = "Format", .width = 48, .size = PrKitColumnSize_Fit},
    };
    PrKitTable content{.count = count, .selection = &view->layers, .columns = columns, .columnCount = 3};
    PrLayoutNode table = prKitTable(kit, parent, content, tableStyle(count));
    PrKitWindow window = prKitListWindow(kit, table);
    for (uint32_t i = window.first; i < window.first + window.count && i < count; ++i) {
        PrLayoutNode row = prKitTableRow(kit, table, {.index = i, .item = i, .selected = view->layers.lead == i});
        prTextBufferClear(&text);
        prTextBufferAppendU64(&text, i, 10);
        prKitTableCell(kit, row, {.column = 0, .text = prTextBufferView(&text), .role = PrKitTextRole_Mono});
        prKitTableCell(kit, row, {.column = 1, .text = prUsdStageLayerLocation(stage, i), .role = PrKitTextRole_Mono});
        const PrUsdLayer* layer = prUsdStageLayer(stage, i);
        PrStringView format = "unread";
        if (layer != nullptr && prUsdLayerFormat(layer) == PrUsdFormat_Text)
            format = "text";
        else if (layer != nullptr)
            format = "crate";
        prKitTableCell(kit, row, {.column = 2, .text = format, .tone = PrKitTone_Secondary});
    }
    uint32_t errors = prUsdStageErrorCount(stage);
    if (errors != 0) {
        heading(kit, parent, "COMPOSITION ERRORS");
        const PrUsdError* list = prUsdStageErrors(stage);
        for (uint32_t i = 0; i < errors; ++i) {
            PrKitTextStyle style{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Wrap};
            prKitLabel(kit, parent, {.text = list[i].message}, style);
        }
    }
    prTextBufferReset(&text);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrUsdView : UsdView {};

PrUsdView* prUsdViewCreate(PrKit* kit) {
    PrUsdView* view = allocZeroed<PrUsdView>();
    view->kit = kit;
    view->properties = {kPrKitRowNone, kPrKitRowNone};
    view->opinions = {kPrKitRowNone, kPrKitRowNone};
    view->layers = {kPrKitRowNone, kPrKitRowNone};
    return view;
}

void prUsdViewDestroy(PrUsdView* PR_NULLABLE view) {
    prMemoryFree(view);
}

void prUsdViewFrame(PrUsdView* view, PrLayoutNode parent, PrUsdViewContent content) {
    PrKit* kit = view->kit;
    const PrUsdStage* stage = content.stage;
    PrKitBoxStyle column{
        .overflow = PrKitOverflow_ScrollY,
        .padding = prKitEdgesAll(8),
        .place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode box = prKitBox(kit, parent, {}, column);
    PrUsdPrimIndex prim = content.prim < prUsdStagePrimCount(stage) ? content.prim : kPrUsdPseudoRoot;
    const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
    if (*content.property != kPrUsdNone && (*content.property < record.firstProperty ||
            *content.property >= record.firstProperty + record.propertyCount))
        *content.property = kPrUsdNone;
    PrTextBuffer text = {};
    prUsdStagePrimPath(stage, prim, &text);
    line(kit, box, prTextBufferView(&text), PrKitTextRole_Heading, PrKitTone_Primary);
    if (prim == kPrUsdPseudoRoot) {
        stageSections(view, box, stage);
        prTextBufferReset(&text);
        return;
    }
    prTextBufferClear(&text);
    prTextBufferAppend(&text, specifierName(record.specifier));
    if (record.typeName.size != 0) {
        prTextBufferAppendChar(&text, ' ');
        prTextBufferAppend(&text, record.typeName);
    }
    struct Flag {
        PrUsdPrimFlags flag;
        PrStringView name;
    };
    const Flag flags[] = {
        {PrUsdPrimFlag_Active, "active"}, {PrUsdPrimFlag_Loaded, "loaded"}, {PrUsdPrimFlag_Defined, "defined"},
        {PrUsdPrimFlag_Abstract, "abstract"}, {PrUsdPrimFlag_Instance, "instance"},
        {PrUsdPrimFlag_InPrototype, "in prototype"}, {PrUsdPrimFlag_HasPayload, "payload"},
    };
    for (const Flag& flag : flags) {
        if ((record.flags & flag.flag) == 0)
            continue;
        prTextBufferAppend(&text, "  ");
        prTextBufferAppend(&text, flag.name);
    }
    line(kit, box, prTextBufferView(&text), PrKitTextRole_Mono, PrKitTone_Secondary);
    PrUsdScratch* scratch = prUsdScratchCreate();
    variantSets(kit, box, stage, prim, scratch);
    heading(kit, box, "PROPERTIES");
    propertiesTable(view, box, stage, prim, content.property, scratch);
    heading(kit, box, "OPINIONS OF THE PRIM, STRONGEST FIRST");
    opinionsTable(view, box, stage, prim, kPrUsdNone);
    if (*content.property != kPrUsdNone) {
        prTextBufferClear(&text);
        prTextBufferAppend(&text, "OPINIONS OF .");
        prTextBufferAppend(&text, prUsdStageProperties(stage)[*content.property].name);
        heading(kit, box, prTextBufferView(&text));
        opinionsTable(view, box, stage, prim, *content.property);
    }
    prUsdScratchDestroy(scratch);
    prTextBufferReset(&text);
}

void prUsdViewPrimLabel(const PrUsdStage* stage, PrUsdPrimIndex prim, PrTextBuffer* out) {
    const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
    prTextBufferAppend(out, prim == kPrUsdPseudoRoot ? PrStringView{"/"} : record.name);
    if (record.typeName.size != 0) {
        prTextBufferAppend(out, "  ");
        prTextBufferAppend(out, record.typeName);
    }
}

void prUsdViewValueText(const PrUsdValue* value, PrTextBuffer* out) {
    UsdElement element = elementOf(value->type);
    if (value->type == PrUsdType_Block) {
        prTextBufferAppend(out, "None");
        return;
    }
    if (element.component == UsdComponent_None || value->data == nullptr) {
        prTextBufferAppend(out, prUsdTypeName(value->type));
        return;
    }
    const uint8_t* data = static_cast<const uint8_t*>(value->data);
    uint32_t stride = componentSize(element.component) * element.count;
    if (!value->isArray) {
        appendElement(out, element, data);
        return;
    }
    prTextBufferAppendChar(out, '[');
    prTextBufferAppendU64(out, value->count, 10);
    prTextBufferAppend(out, "] ");
    for (uint32_t i = 0; i < value->count && i < kShownElements; ++i) {
        if (i != 0)
            prTextBufferAppend(out, ", ");
        appendElement(out, element, data + size_t(i) * stride);
    }
    if (value->count > kShownElements)
        prTextBufferAppend(out, ", ...");
}
