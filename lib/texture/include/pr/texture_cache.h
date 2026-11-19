// A texture cache: the textures that sources realize on one device, for a host that draws them.
//
// A host asks for a source's texture each time it draws it (prTextureCacheRealize). The first ask starts the source's
// realization on the executor that the host gave the cache, reading the source's bits and uploading them as
// prTextureUpload does, and later asks hand back the region of a texture that holds it once it has arrived. The cache
// owns no threads: without an executor, a realization runs in the ask that starts it. It calls the host's `arrived`
// function as a realization that an ask found pending arrives, so that the host draws again.
//
// The cache keeps a source's region while anything else holds the source, since drawings that name the region may be
// kept as long as the source is. Past that, it keeps what fits a budget.
//
// A region is a texture of its own or a slot of a page that the cache shares between small sources, as the cache
// decides from the source's size and format, the device and its tier. A consumer samples the region's rect alone. The
// cache keeps what it realized while a budget allows, and evicts the textures that the frames since the last trim did
// not ask for (prTextureCacheTrim). After the device is lost, the host moves the cache to a new device, which realizes
// every source again as it is asked for.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/status.h"
#include "pr/task.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A cache of the textures that sources realize on one device. One thread at a time asks it for sources and trims it,
// while its realizations complete on any.
typedef struct PrTextureCache PrTextureCache;

// What an ask wants of a source's texture.
enum {
    // Every mip level, down to one texel, which a texture of the source's own holds. A source of one level gets the
    // levels that the device makes from it. An ask without the flag gets this realization once it is ready, so that one
    // texture of the source serves both.
    PrTextureCacheFlag_Levels = 1u << 0,
};
typedef uint8_t PrTextureCacheFlags;

// Where a source's realization is.
enum {
    PrTextureCacheState_Ready,      // arrived: the region holds it
    PrTextureCacheState_Pending,    // in flight
    PrTextureCacheState_Failed,     // its bits do not read or upload, and the cache does not try again
};
typedef uint8_t PrTextureCacheState;

// The values that a realized texture holds.
enum {
    // As the source's format says a sampler reads them: a Srgb form decodes as sRGB.
    PrTextureCacheValues_Sampled,
    // Values of sRGB's transfer function, which a sampler reads as they are: a Srgb form realizes as its plain form,
    // and floats are encoded as extended sRGB, mirrored below zero, in half floats.
    PrTextureCacheValues_Encoded,
};
typedef uint8_t PrTextureCacheValues;

// Whether the cache packs small sources into shared pages.
enum {
    PrTextureCachePack_Auto,        // on a device that draws through a GPU, and not on a memory device
    PrTextureCachePack_Never,
    PrTextureCachePack_Always,      // every source that a page can hold
};
typedef uint8_t PrTextureCachePack;

// Called with the host's context on the thread that completed a realization that an ask found pending, once it has
// arrived or failed. It must not call the cache.
typedef void (*PrTextureCacheArrivedFn)(void* PR_NULLABLE context);

// How a cache realizes sources, and where.
typedef struct PrTextureCacheDesc {
    PrGpuDevice* device;                        // outlives the cache, or until prTextureCacheSetDevice
    PrTaskPool* PR_NULLABLE tasks;              // runs the realizations, and outlives the cache
    uint64_t budget;                            // bytes of idle textures kept, whose sources only the cache holds
    PrTextureCacheValues values;
    PrTextureAlpha alpha;                       // of realized pixels, Straight or Premultiplied; opaque ones stay None
    PrTextureCachePack pack;
    PrTextureCacheArrivedFn PR_NULLABLE arrived;
    void* PR_NULLABLE arrivedContext;
} PrTextureCacheDesc;

// Returns a description of a cache on `device` without an executor, a budget of 64 MiB, values as they are sampled,
// premultiplied pixels and packing as it pays.
PrTextureCacheDesc prTextureCacheDescDefault(PrGpuDevice* device);

// Creates a cache as `desc` describes.
PrTextureCache* prTextureCacheCreate(const PrTextureCacheDesc* desc);

// Waits for the realizations in flight, and frees every texture of the cache and the cache.
void prTextureCacheDestroy(PrTextureCache* PR_NULLABLE cache);

// Returns the device that the cache realizes on.
PrGpuDevice* prTextureCacheDevice(const PrTextureCache* cache);

// Waits for the realizations in flight, frees every texture of the cache on its device, and makes `device` the one it
// realizes on, as after a lost device. Every source realizes again as it is asked for, and none has failed. Every hold
// region is forgotten, and the cache's generation advances.
void prTextureCacheSetDevice(PrTextureCache* cache, PrGpuDevice* device);

// Returns the number of times the cache has moved to another device, which a host that keeps drawings of regions
// compares with the generation it drew them in, to draw them again.
uint64_t prTextureCacheGeneration(const PrTextureCache* cache);

// Returns where the realization of `source` with `flags` is, and starts it if it has not started. A Ready source's
// region is written to `*out`, which is zeroed otherwise. The cache holds a reference to the source. The region stays
// valid, its texels unchanged, while anything else holds the source, so a drawing that keeps the source keeps its
// region. Once only the cache holds it, a trim that finds no ask since the trim before may evict it.
PrTextureCacheState prTextureCacheRealize(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheFlags flags,
    PrTextureRegion* out);

// Asks for `source` as prTextureCacheRealize does, and blocks the calling thread until its realization arrives or
// fails.
PrTextureCacheState prTextureCacheWait(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheFlags flags,
    PrTextureRegion* out);

// Blocks until no realization is in flight.
void prTextureCacheWaitAll(PrTextureCache* cache);

// Marks the end of a frame: of the realizations whose sources only the cache holds and that no ask since the last trim
// asked for, evicts the least recently asked for until those left fit the budget, and forgets the failed ones.
void prTextureCacheTrim(PrTextureCache* cache);

// What a cache holds, and has read.
typedef struct PrTextureCacheInfo {
    uint64_t realizations;                      // reads of sources' bits since the cache was created
    uint64_t bytes;                             // of the textures and pages that the cache holds
    uint32_t textures;                          // textures of a source's own
    uint32_t pages;                             // shared pages
    uint32_t slots;                             // sources in the pages
} PrTextureCacheInfo;

// Returns what the cache holds.
PrTextureCacheInfo prTextureCacheInfo(const PrTextureCache* cache);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
