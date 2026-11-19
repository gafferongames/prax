// The lookup tables of a renderer's shader set.

#include "render/src/render_tables.h"

#include "pr/assert.h"
#include "pr/float16.h"
#include "pr/raw_array.h"
#include "pr/render_shaders.h"

namespace pr_impl {

PrStatus renderTablesCreate(Render* render) {
    Render* r = render;
    const PrRenderShaderSet& set = *r->shaders;
    uint32_t largest = prGpuDeviceInfo(r->device).maxTextureSize;
    if (set.tablesExtent.x > largest || set.tablesExtent.y > largest)
        return PrStatus_Limit;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, set.tablesExtent, PrGpuTextureUsageFlag_Sampled);
    desc.name = "render tables";
    PrStatus status = prGpuTextureCreate(r->device, &desc, &r->tables);
    if (status != PrStatus_Ok)
        return status;

    RawArray<float> floats{};
    RawArray<PrFloat16> halves{};
    // The texels that no table covers are zero.
    halves.resize(set.tablesExtent.x * set.tablesExtent.y * 4);
    for (uint32_t i = 0; i < halves.count; ++i)
        halves.data[i] = prFloat16FromFloat(0);
    for (uint32_t i = 0; i < set.tableCount; ++i) {
        const PrRenderTable& table = set.tables[i];
        // The application's source for the table, or the function that a table of the library's own names.
        PrRenderTableFillFn fill = table.fill;
        void* context = nullptr;
        for (uint32_t k = 0; k < r->options.tableCount; ++k) {
            if (r->options.tables[k].table == i) {
                fill = r->options.tables[k].fill;
                context = r->options.tables[k].context;
            }
        }
        PR_ASSERT(fill != nullptr);
        if (fill == nullptr)
            continue;
        floats.resize(table.extent.x * table.extent.y * 4);
        fill(context, table.extent, floats.data);
        for (uint32_t y = 0; y < table.extent.y; ++y) {
            const float* from = floats.data + size_t(y) * table.extent.x * 4;
            PrFloat16* to = halves.data + (size_t(table.origin.y + y) * set.tablesExtent.x + table.origin.x) * 4;
            for (uint32_t x = 0; x < table.extent.x * 4; ++x)
                to[x] = prFloat16FromFloat(from[x]);
        }
    }
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, set.tablesExtent);
    uint32_t rowBytes = set.tablesExtent.x * 4 * uint32_t(sizeof(PrFloat16));
    prGpuTextureUpload(r->device, r->tables, &region, halves.data, rowBytes, 0);
    floats.reset();
    halves.reset();
    return PrStatus_Ok;
}

} // namespace pr_impl
