// The loader: the pictures of image files, as the texture sources that a catalog names.

#include "apps/pandite/loader.h"

#include "pr/array.h"
#include "pr/construct.h"
#include "pr/float.h"

namespace pandite {

using namespace pr;

struct Loader {
    PrTextureCatalog* catalog;
    PrTextureCache* PR_NULLABLE textures;
    Array<Picture*> pictures;       // the pictures of the frame. Each holds a reference to its source.
    PrUVec2 screen;
    bool waiting;                   // set if a want of the frame found its file still opening
};

namespace {

// Returns the picture of the file at `path` in the state that the catalog has. The picture holds a reference to its
// source.
Picture pictureOf(Loader* loader, PrStringView path) {
    Picture picture{};
    PrTextureCatalogState state = prTextureCatalogSource(loader->catalog, path, &picture.source);
    if (state == PrTextureCatalogState_Pending) {
        loader->waiting = true;
    } else if (state == PrTextureCatalogState_Failed) {
        picture.failed = true;
        picture.status = prTextureCatalogStatus(loader->catalog, path);
    } else {
        picture.extent = prTextureDescDisplayedExtent(prTextureSourceDesc(picture.source));
    }
    return picture;
}

void releasePictures(Loader* loader) {
    for (Picture* picture : loader->pictures) {
        prTextureSourceRelease(picture->source);
        destroy(picture);
    }
    loader->pictures.clear();
}

} // namespace

Loader* loaderCreate(PrTextureCatalog* catalog) {
    Loader* loader = create<Loader>();
    loader->catalog = catalog;
    return loader;
}

void loaderDestroy(Loader* PR_NULLABLE loader) {
    if (loader == nullptr)
        return;
    releasePictures(loader);
    destroy(loader);
}

void loaderBegin(Loader* loader, PrUVec2 screen, PrTextureCache* PR_NULLABLE textures) {
    releasePictures(loader);
    loader->screen = screen;
    loader->textures = textures;
    loader->waiting = false;
}

const Picture& loaderWant(Loader* loader, PrStringView path) {
    Picture* picture = create<Picture>();
    *picture = pictureOf(loader, path);
    loader->pictures.push(picture);
    return *picture;
}

void loaderWantAhead(Loader* loader, PrStringView path) {
    const Picture& picture = loaderWant(loader, path);
    if (picture.source == nullptr || loader->textures == nullptr || loader->screen.x == 0 || loader->screen.y == 0)
        return;
    // The view fits an image that is larger than the screen to the screen, and shows a smaller image at its own size.
    float fit = prF32Min(float(loader->screen.x) / float(picture.extent.x),
        float(loader->screen.y) / float(picture.extent.y));
    fit = prF32Min(fit, 1);
    PrVec2 shown{float(picture.extent.x) * fit, float(picture.extent.y) * fit};
    // This ask matches the ask that an image element makes for a picture of this size, so the element finds the
    // texture.
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(PrTextureCacheFlag_Levels);
    ask.priority = PrTextureCachePriority_Low;
    ask.detail = uint32_t(prF32Ceil(prF32Max(prF32Max(shown.x, shown.y), 1)));
    PrTextureRegion region{};
    prTextureCacheRealize(loader->textures, picture.source, ask, &region);
}

void loaderEnd(Loader* loader) {
    prTextureCatalogTrim(loader->catalog);
}

void loaderForget(Loader* loader, PrStringView path) {
    prTextureCatalogForget(loader->catalog, path);
}

bool loaderIsBusy(const Loader* loader) {
    if (loader->waiting)
        return true;
    if (loader->textures == nullptr)
        return false;
    PrTextureCacheInfo info = prTextureCacheInfo(loader->textures);
    return info.waiting != 0 || info.inFlight != 0;
}

} // namespace pandite
