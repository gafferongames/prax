// The loader: pictures as a catalog's sources, the previews that a texture cache realizes of them, failures, and
// pictures that are forgotten.

#include "pr/array.h"
#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/gpu_select.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/string.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "apps/pandite/loader.h"
#include "tools/harness/texture_await.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace pandite;
using namespace pr;

namespace {

// What a test's loader opens files with: the codecs, the pool, the formats and the catalog.
struct Harness {
    PrCompressCodecSet* compress;
    PrImageCodecSet* images;
    PrTaskPool* pool;
    PrTextureFormatSet* formats;
    PrTextureCatalog* catalog;
};

Harness harnessCreate() {
    Harness h{};
    h.compress = prCompressCodecSetCreateDefault();
    h.images = prImageCodecSetCreateDefault(h.compress);
    h.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 2);
    PrTextureFormat* formats[] = {prImageTextureFormatCreate(h.images, PrImageTextureColor_Srgb)};
    h.formats = prTextureFormatSetCreate(formats, 1);
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(h.formats);
    desc.tasks = h.pool;
    h.catalog = prTextureCatalogCreate(&desc);
    return h;
}

void harnessDestroy(Harness& h) {
    prTextureCatalogDestroy(h.catalog);
    prTextureFormatSetDestroy(h.formats);
    prTaskPoolDestroy(h.pool);
    prImageCodecSetDestroy(h.images);
    prCompressCodecSetDestroy(h.compress);
}

String pathOf(PrStringView name) {
    return format("{}/loader/{}", PANDITE_TEST_DIR, name);
}

// Writes a PNG at `path`: a gradient of `width` by `height` pixels.
void writePng(const Harness& h, PrStringView path, uint32_t width, uint32_t height) {
    Array<uint8_t> pixels(size_t(width) * height * 4);
    for (uint32_t i = 0; i < width * height; ++i) {
        pixels[i * 4] = uint8_t(i % width);
        pixels[i * 4 + 1] = uint8_t(i / width);
        pixels[i * 4 + 2] = 128;
        pixels[i * 4 + 3] = 255;
    }
    PrPixelView view{pixels.data(), size_t(width) * 4, {width, height}};
    size_t size = 0;
    REQUIRE(prImageCodecSetEncode(h.images, "image/png", view, nullptr, 0, &size) == PrStatus_Ok);
    Array<uint8_t> encoded(size);
    REQUIRE(prImageCodecSetEncode(h.images, "image/png", view, encoded.data(), size, &size) == PrStatus_Ok);
    REQUIRE(prFileWrite(path, PrByteView{encoded.data(), size}));
}

// Runs frames that want `path` until the file opens or fails, or until five seconds pass. Returns the picture of the
// last frame. The picture stays valid until the next frame of the loader.
const Picture& wantUntilOpen(Loader* loader, PrStringView path) {
    PrTimePerf start = prTimePerfNow();
    for (;;) {
        loaderBegin(loader, PrUVec2{1920, 1080}, nullptr);
        const Picture& picture = loaderWant(loader, path);
        loaderEnd(loader);
        if (picture.source != nullptr || picture.failed || prTimePerfMilliseconds(start, prTimePerfNow()) > 5000)
            return picture;
        prThreadSleep(1);
    }
}

} // namespace

TEST_CASE("a wanted picture holds its file's source, and a cache realizes it with the mip levels that a size needs") {
    Harness h = harnessCreate();
    REQUIRE(prFileCreateDirectory(pathOf("")));
    String path = pathOf("gradient.png");
    writePng(h, path, 800, 400);
    Loader* loader = loaderCreate(h.catalog);

    const Picture& picture = wantUntilOpen(loader, path);
    REQUIRE(picture.source != nullptr);
    CHECK(picture.extent == PrUVec2{800, 400});
    CHECK(prTextureSourceDesc(picture.source)->extent == PrUVec2{800, 400});

    // A cache realizes the source. A PNG has no previews, so a thumbnail of 90 pixels reads the image, and the cache
    // makes the mip levels that the thumbnail minifies from.
    PrGpuDevice* device = prGpuDeviceCreate(PrGpuBackend_Memory, nullptr);
    REQUIRE(device != nullptr);
    PrTextureCacheDesc cacheDesc = prTextureCacheDescDefault(device);
    PrTextureCache* textures = prTextureCacheCreate(&cacheDesc);
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(PrTextureCacheFlag_Levels);
    ask.detail = 90;
    PrTextureRegion region{};
    REQUIRE(textureCacheAwait(textures, picture.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{800, 400});
    CHECK(region.source == PrUVec2{800, 400});
    CHECK(region.levelCount > 1);
    // The view at the image's size minifies nothing. The texture with levels serves it too, so the cache holds one.
    ask.detail = 800;
    REQUIRE(textureCacheAwait(textures, picture.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(prTextureCacheInfo(textures).textures == 1);

    // A second want of the path is the same source.
    const PrTextureSource* first = picture.source;
    prTextureSourceRetain(first);
    CHECK(wantUntilOpen(loader, path).source == first);
    prTextureSourceRelease(first);

    prTextureCacheDestroy(textures);
    prGpuDeviceDestroy(device);
    loaderDestroy(loader);
    CHECK(prFileRemove(path));
    harnessDestroy(h);
}

TEST_CASE("a picture whose file does not open fails, and opens the file again after it is forgotten") {
    Harness h = harnessCreate();
    REQUIRE(prFileCreateDirectory(pathOf("")));
    String broken = pathOf("broken.png");
    REQUIRE(prFileWrite(broken, PrByteView{reinterpret_cast<const uint8_t*>("not a png"), 9}));
    Loader* loader = loaderCreate(h.catalog);
    const Picture& picture = wantUntilOpen(loader, broken);
    CHECK(picture.failed);
    CHECK(picture.source == nullptr);
    CHECK(wantUntilOpen(loader, pathOf("missing.png")).failed);

    // The file is written whole, and a forgotten picture opens what the file now holds.
    writePng(h, broken, 8, 8);
    CHECK(wantUntilOpen(loader, broken).failed);
    loaderForget(loader, broken);
    const Picture& again = wantUntilOpen(loader, broken);
    CHECK(again.source != nullptr);
    CHECK(again.extent == PrUVec2{8, 8});
    loaderDestroy(loader);
    CHECK(prFileRemove(broken));
    harnessDestroy(h);
}

TEST_CASE("a picture that no frame wants and nothing draws leaves the catalog") {
    Harness h = harnessCreate();
    REQUIRE(prFileCreateDirectory(pathOf("")));
    String path = pathOf("released.png");
    writePng(h, path, 64, 64);
    Loader* loader = loaderCreate(h.catalog);
    REQUIRE(wantUntilOpen(loader, path).source != nullptr);
    CHECK(prTextureCatalogNameCount(h.catalog) == 1);
    // The next frame releases the source that the last frame held. Nothing else holds the source.
    loaderBegin(loader, PrUVec2{1920, 1080}, nullptr);
    loaderEnd(loader);
    CHECK(prTextureCatalogNameCount(h.catalog) == 0);
    loaderDestroy(loader);
    CHECK(prFileRemove(path));
    harnessDestroy(h);
}
