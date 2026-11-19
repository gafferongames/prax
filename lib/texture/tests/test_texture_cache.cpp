#include "pr/array.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/task.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A source of one level of 2D texels that counts its reads. A read completes during the call with `status`, or later,
// when the test completes the reads that wait.
struct Texels {
    PrTextureDesc desc{};
    Array<uint8_t> texels;
    uint32_t reads = 0;
    bool deferred = false;
    PrStatus status = PrStatus_Ok;
    Array<PrTextureRead*> waiting;

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        Texels* t = static_cast<Texels*>(context);
        ++t->reads;
        if (t->deferred) {
            t->waiting.push(read);
            return;
        }
        t->complete(read);
    }

    void complete(PrTextureRead* read) {
        if (status == PrStatus_Ok)
            prMemCopy(prTextureReadStorage(read), texels.begin(), texels.count());
        prTextureReadComplete(read, status);
    }

    void completeWaiting() {
        Array<PrTextureRead*> pending = waiting;
        waiting.clear();
        for (PrTextureRead* read : pending)
            complete(read);
    }

    // Returns a source of `extent` texels of `format`, each of the four bytes of `texel`.
    const PrTextureSource* source(PrUVec2 extent, const uint8_t texel[4], PrGpuFormat format = PrGpuFormat_Rgba8,
            PrTextureAlpha alpha = PrTextureAlpha_Premultiplied) {
        desc = prTextureDesc2d(format, extent, alpha);
        texels.clear();
        for (uint32_t i = 0; i < extent.x * extent.y; ++i)
            texels.append(PrByteView{texel, 4});
        PrTextureSourceDesc source{.texture = desc, .read = read, .context = this};
        return prTextureSourceCreate(&source);
    }
};

// Returns a cache of the default description on `device`.
PrTextureCache* defaultCache(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    return prTextureCacheCreate(&desc);
}

// Counts the calls of a cache's `arrived`.
void countArrival(void* PR_NULLABLE context) {
    ++*static_cast<uint32_t*>(context);
}

// Returns the texel at `at` of a memory device's texture.
const uint8_t* texelAt(PrGpuDevice* device, PrGpuTexture texture, PrIVec2 at) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, texture, PrGpuNativeKind_Pixels, &native));
    return native.pixels.data + size_t(at.y) * native.pixels.stride + size_t(at.x) * 4;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture");

TEST_CASE("a cache without an executor realizes a source in the ask that starts it, and reads it once") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    // A cache of no budget keeps what anything else holds, and nothing past it.
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.budget = 0;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    Texels texels;
    const uint8_t red[4] = {255, 0, 0, 255};
    const PrTextureSource* source = texels.source({3, 2}, red);
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    const PrRect whole{0, 0, 3, 2};
    CHECK(prMemEquals(&region.rect, &whole, sizeof(PrRect)));
    CHECK(region.extent == PrUVec2{3, 2});
    CHECK(region.texture.alpha == PrTextureAlpha_Premultiplied);
    CHECK(prMemEquals(texelAt(device, region.texture.texture, {2, 1}), red, 4));
    PrTextureRegion again{};
    CHECK(prTextureCacheRealize(cache, source, 0, &again) == PrTextureCacheState_Ready);
    CHECK(again.texture.texture.id == region.texture.texture.id);
    CHECK(texels.reads == 1);
    // Every level is a texture of its own.
    PrTextureRegion levels{};
    REQUIRE(prTextureCacheRealize(cache, source, PrTextureCacheFlag_Levels, &levels) == PrTextureCacheState_Ready);
    CHECK(levels.texture.texture.id != region.texture.texture.id);
    CHECK(prGpuTextureDesc(device, levels.texture.texture).levels == 2);
    CHECK(texels.reads == 2);
    // The device made the second level from the first.
    PrGpuTexture second{};
    REQUIRE(prGpuTextureCreateView(device, levels.texture.texture, PrGpuTextureKind_2d,
        PrGpuSubresources{.firstLevel = 1, .levels = 1, .layers = 1}, &second) == PrStatus_Ok);
    CHECK(prMemEquals(texelAt(device, second, {0, 0}), red, 4));
    prGpuTextureRelease(device, second);
    // Once every level is realized, an ask for one level draws from it too.
    CHECK(prTextureCacheRealize(cache, source, 0, &again) == PrTextureCacheState_Ready);
    CHECK(again.texture.texture.id == levels.texture.texture.id);
    // Both realizations stay while anything else holds the source, and go once only the cache does and no frame asks.
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 2);
    prTextureSourceRelease(source);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 0);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache's realization that arrives later calls the host's arrived once, and a waiter waits for it") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    uint32_t arrivals = 0;
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.arrived = countArrival;
    desc.arrivedContext = &arrivals;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    Texels texels;
    texels.deferred = true;
    const uint8_t green[4] = {0, 255, 0, 255};
    const PrTextureSource* source = texels.source({2, 2}, green);
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Pending);
    CHECK(region.texture.texture.id == 0);
    CHECK(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Pending);
    CHECK(arrivals == 0);
    texels.completeWaiting();
    CHECK(arrivals == 1);
    CHECK(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    CHECK(texels.reads == 1);

    // On a pool, a waiter blocks until the realization arrives.
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrTextureCacheDesc pooled = prTextureCacheDescDefault(device);
    pooled.tasks = pool;
    PrTextureCache* onPool = prTextureCacheCreate(&pooled);
    texels.deferred = false;
    PrTextureRegion waited{};
    CHECK(prTextureCacheWait(onPool, source, 0, &waited) == PrTextureCacheState_Ready);
    CHECK(prMemEquals(texelAt(device, waited.texture.texture, {1, 1}), green, 4));
    prTextureCacheDestroy(onPool);
    prTaskPoolDestroy(pool);
    prTextureSourceRelease(source);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache keeps a failure while anything holds its source, and does not read it again") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = defaultCache(device);
    Texels texels;
    texels.status = PrStatus_InvalidData;
    const uint8_t blue[4] = {0, 0, 255, 255};
    const PrTextureSource* source = texels.source({2, 2}, blue);
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Failed);
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Failed);
    CHECK(texels.reads == 1);
    prTextureSourceRelease(source);
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 0);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache evicts what only it holds and no frame asked for since the last trim, past its budget, oldest first") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.budget = 16 * 16 * 4;
    desc.pack = PrTextureCachePack_Never;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    const uint8_t white[4] = {255, 255, 255, 255};
    Texels first, second, third;
    const PrTextureSource* sources[3] = {
        first.source({16, 16}, white), second.source({16, 16}, white), third.source({16, 16}, white),
    };
    PrTextureRegion region{};
    for (const PrTextureSource* source : sources)
        REQUIRE(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    // What anything else holds stays, whatever the budget.
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 3);
    // Once only the cache holds them, the first, asked for least recently, is evicted, and the third fits the budget.
    prTextureSourceRelease(sources[0]);
    prTextureSourceRelease(sources[2]);
    prTextureCacheRealize(cache, sources[2], 0, &region);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 3);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 2);
    prTextureCacheRealize(cache, sources[1], 0, &region);
    CHECK(second.reads == 1);
    CHECK(third.reads == 1);
    prTextureSourceRelease(sources[1]);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache packs small sources into a shared page, and frees the page once its slots are evicted") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Always;
    desc.budget = 0;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    Texels small, other, large;
    const PrTextureSource* a = small.source({5, 3}, red);
    const PrTextureSource* b = other.source({7, 3}, green);
    const PrTextureSource* big = large.source({300, 2}, red);
    PrTextureRegion ra{}, rb{}, rbig{};
    REQUIRE(prTextureCacheRealize(cache, a, 0, &ra) == PrTextureCacheState_Ready);
    REQUIRE(prTextureCacheRealize(cache, b, 0, &rb) == PrTextureCacheState_Ready);
    REQUIRE(prTextureCacheRealize(cache, big, 0, &rbig) == PrTextureCacheState_Ready);
    CHECK(ra.texture.texture.id == rb.texture.texture.id);
    CHECK(prRectWidth(ra.rect) == 5);
    CHECK(prRectHeight(rb.rect) == 3);
    CHECK(prRectIsEmpty(prRectIntersect(ra.rect, rb.rect)));
    CHECK(prMemEquals(texelAt(device, ra.texture.texture, {int32_t(ra.rect.left) + 4, int32_t(ra.rect.top) + 2}), red, 4));
    CHECK(prMemEquals(texelAt(device, rb.texture.texture, {int32_t(rb.rect.left), int32_t(rb.rect.top)}), green, 4));
    // A source past a page's largest side has a texture of its own.
    CHECK(rbig.texture.texture.id != ra.texture.texture.id);
    PrTextureCacheInfo stats = prTextureCacheInfo(cache);
    CHECK(stats.pages == 1);
    CHECK(stats.slots == 2);
    CHECK(stats.textures == 1);
    // A slot that is evicted is taken again by the next source of its size.
    prTextureSourceRelease(a);
    prTextureSourceRelease(big);
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).slots == 1);
    CHECK(prTextureCacheInfo(cache).textures == 0);
    Texels reused;
    const PrTextureSource* c = reused.source({5, 3}, green);
    PrTextureRegion rc{};
    REQUIRE(prTextureCacheRealize(cache, c, 0, &rc) == PrTextureCacheState_Ready);
    CHECK(prMemEquals(&rc.rect, &ra.rect, sizeof(PrRect)));
    prTextureSourceRelease(b);
    prTextureSourceRelease(c);
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).pages == 0);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache moved to a new device after a loss realizes every source again") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = defaultCache(device);
    Texels texels;
    const uint8_t red[4] = {255, 0, 0, 255};
    const PrTextureSource* source = texels.source({2, 2}, red);
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    prGpuDeviceSimulateLoss(device);
    PrGpuDevice* replaced = prGpuMemoryCreate(nullptr);
    prTextureCacheSetDevice(cache, replaced);
    prGpuDeviceDestroy(device);
    REQUIRE(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    CHECK(texels.reads == 2);
    CHECK(prMemEquals(texelAt(replaced, region.texture.texture, {1, 1}), red, 4));
    prTextureSourceRelease(source);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(replaced);
}

TEST_CASE("a cache realizes encoded values as a sampler reads them as they are, in the cache's alpha") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.values = PrTextureCacheValues_Encoded;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    // A Srgb form realizes as its plain form, its straight values premultiplied.
    Texels srgb;
    const uint8_t half[4] = {200, 100, 0, 128};
    const PrTextureSource* source = srgb.source({1, 1}, half, PrGpuFormat_Rgba8Srgb, PrTextureAlpha_Straight);
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, source, 0, &region) == PrTextureCacheState_Ready);
    CHECK(prGpuTextureDesc(device, region.texture.texture).format == PrGpuFormat_Rgba8);
    CHECK(region.texture.alpha == PrTextureAlpha_Premultiplied);
    const uint8_t* texel = texelAt(device, region.texture.texture, {0, 0});
    CHECK(texel[0] == 100);
    CHECK(texel[1] == 50);
    CHECK(texel[3] == 128);
    prTextureSourceRelease(source);
    // Floats are encoded as extended sRGB in half floats.
    Texels floats;
    float linear[4] = {0.2140f, -0.2140f, 1, 1};
    floats.desc = prTextureDesc2d(PrGpuFormat_Rgba32F, PrUVec2{1, 1}, PrTextureAlpha_None);
    floats.texels.append(PrByteView{reinterpret_cast<const uint8_t*>(linear), sizeof(linear)});
    PrTextureSourceDesc wide{.texture = floats.desc, .read = Texels::read, .context = &floats};
    const PrTextureSource* wideSource = prTextureSourceCreate(&wide);
    REQUIRE(prTextureCacheRealize(cache, wideSource, 0, &region) == PrTextureCacheState_Ready);
    CHECK(prGpuTextureDesc(device, region.texture.texture).format == PrGpuFormat_Rgba16F);
    CHECK(region.texture.alpha == PrTextureAlpha_None);
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, region.texture.texture, PrGpuNativeKind_Pixels, &native));
    PrFloat16 stored[4];
    prMemCopy(stored, native.pixels.data, sizeof(stored));
    CHECK(prFloat16ToFloat(stored[0]) == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(prFloat16ToFloat(stored[1]) == doctest::Approx(-0.5f).epsilon(0.01));
    prTextureSourceRelease(wideSource);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
