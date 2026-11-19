// The image cache: the gallery's files decoded at full size and as thumbnails.

#include "apps/alchemy/image_cache.h"

#include "pr/array.h"
#include "pr/blob_ref.h"
#include "pr/cache.h"
#include "pr/file.h"
#include "pr/function.h"
#include "pr/image_process.h"
#include "pr/image_texture_source.h"
#include "pr/mem.h"
#include "pr/string.h"

namespace alchemy {

// The context of the cache of one scale.
struct ScaleCache {
    ImageCache* images;
    ImageScale scale;
    PrCache* cache;
};

// A cache of decoded images for each scale, whose loads decode on the pool.
struct ImageCache {
    PrTaskPool* pool;
    const PrImageCodecSet* codec;
    Function<void()> wake;
    ScaleCache scales[2];               // by ImageScale
};

namespace {

// The frames that an image may go unrequested before it is freed: a thumbnail scrolled out of the browser, or an image
// that the viewport no longer shows.
constexpr uint32_t kThumbnailIdleFrames = 600;
constexpr uint32_t kFullIdleFrames = 120;

// A decode on the pool, which finishes its load.
struct Decode {
    const ScaleCache* scale;
    String path;
    PrCacheLoad* load;
};

void runDecode(void* PR_NULLABLE context) {
    Decode* decode = static_cast<Decode*>(context);
    ImageCache* images = decode->scale->images;
    const PrTextureSource* image = nullptr;
    if (BlobRef file = BlobRef::adopt(prFileOpen(decode->path))) {
        image = imageDecode(images->codec, file.bytes().data, file.bytes().size);
        if (image != nullptr && decode->scale->scale == ImageScale_Thumbnail) {
            const PrTextureSource* full = image;
            image = imageShrink(full, kThumbnailMaxEdge);
            prTextureSourceRelease(full);
        }
    }
    prCacheLoadFinish(decode->load, image);
    delete decode;
}

void* startDecode(void* PR_NULLABLE context, PrStringView path, PrCacheLoad* load) {
    const ScaleCache* scale = static_cast<const ScaleCache*>(context);
    prTaskPoolPost(scale->images->pool, runDecode, new Decode{scale, path, load});
    return nullptr;
}

void releaseImage(void* PR_NULLABLE, const void* image) {
    prTextureSourceRelease(static_cast<const PrTextureSource*>(image));
}

void wakeOwner(void* PR_NULLABLE context) {
    ImageCache* images = static_cast<const ScaleCache*>(context)->images;
    if (images->wake)
        images->wake();
}

} // namespace

ImageCache* imageCacheCreate(PrTaskPool* pool, const PrImageCodecSet* codec, Function<void()> wake) {
    ImageCache* images = new ImageCache();
    images->pool = pool;
    images->codec = codec;
    images->wake = move(wake);
    const ImageScale scales[] = {ImageScale_Full, ImageScale_Thumbnail};
    for (ImageScale scale : scales) {
        ScaleCache& entry = images->scales[scale];
        entry.images = images;
        entry.scale = scale;
        PrCacheDesc desc{
            .load = startDecode,
            .release = releaseImage,
            .wake = wakeOwner,
            .context = &entry,
            .idlePumps = scale == ImageScale_Full ? kFullIdleFrames : kThumbnailIdleFrames,
        };
        entry.cache = prCacheCreate(&desc);
    }
    return images;
}

void imageCacheDestroy(ImageCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (ScaleCache& scale : cache->scales)
        prCacheDestroy(scale.cache);
    delete cache;
}

const PrTextureSource* PR_NULLABLE imageCacheGet(ImageCache* cache, PrStringView path, ImageScale scale) {
    return static_cast<const PrTextureSource*>(prCacheValue(cache->scales[scale].cache, path));
}

bool imageCacheFailed(ImageCache* cache, PrStringView path, ImageScale scale) {
    return prCacheKeyState(cache->scales[scale].cache, path) == PrCacheState_Failed;
}

void imageCacheForget(ImageCache* cache, PrStringView path) {
    for (ScaleCache& scale : cache->scales)
        prCacheRemove(scale.cache, path);
}

bool imageCachePump(ImageCache* cache) {
    bool landed = false;
    for (ScaleCache& scale : cache->scales)
        landed = prCachePump(scale.cache) || landed;
    return landed;
}

const PrTextureSource* PR_NULLABLE imageDecode(const PrImageCodecSet* codec, const void* bytes, size_t size) {
    const PrTextureSource* image = nullptr;
    PrByteView view{static_cast<const uint8_t*>(bytes), size};
    if (prImageTextureSourceOpenBytes(view, codec, nullptr, PrImageTextureColor_Srgb, &image) != PrStatus_Ok)
        return nullptr;
    return image;
}

bool imageReadRgba8(const PrTextureSource* image, PrTextureAlpha alpha, PrPixels out) {
    PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba8);
    if (prTextureSourceDesc(image)->alpha != PrTextureAlpha_None)
        convert.alpha = alpha;
    const PrTextureSource* chain = prTextureSourceCreateConvert(image, &convert);
    PrTextureReadDesc first{.levelCount = 1, .layerCount = 1};
    PrTextureRead* read = prTextureReadCreate(chain, &first);
    prTextureReadWait(read);
    bool ok = prTextureReadStatus(read) == PrStatus_Ok;
    if (ok) {
        size_t row = size_t(out.extent.x) * 4;
        for (uint32_t y = 0; y < out.extent.y; ++y)
            prMemCopy(out.data + y * out.stride, prTextureReadStorage(read) + y * row, row);
    }
    prTextureReadDestroy(read);
    prTextureSourceRelease(chain);
    return ok;
}

const PrTextureSource* imageShrink(const PrTextureSource* image, uint32_t maxEdge) {
    const PrTextureDesc& desc = *prTextureSourceDesc(image);
    PrUVec2 extent = desc.extent;
    // An image of floats holds extended sRGB, which a shrunk copy would have to carry, so only 8-bit images shrink.
    PrGpuFormat stored = prGpuFormatPlain(desc.format);
    if (stored != PrGpuFormat_Rgba8 || (extent.x <= maxEdge && extent.y <= maxEdge)) {
        prTextureSourceRetain(image);
        return image;
    }
    uint32_t pixelSize = 4;
    Array<uint8_t> level(size_t(extent.x) * extent.y * pixelSize);
    if (!imageReadRgba8(image, PrTextureAlpha_Premultiplied, PrPixels{level.data(), extent.x * pixelSize, extent})) {
        prTextureSourceRetain(image);
        return image;
    }
    PrPixelView above{level.data(), extent.x * pixelSize, extent};
    while (above.extent.x > maxEdge || above.extent.y > maxEdge) {
        PrUVec2 next{above.extent.x > 1 ? above.extent.x / 2 : 1, above.extent.y > 1 ? above.extent.y / 2 : 1};
        Array<uint8_t> below(size_t(next.x) * next.y * pixelSize);
        prImageMipLevel(above, PrPixels{below.data(), next.x * pixelSize, next}, PrPixelFormat_Rgba8);
        level = move(below);
        above = PrPixelView{level.data(), next.x * pixelSize, next};
    }
    // The thumbnail keeps its pixels, which nothing makes again, and shows as the image's orientation displays it.
    bool opaque = desc.alpha == PrTextureAlpha_None;
    PrTextureDesc shrunk = prTextureDesc2d(PrGpuFormat_Rgba8, above.extent,
        PrTextureAlpha(opaque ? PrTextureAlpha_None : PrTextureAlpha_Premultiplied));
    shrunk.orientation = desc.orientation;
    return prTextureSourceCreatePixels(&shrunk, above);
}

} // namespace alchemy
