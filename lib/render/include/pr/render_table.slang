// The lookup tables of lib/render's shader set, as shaders read them: numbers that a shading model or a material type
// tabulates instead of computing, such as a lobe's albedo by roughness and angle, or the matrices of a linearly
// transformed cosine.
//
// A shader set declares its tables (pr_render_shaders, TABLE). Each has a width and a height in texels, and a C
// function fills it with four floats a texel when a renderer is created (PrRenderOptions::tables). The renderer keeps
// every table of its set in one texture of half floats, each in a region of its own, so the tables of a set cost one
// texture slot on a device without bindless, however many they are.
//
// The build defines a constant for each table of an application's set, kRenderTable<Name>, before it compiles the set's
// sources. The library's own table has its constant in pr/render_shading.slang.
//
// A table has two axes. A model that tabulates a function of three variables declares a table for each step of the
// third, and interpolates between two of them itself.

#pragma once

#include "pr/render_scene.slang"

// A table: where it lies in the texture that holds the set's tables, in texels, and its width and height.
struct RenderTable {
    float2 origin;
    float2 extent;
};

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(Texture2D<float4>, renderSlotTables, 7);

// Returns a texel of the texture that holds the set's tables.
float4 renderTablesLoad(int2 texel) {
    return renderSlotTables.Load(int3(texel, 0));
}

#else

float4 renderTablesLoad(int2 texel) {
    return prGpuGet<Texture2D<float4>>(frame.tables).Load(int3(texel, 0));
}

#endif

// Returns the value of a table at `uv`, which is clamped to [0, 1] on each axis. The table's first texel is at 0 and
// its last at 1, so a function tabulated at `extent` evenly spaced values of each variable, ends included, reads back
// exactly at those values, and is interpolated linearly between them.
//
// The function reads the four texels about `uv` by their indices and interpolates them itself. A device's own filter
// would weigh them with a few bits of precision, which differ from one device to another. So both binding tiers and
// every device read the same value, to the rounding of their arithmetic.
float4 renderTableSample(RenderTable table, float2 uv) {
    float2 last = table.extent - 1;
    float2 at = saturate(uv) * last;
    float2 low = min(floor(at), max(last - 1, 0));
    float2 part = at - low;
    // The indices are clamped as integers too, so that a coordinate that is not a number, as a surface without a normal
    // gives, reads texels of the table and not whatever lies outside the texture.
    int2 limit = int2(last);
    int2 first = clamp(int2(low), int2(0, 0), limit);
    int2 second = min(first + 1, limit);
    int2 origin = int2(table.origin);
    float4 lowRow = lerp(renderTablesLoad(origin + first), renderTablesLoad(origin + int2(second.x, first.y)), part.x);
    float4 highRow = lerp(renderTablesLoad(origin + int2(first.x, second.y)), renderTablesLoad(origin + second), part.x);
    return lerp(lowRow, highRow, part.y);
}
