// The textures that tests draw images with: a memory device that the tests that render in memory share, and caches of
// lib/draw's colors (PrTextureCacheValues_Encoded) that realize in the call that asks, so that a test's list draws its
// images at once.

#pragma once

#include "pr/draw.h"
#include "pr/draw_cpu.h"
#include "pr/gpu_memory.h"
#include "pr/texture_cache.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns a cache on `device` of lib/draw's colors, which realizes in the call that asks. The caller destroys it before
// the device.
inline PrTextureCache* harnessTextureCacheCreate(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.values = PrTextureCacheValues_Encoded;
    return prTextureCacheCreate(&desc);
}

// Returns the memory device that the tests that render in memory share, made on first use and kept for the process.
inline PrGpuDevice* harnessMemoryDevice() {
    static PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    return device;
}

// Returns the cache on the shared memory device.
inline PrTextureCache* harnessMemoryTextures() {
    static PrTextureCache* textures = harnessTextureCacheCreate(harnessMemoryDevice());
    return textures;
}

// Returns a Texture source of `rect` of the image's texels, realized in `textures`, with every mip level for `flags` of
// PrTextureCacheFlag_Levels, or a source that draws nothing if the image does not realize. The image must outlive the
// lists that draw the source.
inline PrDrawSource harnessImageSource(PrTextureCache* textures, const PrTextureSource* image, PrRect rect,
        PrTextureCacheFlags flags = 0) {
    PrTextureRegion region{};
    if (prTextureCacheWait(textures, image, flags, &region) != PrTextureCacheState_Ready)
        return PrDrawSource{};
    return prDrawSourceRegion(&region, rect);
}

// Returns a cpu renderer on the shared memory device, without a glyph decoder, which the caller destroys.
inline PrDrawRenderer* harnessMemoryRenderer() {
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.device = harnessMemoryDevice();
    return prDrawCpuCreate(&options);
}

// Returns a source of the image's texels in the shared memory device's cache, as harnessImageSource does.
inline PrDrawSource harnessMemoryImage(const PrTextureSource* image, PrRect rect, PrTextureCacheFlags flags = 0) {
    return harnessImageSource(harnessMemoryTextures(), image, rect, flags);
}

} // namespace harness

PR_ASSUME_NONNULL_END
