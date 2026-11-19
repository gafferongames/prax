// The texture cache: sources realized on one device, as textures of their own or slots of shared pages.

#include "pr/texture_cache.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

namespace pr_impl {

constexpr uint64_t kDefaultBudget = uint64_t(64) << 20;

// The side of the first shared page of a format, and of any page at most: each page of a format that has pages already
// is twice the side of the last, so that a few small images take a small page. And the largest side of a source that a
// page holds.
constexpr uint32_t kFirstPageSize = 512;
constexpr uint32_t kPageSize = 2048;
constexpr uint32_t kPackedMaxSide = 256;

// Slots start and end on multiples of this, which keeps a page's columns and shelves few.
constexpr uint32_t kSlotAlign = 4;

// A free run of a shelf's columns.
struct CacheSpan {
    uint32_t x;
    uint32_t width;
};

// A row of a page whose slots share a height.
struct CacheShelf {
    uint32_t y;
    uint32_t height;
    RawArray<CacheSpan> free;
};

// A page that small sources of one format and alpha share.
struct CachePage {
    PrGpuTexture texture;
    PrGpuFormat format;
    PrTextureAlpha alpha;
    uint32_t size;
    uint32_t nextY;                 // the top of the first row that no shelf has taken
    uint32_t slots;
    RawArray<CacheShelf> shelves;
};

// A source's realization, by its id and flags.
struct CacheEntry {
    uint64_t key;
    PrTextureCacheState state;
    bool asked;                     // an ask found it pending
    uint64_t lastAsked;             // the trim count when an ask last asked for it
    const PrTextureSource* source;  // which the entry holds a reference to
    uint64_t bytes;                 // of its texture, or of its slot
    PrTextureRegion region;
    uint32_t page;                  // its page's index plus one, or zero for a texture of its own
    uint32_t shelf;
    bool evicted;                   // a trim removes it
};

struct TextureCache;

// A realization in flight: the chain that it reads, and where it lands.
struct CacheJob {
    TextureCache* cache;
    uint64_t key;
    const PrTextureSource* chain;
    PrTextureUploadFlags uploadFlags;
    bool packed;
    PrGpuTexture page;              // packed: the page's texture, and the slot's origin in it
    PrIVec2 origin;
};

struct TextureCache {
    PrTextureCacheDesc desc;
    PrMutex lock;
    PrCondition settled;            // signaled as a realization leaves flight
    RawArray<CacheEntry> entries;
    KeyIndex index;
    RawArray<CachePage*> pages;
    uint32_t inFlight;
    uint64_t trims;
    uint64_t generation;            // the moves to another device
    uint64_t realizations;
};

namespace {

uint64_t keyOf(const PrTextureSource* source, PrTextureCacheFlags flags) {
    return prTextureSourceId(source) * 2 + ((flags & PrTextureCacheFlag_Levels) != 0 ? 1 : 0);
}

// Returns the key of the other realization of the same source: with every level for one without, and the reverse.
uint64_t siblingOf(uint64_t key) {
    return key ^ 1;
}

// True if `format`, or the plain format that it is a swizzled form of, holds floats.
bool holdsFloats(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
        case PrGpuFormat_Rg11B10F:
        case PrGpuFormat_Bc6HUfloat:
        case PrGpuFormat_Bc6HSfloat:
            return true;
        default:
            return false;
    }
}

// True if lib/texture's conversions read texels of `format`: a plain format of 8 bits or floats, or a swizzled form.
bool isPixels(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R8:
        case PrGpuFormat_Rg8:
        case PrGpuFormat_Rgba8:
        case PrGpuFormat_Bgra8:
        case PrGpuFormat_R16F:
        case PrGpuFormat_Rg16F:
        case PrGpuFormat_Rgba16F:
        case PrGpuFormat_R32F:
        case PrGpuFormat_Rg32F:
        case PrGpuFormat_Rgba32F:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoded floats

// Returns `value` encoded with sRGB's transfer function, mirrored below zero, as extended sRGB encodes it.
float encodeExtended(float value) {
    float encoded = prColorTransferEncode(PrColorTransfer_Srgb, prF32Abs(value));
    return value < 0 ? -encoded : encoded;
}

// A source of another's floats encoded as extended sRGB in half floats. The other is read as straight Rgba32F, a
// conversion of the source.
struct EncodedSource {
    const PrTextureSource* inner;
    PrTextureDesc desc;
};

struct EncodedRead {
    const EncodedSource* source;
    PrTextureRead* outer;
};

void encodedDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    EncodedRead held = *static_cast<EncodedRead*>(context);
    prMemoryFree(context);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureDesc& desc = held.source->desc;
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(held.source->inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(held.outer);
    bool opaque = desc.alpha == PrTextureAlpha_None;
    bool premultiplied = desc.alpha == PrTextureAlpha_Premultiplied;
    for (uint32_t level = read.firstLevel; status == PrStatus_Ok && level < read.firstLevel + read.levelCount; ++level) {
        PrUVec2 extent = prTextureLevelExtent(desc.extent, level);
        size_t count = size_t(extent.x) * extent.y;
        for (uint32_t layer = read.firstLayer; layer < read.firstLayer + read.layerCount; ++layer) {
            const float* from = reinterpret_cast<const float*>(
                prTextureReadStorage(inner) + prTextureReadOffset(&innerDesc, &read, level, layer));
            uint8_t* to = prTextureReadStorage(held.outer) + prTextureReadOffset(&desc, &read, level, layer);
            for (size_t i = 0; i < count; ++i) {
                float c[4];
                prMemCopy(c, from + i * 4, sizeof(c));
                float a = opaque ? 1 : prF32Clamp(c[3], 0, 1);
                float scale = premultiplied ? a : 1;
                PrFloat16 stored[4];
                for (uint32_t k = 0; k < 3; ++k)
                    stored[k] = prFloat16FromFloat(encodeExtended(c[k]) * scale);
                stored[3] = prFloat16FromFloat(a);
                prMemCopy(to + i * 8, stored, sizeof(stored));
            }
        }
    }
    prTextureReadDestroy(inner);
    prTextureReadComplete(held.outer, status);
}

void encodedRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const EncodedSource* source = static_cast<const EncodedSource*>(context);
    EncodedRead* held = static_cast<EncodedRead*>(prMemoryAlloc(sizeof(EncodedRead)));
    *held = EncodedRead{source, read};
    PrTextureReadDesc inner = *prTextureReadInfo(read);
    inner.storage = nullptr;
    inner.done = encodedDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

// Creates a source of `floats`' values as encoded extended sRGB in half floats of `alpha`, or opaque if they are.
const PrTextureSource* createEncoded(const PrTextureSource* floats, PrTextureAlpha alpha) {
    PrTextureConvertDesc straight = prTextureConvertDescDefault(PrGpuFormat_Rgba32F);
    if (prTextureSourceDesc(floats)->alpha != PrTextureAlpha_None)
        straight.alpha = PrTextureAlpha_Straight;
    EncodedSource* source = allocZeroed<EncodedSource>();
    source->inner = prTextureSourceCreateConvert(floats, &straight);
    source->desc = *prTextureSourceDesc(source->inner);
    source->desc.format = PrGpuFormat_Rgba16F;
    if (source->desc.alpha != PrTextureAlpha_None)
        source->desc.alpha = alpha;
    PrTextureSourceDesc desc{
        .texture = source->desc,
        .read = encodedRead,
        .release = [](void* PR_NULLABLE context) {
            EncodedSource* held = static_cast<EncodedSource*>(context);
            prTextureSourceRelease(held->inner);
            prMemoryFree(held);
        },
        .context = source,
    };
    return prTextureSourceCreate(&desc);
}

// ---------------------------------------------------------------------------------------------------------------------
// Chains

// Returns a source of `blocks`' blocks that the device samples as the cache's values say, or null if it does not.
const PrTextureSource* PR_NULLABLE blocksChain(const TextureCache& c, const PrTextureSource* source) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    PrGpuFormat formats[kPrTextureSourceMaxFormats + 1];
    formats[0] = desc.format;
    uint32_t count = 1 + prTextureSourceFormats(source, formats + 1, kPrTextureSourceMaxFormats);
    if (count > kPrTextureSourceMaxFormats + 1)
        count = kPrTextureSourceMaxFormats + 1;
    bool encoded = c.desc.values == PrTextureCacheValues_Encoded;
    for (uint32_t i = 0; i < count; ++i) {
        PrGpuFormat format = formats[i];
        PrGpuFormat sampled = encoded ? prGpuFormatPlain(format) : format;
        if (!prGpuFormatIsCompressed(format) || (encoded && holdsFloats(format)) ||
                !prGpuDeviceSupportsFormat(c.desc.device, sampled, PrGpuTextureUsageFlag_Sampled))
            continue;
        const PrTextureSource* blocks = nullptr;
        if (prTextureSourceReformat(source, format, &blocks) != PrStatus_Ok)
            continue;
        // Blocks keep their alpha, unless the cache's pixels are straight and theirs are not.
        PrTextureAlpha alpha = prTextureSourceDesc(blocks)->alpha;
        if (alpha == PrTextureAlpha_Premultiplied && c.desc.alpha == PrTextureAlpha_Straight) {
            prTextureSourceRelease(blocks);
            continue;
        }
        if (sampled == format)
            return blocks;
        PrTextureConvertDesc relabel = prTextureConvertDescDefault(sampled);
        relabel.alpha = alpha;
        const PrTextureSource* chain = prTextureSourceCreateConvert(blocks, &relabel);
        prTextureSourceRelease(blocks);
        return chain;
    }
    return nullptr;
}

// Returns a source of `source`'s pixels as the cache realizes them: Rgba8, or its Srgb form for sampled values of it,
// for values of 8 bits, and Rgba16F for floats, in the cache's alpha. Returns null for a source that no conversion
// reads.
const PrTextureSource* PR_NULLABLE pixelsChain(const TextureCache& c, const PrTextureSource* source) {
    const PrTextureSource* pixels = nullptr;
    if (isPixels(prTextureSourceDesc(source)->format)) {
        prTextureSourceRetain(source);
        pixels = source;
    } else {
        const PrGpuFormat decoded[] = {
            PrGpuFormat_Rgba8, PrGpuFormat_Rgba8Srgb, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F,
        };
        for (PrGpuFormat candidate : decoded) {
            if (prTextureSourceProduces(source, candidate) &&
                    prTextureSourceReformat(source, candidate, &pixels) == PrStatus_Ok)
                break;
        }
        if (pixels == nullptr)
            return nullptr;
    }
    const PrTextureDesc& desc = *prTextureSourceDesc(pixels);
    PrTextureAlpha alpha = desc.alpha == PrTextureAlpha_None ? PrTextureAlpha(PrTextureAlpha_None) : c.desc.alpha;
    bool encoded = c.desc.values == PrTextureCacheValues_Encoded;
    const PrTextureSource* chain = nullptr;
    if (holdsFloats(desc.format) && encoded) {
        chain = createEncoded(pixels, alpha);
    } else if (holdsFloats(desc.format)) {
        PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba16F);
        convert.alpha = alpha;
        chain = prTextureSourceCreateConvert(pixels, &convert);
    } else {
        // Values of 8 bits are encoded already: a Srgb form reads as its plain form, so that nothing decodes them.
        const PrTextureSource* plain = pixels;
        prTextureSourceRetain(plain);
        PrGpuFormat target = PrGpuFormat_Rgba8;
        if (prGpuFormatIsSrgb(desc.format) && encoded) {
            prTextureSourceRelease(plain);
            PrTextureConvertDesc relabel = prTextureConvertDescDefault(prGpuFormatPlain(desc.format));
            relabel.alpha = desc.alpha;
            plain = prTextureSourceCreateConvert(pixels, &relabel);
        } else if (prGpuFormatIsSrgb(desc.format)) {
            target = PrGpuFormat_Rgba8Srgb;
        }
        PrTextureConvertDesc convert = prTextureConvertDescDefault(target);
        convert.alpha = alpha;
        chain = prTextureSourceCreateConvert(plain, &convert);
        prTextureSourceRelease(plain);
    }
    prTextureSourceRelease(pixels);
    return chain;
}

// Returns true if the cache packs sources on its device by default: on any device that draws through a GPU, on either
// binding tier, as a texture of its own costs a renderer a bind, and a draw where the renderer batches by texture. A
// memory device reads texels in place, so a page would only add a copy.
bool packsByDefault(const TextureCache& c) {
    return !prGpuDeviceHasNative(c.desc.device, PrGpuNativeKind_Pixels);
}

// Returns true if a page holds `chain`'s pixels for an ask with `flags`.
bool packs(const TextureCache& c, const PrTextureSource* chain, PrTextureCacheFlags flags) {
    if (c.desc.pack == PrTextureCachePack_Never || (flags & PrTextureCacheFlag_Levels) != 0)
        return false;
    if (c.desc.pack == PrTextureCachePack_Auto && !packsByDefault(c))
        return false;
    const PrTextureDesc& desc = *prTextureSourceDesc(chain);
    return desc.kind == PrGpuTextureKind_2d && desc.layerCount == 1 && desc.levelCount == 1 &&
        (desc.format == PrGpuFormat_Rgba8 || desc.format == PrGpuFormat_Rgba8Srgb) && desc.extent.x != 0 &&
        desc.extent.y != 0 && desc.extent.x <= kPackedMaxSide && desc.extent.y <= kPackedMaxSide;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pages

// Returns the height of the shelves that hold a slot `height` texels tall: the next power of two from kSlotAlign * 2.
uint32_t shelfHeightOf(uint32_t height) {
    uint32_t shelf = kSlotAlign * 2;
    while (shelf < height)
        shelf *= 2;
    return shelf;
}

// Takes `width` columns of a shelf's free runs. Returns false if no run is wide enough.
bool takeColumns(CacheShelf& shelf, uint32_t width, uint32_t* x) {
    for (uint32_t i = 0; i < shelf.free.count; ++i) {
        CacheSpan& span = shelf.free.data[i];
        if (span.width < width)
            continue;
        *x = span.x;
        span.x += width;
        span.width -= width;
        if (span.width == 0)
            shelf.free.erase(i);
        return true;
    }
    return false;
}

// Gives a slot's columns back to its shelf, merged with the runs beside them.
void freeColumns(CacheShelf& shelf, uint32_t x, uint32_t width) {
    uint32_t at = 0;
    while (at < shelf.free.count && shelf.free.data[at].x < x)
        ++at;
    shelf.free.insert(at, CacheSpan{x, width});
    if (at + 1 < shelf.free.count && shelf.free.data[at].x + shelf.free.data[at].width == shelf.free.data[at + 1].x) {
        shelf.free.data[at].width += shelf.free.data[at + 1].width;
        shelf.free.erase(at + 1);
    }
    if (at > 0 && shelf.free.data[at - 1].x + shelf.free.data[at - 1].width == shelf.free.data[at].x) {
        shelf.free.data[at - 1].width += shelf.free.data[at].width;
        shelf.free.erase(at);
    }
}

// Places a slot of `extent` in the page. Returns false if it has no room.
bool placeIn(CachePage& page, PrUVec2 extent, uint32_t* shelfAt, PrIVec2* origin) {
    uint32_t width = alignUp(extent.x, kSlotAlign);
    uint32_t height = shelfHeightOf(extent.y);
    uint32_t x = 0;
    for (uint32_t i = 0; i < page.shelves.count; ++i) {
        CacheShelf& shelf = page.shelves.data[i];
        if (shelf.height == height && takeColumns(shelf, width, &x)) {
            *shelfAt = i;
            *origin = PrIVec2{int32_t(x), int32_t(shelf.y)};
            return true;
        }
    }
    if (page.nextY + height > page.size)
        return false;
    CacheShelf shelf{.y = page.nextY, .height = height};
    shelf.free.push(CacheSpan{0, page.size});
    page.nextY += height;
    page.shelves.push(shelf);
    CacheShelf& added = page.shelves.data[page.shelves.count - 1];
    takeColumns(added, width, &x);
    *shelfAt = page.shelves.count - 1;
    *origin = PrIVec2{int32_t(x), int32_t(added.y)};
    return true;
}

void destroyPage(TextureCache& c, CachePage* page) {
    prGpuTextureRelease(c.desc.device, page->texture);
    for (CacheShelf& shelf : page->shelves.span())
        shelf.free.reset();
    page->shelves.reset();
    prMemoryFree(page);
}

// Places a slot of `extent` in a page of `format` and `alpha`, which it creates if none has room. Returns false if a
// page cannot be created.
bool placeSlot(TextureCache& c, PrGpuFormat format, PrTextureAlpha alpha, PrUVec2 extent, CacheEntry& entry, PrIVec2* origin) {
    for (uint32_t i = 0; i < c.pages.count; ++i) {
        CachePage& page = *c.pages.data[i];
        if (page.format == format && page.alpha == alpha && placeIn(page, extent, &entry.shelf, origin)) {
            entry.page = i + 1;
            ++page.slots;
            return true;
        }
    }
    uint32_t size = kFirstPageSize;
    for (const CachePage* page : c.pages.span()) {
        if (page != nullptr && page->format == format && page->alpha == alpha && page->size * 2 > size)
            size = page->size * 2;
    }
    if (size > kPageSize)
        size = kPageSize;
    uint32_t most = prGpuDeviceInfo(c.desc.device).maxTextureSize;
    if (most != 0 && most < size)
        size = most;
    PrGpuTextureDesc desc = prGpuTextureDesc2d(format, PrUVec2{size, size}, PrGpuTextureUsageFlag_Sampled);
    desc.name = "texture cache page";
    PrGpuTexture texture{};
    if (prGpuTextureCreate(c.desc.device, &desc, &texture) != PrStatus_Ok)
        return false;
    CachePage* page = allocZeroed<CachePage>();
    *page = CachePage{.texture = texture, .format = format, .alpha = alpha, .size = size};
    // A page in the slot of a freed one keeps the indices of the entries in the others.
    uint32_t at = 0;
    while (at < c.pages.count && c.pages.data[at] != nullptr)
        ++at;
    if (at == c.pages.count)
        c.pages.push(nullptr);
    c.pages.data[at] = page;
    if (!placeIn(*page, extent, &entry.shelf, origin))
        return false;
    entry.page = at + 1;
    ++page->slots;
    return true;
}

// Frees what the entry holds: its texture, or its slot, and its page once the page holds nothing.
void freeEntry(TextureCache& c, CacheEntry& entry) {
    if (entry.page != 0) {
        CachePage* page = c.pages.data[entry.page - 1];
        PrRect r = entry.region.rect;
        freeColumns(page->shelves.data[entry.shelf], uint32_t(r.left), alignUp(uint32_t(r.right - r.left), kSlotAlign));
        if (--page->slots == 0) {
            destroyPage(c, page);
            c.pages.data[entry.page - 1] = nullptr;
        }
    } else if (entry.region.texture.texture.id != 0) {
        prGpuTextureRelease(c.desc.device, entry.region.texture.texture);
    }
    entry.page = 0;
    entry.region = PrTextureRegion{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Realizations

CacheEntry* PR_NULLABLE entryOf(TextureCache& c, uint64_t key) {
    uint32_t at = c.index.find(key, [&](uint32_t i) { return c.entries.data[i].key; });
    return at != kHashMissing ? &c.entries.data[at] : nullptr;
}

// Completes a realization: the entry takes what it made, and the host learns that it arrived.
void finish(CacheJob* job, PrStatus status, PrTexture texture) {
    TextureCache& c = *job->cache;
    prMutexLock(&c.lock);
    CacheEntry* entry = entryOf(c, job->key);
    PR_ASSERT(entry != nullptr && entry->state == PrTextureCacheState_Pending);
    bool asked = entry->asked;
    if (status == PrStatus_Ok) {
        entry->state = PrTextureCacheState_Ready;
        if (!job->packed) {
            PrUVec2 extent = prGpuTextureExtent(c.desc.device, texture.texture);
            const PrTextureDesc& desc = *prTextureSourceDesc(job->chain);
            entry->region = PrTextureRegion{
                .texture = texture,
                .extent = extent,
                .rect = PrRect{0, 0, float(desc.extent.x), float(desc.extent.y)},
            };
            entry->bytes = prTextureUploadSize(c.desc.device, &desc, job->uploadFlags);
        }
    } else {
        if (texture.texture.id != 0)
            prGpuTextureRelease(c.desc.device, texture.texture);
        freeEntry(c, *entry);
        entry->state = PrTextureCacheState_Failed;
        entry->bytes = 0;
    }
    entry->asked = false;
    prTextureSourceRelease(job->chain);
    prMemoryFree(job);
    // The callback runs before the realization leaves flight, so that it is done before the cache can be destroyed.
    if (asked && c.desc.arrived != nullptr) {
        prMutexUnlock(&c.lock);
        c.desc.arrived(c.desc.arrivedContext);
        prMutexLock(&c.lock);
    }
    --c.inFlight;
    prConditionBroadcast(&c.settled);
    prMutexUnlock(&c.lock);
}

// Reads a packed source's pixels and writes them into its slot.
void packedDone(void* PR_NULLABLE context, PrTextureRead* read) {
    CacheJob* job = static_cast<CacheJob*>(context);
    PrStatus status = prTextureReadStatus(read);
    if (status == PrStatus_Ok) {
        PrUVec2 extent = prTextureSourceDesc(job->chain)->extent;
        prGpuTextureWrite(job->cache->desc.device, job->page, job->origin,
            PrPixelView{prTextureReadStorage(read), size_t(extent.x) * 4, extent});
        if (prGpuDeviceIsLost(job->cache->desc.device))
            status = PrStatus_Lost;
    }
    prTextureReadDestroy(read);
    finish(job, status, PrTexture{});
}

void run(void* PR_NULLABLE context) {
    CacheJob* job = static_cast<CacheJob*>(context);
    if (job->packed) {
        PrTextureReadDesc read{.levelCount = 1, .layerCount = 1, .done = packedDone, .context = job};
        prTextureReadCreate(job->chain, &read);
        return;
    }
    prTextureUpload(job->cache->desc.device, job->chain, job->uploadFlags, "texture cache",
        [](void* PR_NULLABLE held, PrStatus status, PrTexture texture) {
            finish(static_cast<CacheJob*>(held), status, texture);
        }, job);
}

// Makes the chain that realizes `source` for an ask with `flags`: its blocks where the device samples them, and its
// pixels otherwise.
const PrTextureSource* PR_NULLABLE chainOf(const TextureCache& c, const PrTextureSource* source) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    if (desc.kind != PrGpuTextureKind_2d) {
        prTextureSourceRetain(source);
        return source;
    }
    const PrTextureSource* chain = blocksChain(c, source);
    return chain != nullptr ? chain : pixelsChain(c, source);
}

// Starts the realization of the entry at `key`, which is pending. Called with the lock held, which it releases.
void start(TextureCache& c, uint64_t key, const PrTextureSource* source, PrTextureCacheFlags flags) {
    const PrTextureSource* chain = chainOf(c, source);
    CacheEntry* entry = entryOf(c, key);
    if (chain == nullptr) {
        entry->state = PrTextureCacheState_Failed;
        prMutexUnlock(&c.lock);
        return;
    }
    CacheJob* job = allocZeroed<CacheJob>();
    *job = CacheJob{.cache = &c, .key = key, .chain = chain};
    const PrTextureDesc& desc = *prTextureSourceDesc(chain);
    if (packs(c, chain, flags)) {
        PrIVec2 origin{};
        if (placeSlot(c, desc.format, desc.alpha == PrTextureAlpha_None ? c.desc.alpha : desc.alpha, desc.extent,
                *entry, &origin)) {
            const CachePage& page = *c.pages.data[entry->page - 1];
            job->packed = true;
            job->page = page.texture;
            job->origin = origin;
            entry->region = PrTextureRegion{
                .texture = PrTexture{.texture = page.texture, .alpha = page.alpha},
                .extent = PrUVec2{page.size, page.size},
                .rect = PrRect{
                    float(origin.x), float(origin.y), float(origin.x + int32_t(desc.extent.x)),
                    float(origin.y + int32_t(desc.extent.y)),
                },
            };
            entry->bytes = uint64_t(desc.extent.x) * desc.extent.y * 4;
        }
    }
    if (!job->packed && (flags & PrTextureCacheFlag_Levels) != 0 && desc.levelCount == 1)
        job->uploadFlags = PrTextureUploadFlag_MakesLevels;
    ++c.inFlight;
    ++c.realizations;
    PrTaskPool* tasks = c.desc.tasks;
    prMutexUnlock(&c.lock);
    if (tasks != nullptr)
        prTaskPoolPost(tasks, run, job);
    else
        run(job);
}

// Rebuilds the index after entries were removed.
void reindex(TextureCache& c) {
    c.index.rebuild(c.entries.count, [&](uint32_t i) { return c.entries.data[i].key; });
}

void waitAll(TextureCache& c) {
    prMutexLock(&c.lock);
    while (c.inFlight != 0)
        prConditionWait(&c.settled, &c.lock);
    prMutexUnlock(&c.lock);
}

void freeAll(TextureCache& c) {
    for (CacheEntry& entry : c.entries.span()) {
        if (entry.page == 0 && entry.region.texture.texture.id != 0)
            prGpuTextureRelease(c.desc.device, entry.region.texture.texture);
        prTextureSourceRelease(entry.source);
    }
    for (CachePage* page : c.pages.span()) {
        if (page != nullptr)
            destroyPage(c, page);
    }
    c.pages.clear();
    c.entries.clear();
    c.index.clear();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextureCache : TextureCache {};

PrTextureCacheDesc prTextureCacheDescDefault(PrGpuDevice* device) {
    return PrTextureCacheDesc{
        .device = device,
        .budget = kDefaultBudget,
        .values = PrTextureCacheValues_Sampled,
        .alpha = PrTextureAlpha_Premultiplied,
        .pack = PrTextureCachePack_Auto,
    };
}

PrTextureCache* prTextureCacheCreate(const PrTextureCacheDesc* desc) {
    PR_ASSERT(desc->alpha == PrTextureAlpha_Straight || desc->alpha == PrTextureAlpha_Premultiplied);
    PrTextureCache* cache = allocZeroed<PrTextureCache>();
    cache->desc = *desc;
    prMutexInit(&cache->lock);
    prConditionInit(&cache->settled);
    return cache;
}

void prTextureCacheDestroy(PrTextureCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    waitAll(*cache);
    freeAll(*cache);
    cache->entries.reset();
    cache->index.reset();
    cache->pages.reset();
    prMutexReset(&cache->lock);
    prConditionReset(&cache->settled);
    prMemoryFree(cache);
}

PrGpuDevice* prTextureCacheDevice(const PrTextureCache* cache) {
    return cache->desc.device;
}

void prTextureCacheSetDevice(PrTextureCache* cache, PrGpuDevice* device) {
    waitAll(*cache);
    prMutexLock(&cache->lock);
    freeAll(*cache);
    cache->desc.device = device;
    ++cache->generation;
    prMutexUnlock(&cache->lock);
}

uint64_t prTextureCacheGeneration(const PrTextureCache* cache) {
    return cache->generation;
}

PrTextureCacheState prTextureCacheRealize(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheFlags flags,
        PrTextureRegion* out) {
    *out = PrTextureRegion{};
    uint64_t key = keyOf(source, flags);
    prMutexLock(&cache->lock);
    // A source realized with every level draws from that realization where one level would do, so that one texture of
    // it serves both.
    if ((flags & PrTextureCacheFlag_Levels) == 0) {
        CacheEntry* levels = entryOf(*cache, siblingOf(key));
        if (levels != nullptr && levels->state == PrTextureCacheState_Ready) {
            levels->lastAsked = cache->trims;
            *out = levels->region;
            prMutexUnlock(&cache->lock);
            return PrTextureCacheState_Ready;
        }
    }
    CacheEntry* entry = entryOf(*cache, key);
    if (entry == nullptr) {
        cache->index.insert(key, cache->entries.count);
        prTextureSourceRetain(source);
        cache->entries.push(CacheEntry{
            .key = key,
            .state = PrTextureCacheState_Pending,
            .lastAsked = cache->trims,
            .source = source,
        });
        start(*cache, key, source, flags);
        prMutexLock(&cache->lock);
        entry = entryOf(*cache, key);
    }
    entry->lastAsked = cache->trims;
    PrTextureCacheState state = entry->state;
    if (state == PrTextureCacheState_Pending)
        entry->asked = true;
    else if (state == PrTextureCacheState_Ready)
        *out = entry->region;
    prMutexUnlock(&cache->lock);
    return state;
}

PrTextureCacheState prTextureCacheWait(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheFlags flags,
        PrTextureRegion* out) {
    PrTextureCacheState state = prTextureCacheRealize(cache, source, flags, out);
    if (state != PrTextureCacheState_Pending)
        return state;
    uint64_t key = keyOf(source, flags);
    prMutexLock(&cache->lock);
    CacheEntry* entry = entryOf(*cache, key);
    while (entry->state == PrTextureCacheState_Pending) {
        prConditionWait(&cache->settled, &cache->lock);
        entry = entryOf(*cache, key);
    }
    state = entry->state;
    if (state == PrTextureCacheState_Ready)
        *out = entry->region;
    prMutexUnlock(&cache->lock);
    return state;
}

void prTextureCacheWaitAll(PrTextureCache* cache) {
    waitAll(*cache);
}

void prTextureCacheTrim(PrTextureCache* cache) {
    prMutexLock(&cache->lock);
    TextureCache& c = *cache;
    uint64_t current = c.trims;
    // The entries that no ask since the last trim asked for, least recently asked for first.
    RawArray<uint32_t> idle;
    uint64_t idleBytes = 0;
    bool removed = false;
    for (uint32_t i = 0; i < c.entries.count; ++i) {
        const CacheEntry& entry = c.entries.data[i];
        // What anything else holds the source of stays, as drawings may name its region. The cache holds the source
        // once for each realization of it.
        uint32_t held = entryOf(c, siblingOf(entry.key)) != nullptr ? 2 : 1;
        if (entry.lastAsked == current || entry.state == PrTextureCacheState_Pending ||
                prTextureSourceReferences(entry.source) > held)
            continue;
        idle.push(i);
        idleBytes += entry.bytes;
    }
    for (uint32_t i = 1; i < idle.count; ++i) {
        uint32_t moved = idle.data[i];
        uint32_t k = i;
        for (; k > 0 && c.entries.data[idle.data[k - 1]].lastAsked > c.entries.data[moved].lastAsked; --k)
            idle.data[k] = idle.data[k - 1];
        idle.data[k] = moved;
    }
    for (uint32_t at : idle.span()) {
        CacheEntry& entry = c.entries.data[at];
        if (entry.state == PrTextureCacheState_Failed) {
            prTextureSourceRelease(entry.source);
            entry.evicted = true;
            removed = true;
            continue;
        }
        if (idleBytes <= c.desc.budget)
            continue;
        idleBytes -= entry.bytes;
        freeEntry(c, entry);
        prTextureSourceRelease(entry.source);
        entry.evicted = true;
        removed = true;
    }
    idle.reset();
    if (removed) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < c.entries.count; ++i) {
            if (!c.entries.data[i].evicted)
                c.entries.data[kept++] = c.entries.data[i];
        }
        c.entries.count = kept;
        reindex(c);
    }
    ++c.trims;
    prMutexUnlock(&cache->lock);
}

PrTextureCacheInfo prTextureCacheInfo(const PrTextureCache* cache) {
    PrTextureCacheInfo info{.realizations = cache->realizations};
    PrMutex* lock = const_cast<PrMutex*>(&cache->lock);
    prMutexLock(lock);
    for (const CacheEntry& entry : cache->entries.span()) {
        if (entry.state != PrTextureCacheState_Ready)
            continue;
        if (entry.page != 0) {
            ++info.slots;
        } else {
            ++info.textures;
            info.bytes += entry.bytes;
        }
    }
    for (const CachePage* page : cache->pages.span()) {
        if (page != nullptr) {
            ++info.pages;
            info.bytes += uint64_t(page->size) * page->size * 4;
        }
    }
    prMutexUnlock(lock);
    return info;
}
