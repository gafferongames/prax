// The address handler of images: a step names a frame, a mip level, or both, in the form of Media Fragments URI 1.0, as
// `frame=2`, `level=1` or `frame=2&level=1`, and is a location. A frame is a frame of an animation, or a layer or a
// face of a texture, as PrImageInfo counts them.

#pragma once

#include "pr/image.h"

#include "apps/scribe/address.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// Creates the handler of the images that `codec` decodes. `codec` outlives the handler. The caller frees the handler
// with addressHandlerDestroy.
AddressHandler* imageAddressHandlerCreate(const PrImageCodecSet* codec);

} // namespace scribe

PR_ASSUME_NONNULL_END
