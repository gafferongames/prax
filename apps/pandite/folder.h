// The images of a folder: the files beside the file that the viewer opened. The viewer steps through them in natural
// order, the order in which people sort names (prStringCompareNatural).

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/image.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The index that stands for no image of a folder.
constexpr uint32_t kNoImage = 0xffffffffu;

// The images of a folder: the absolute path of the folder, and the names of the files in it that decode, in natural
// order.
struct Folder {
    String directory;
    Array<String> names;
};

// Returns true if `codecs` decode the format of the file `name`. The extension of the name gives the format
// (prMimeFormatOfFileName).
bool folderShows(const PrImageCodecSet* codecs, PrStringView name);

// Lists into `out` the images of the directory `directory` that `codecs` decode. Returns false and lists none if the
// directory cannot be read.
bool folderList(PrStringView directory, const PrImageCodecSet* codecs, Folder* out);

// Returns the index of the image named `name`, or kNoImage if the folder has none of that name.
uint32_t folderFind(const Folder& folder, PrStringView name);

// Returns the index of the image named `name`. If the folder has none, returns the index of the first image that sorts
// after `name`, or of the last image if none sorts after it. That is where an image that left the folder stood. Returns
// kNoImage for an empty folder.
uint32_t folderNearest(const Folder& folder, PrStringView name);

// Returns the path of the image at `index`.
String folderPath(const Folder& folder, uint32_t index);

} // namespace pandite

PR_ASSUME_NONNULL_END
