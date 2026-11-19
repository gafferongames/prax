// The library's own shader set, which the build generates as it generates an application's (render_shaders.cmake), the
// library's own table, the lookups by name of a set's types, their parameters, textures and attributes, and the
// parameter records that a loader fills by name.

#include "pr/render_shaders.h"

#include "pr/assert.h"
#include "pr/mem.h"

#include "render/src/render_split_sum_table.h"
#include "render_shaders/RenderLibraryShaders.h"

namespace pr_impl {

namespace {

// The geometry of every mesh, as PrRenderGeometryVertex lays it out.
constexpr PrRenderVertexAttribute kGeometryAttributes[] = {
    {"position", PrRenderSemantic_Custom, 0, {PrMeshComponent_Float32, 3, false}, 0, {0, 0, 0, 1}},
    {"normal", PrRenderSemantic_Custom, 1, {PrMeshComponent_Int16, 4, true}, 12, {0, 0, 1, 0}},
    {"tangent", PrRenderSemantic_Custom, 2, {PrMeshComponent_Int16, 4, true}, 20, {0, 0, 0, 1}},
};

constexpr PrRenderVertexLayout kGeometryLayout = {
    .attributes = kGeometryAttributes,
    .attributeCount = 3,
    .stride = kPrRenderGeometryBytes,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prRenderSplitSumTable(void* context, PrUVec2 extent, float* texels) {
    (void)context;
    PR_ASSERT(extent.x == kRenderSplitSumSide && extent.y == kRenderSplitSumSide);
    for (uint32_t i = 0; i < extent.x * extent.y; ++i) {
        texels[i * 4] = float(kRenderSplitSum[i * 3]) / 65535;
        texels[i * 4 + 1] = float(kRenderSplitSum[i * 3 + 1]) / 65535;
        texels[i * 4 + 2] = float(kRenderSplitSum[i * 3 + 2]) / 65535;
        texels[i * 4 + 3] = 1;
    }
}

uint32_t prRenderVertexAttributeFromName(const PrRenderVertexLayout* layout, PrStringView name) {
    for (uint32_t i = 0; i < layout->attributeCount; ++i) {
        if (layout->attributes[i].name == name)
            return i;
    }
    return kPrRenderVertexAttributeMissing;
}

PrStatus prRenderVertexLayoutFill(const PrRenderVertexLayout* layout, const PrRenderAttributeSource* PR_NULLABLE sources,
        uint32_t sourceCount, uint32_t vertexCount, void* out, uint32_t* PR_NULLABLE missing) {
    uint32_t unmatched = 0;
    PrStatus status = PrStatus_Ok;
    for (uint32_t a = 0; a < layout->attributeCount && status == PrStatus_Ok; ++a) {
        const PrRenderVertexAttribute& attribute = layout->attributes[a];
        PrMeshAttributeView values{};
        bool matched = false;
        for (uint32_t i = 0; i < sourceCount && !matched; ++i) {
            const PrRenderAttributeSource& source = sources[i];
            if (source.semantic != attribute.semantic)
                continue;
            if (attribute.semantic == PrRenderSemantic_Custom)
                matched = source.name == attribute.name;
            else
                matched = source.semanticIndex == attribute.semanticIndex;
            if (matched)
                values = source.values;
        }
        unmatched += matched ? 0 : 1;
        PrRenderVec4 f = attribute.fallback;
        const float fallback[4] = {f.x, f.y, f.z, f.w};
        uint8_t* first = static_cast<uint8_t*>(out) + attribute.offset;
        PrMeshAttribute written{first, vertexCount, layout->stride, attribute.format};
        status = prMeshAttributeConvert(values, fallback, written);
    }
    if (missing != nullptr)
        *missing = unmatched;
    return status;
}

uint32_t prRenderMaterialTextureFromName(const PrRenderMaterialType* type, PrStringView name) {
    for (uint32_t i = 0; i < type->textureCount; ++i) {
        if (type->textures[i].name == name)
            return i;
    }
    return kPrRenderMaterialTextureMissing;
}

uint32_t prRenderMaterialParameterFromName(const PrRenderMaterialType* type, PrStringView name) {
    for (uint32_t i = 0; i < type->parameterCount; ++i) {
        if (type->parameters[i].name == name)
            return i;
    }
    return kPrRenderMaterialParameterMissing;
}

void prRenderMaterialParametersDefault(const PrRenderMaterialType* type, void* record) {
    prMemZero(record, type->parameterBytes);
    for (uint32_t i = 0; i < type->parameterCount; ++i) {
        const PrRenderMaterialParameter& p = type->parameters[i];
        const float value[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
        float values[4 * 255];
        for (uint32_t e = 0; e < p.elementCount; ++e)
            prMemCopy(values + e * p.componentCount, value, p.componentCount * sizeof(float));
        prRenderMaterialParameterSet(type, record, i, values, uint32_t(p.elementCount) * p.componentCount);
    }
}

void prRenderMaterialParameterSet(const PrRenderMaterialType* type, void* record, uint32_t parameter, const float* values,
        uint32_t count) {
    PR_ASSERT(parameter < type->parameterCount);
    const PrRenderMaterialParameter& p = type->parameters[parameter];
    uint8_t* at = static_cast<uint8_t*>(record) + p.offset;
    for (uint32_t i = 0; i < count && i < uint32_t(p.componentCount) * p.elementCount; ++i) {
        if (p.kind == PrRenderParameterKind_Float) {
            prMemCopy(at + i * 4, &values[i], 4);
        } else if (p.kind == PrRenderParameterKind_Int) {
            int32_t value = int32_t(values[i]);
            prMemCopy(at + i * 4, &value, 4);
        } else {
            uint32_t value = values[i] > 0 ? uint32_t(values[i]) : 0;
            prMemCopy(at + i * 4, &value, 4);
        }
    }
}

const PrRenderShaderSet* prRenderShaderSetLibrary(void) {
    return renderLibraryShaderSet();
}

uint32_t prRenderMaterialTypeFromName(const PrRenderShaderSet* set, PrStringView name) {
    for (uint32_t i = 0; i < set->materialTypeCount; ++i) {
        if (set->materialTypes[i].name == name)
            return i;
    }
    return kPrRenderMaterialTypeMissing;
}

const PrRenderVertexLayout* prRenderMaterialTypeLayout(const PrRenderShaderSet* set, uint32_t type, uint32_t attributes) {
    PR_ASSERT(type < set->materialTypeCount && attributes < set->materialTypes[type].variantCount);
    return &set->layouts[set->variants[set->materialTypes[type].firstVariant + attributes].layout];
}

const PrRenderVertexLayout* prRenderGeometryLayout(void) {
    return &kGeometryLayout;
}
