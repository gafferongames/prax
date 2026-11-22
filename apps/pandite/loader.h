// The loader: the pictures of image files. A picture is a texture source that a catalog names by the path of the file.
//
// A frame declares what it wants between loaderBegin and loaderEnd: the image on screen, the images beside it, and the
// gallery's thumbnails. A want returns the source of the file once the catalog has opened the file. The view draws the
// source in an image element, and the element asks the texture cache for the texels that it shows. A thumbnail realizes
// a preview of about its size. An image larger than the screen realizes the smallest preview that covers the screen,
// which reads at a fraction of the cost of the image (pr/texture_cache.h). The whole image is read when the view zooms
// into it.
//
// The loader keeps no pixels. A source holds the path of its file, and the cache holds the textures, within its budget.
// A want of an image beside the one on screen asks the cache for it ahead of need, at low priority, so that the image
// is ready when the user steps to it.
//
// A loader is used from one thread.

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"
#include "pr/texture_catalog.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

// The picture of a file: its source once the file has opened, the extent that it displays at when upright, and why it
// failed.
struct Picture {
    const PrTextureSource* PR_NULLABLE source;  // valid until the next loaderBegin
    PrUVec2 extent;                             // zero until the file opens
    bool failed;                                // the file does not open
    PrStatus status;                            // why it failed
};

// The pictures of files.
struct Loader;

// Creates and destroys a loader. The loader gets its sources from `catalog`, which must outlive the loader.
Loader* loaderCreate(PrTextureCatalog* catalog);
void loaderDestroy(Loader* PR_NULLABLE loader);

// Begins the wants of a frame for a screen of `screen` target pixels. Releases the sources of the previous frame.
// `textures` is the cache that the loader asks for the images that the frame wants ahead of need: the cache of the
// window that draws the pictures, or null for none.
void loaderBegin(Loader* loader, PrUVec2 screen, PrTextureCache* PR_NULLABLE textures);

// Wants the picture of the file at `path` and returns the picture in its current state. The picture stays valid until
// the next loaderBegin.
const Picture& loaderWant(Loader* loader, PrStringView path);

// Wants the picture of the file at `path` ahead of need. Asks the cache, at low priority, for the texture that the view
// would draw if the image were fitted to the screen.
void loaderWantAhead(Loader* loader, PrStringView path);

// Ends the wants of the frame.
void loaderEnd(Loader* loader);

// Forgets the picture of the file at `path`, so that a later want opens the file again. Use it after the file changes.
void loaderForget(Loader* loader, PrStringView path);

// Returns true if a want of the last frame is waiting for its file to open, or if the cache has textures in progress.
bool loaderIsBusy(const Loader* loader);

} // namespace pandite

PR_ASSUME_NONNULL_END
