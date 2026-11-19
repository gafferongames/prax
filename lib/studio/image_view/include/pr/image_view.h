// Studio's image view: an encoded image of any format that an image codec decodes, fitted to the view, with its frames
// and mip levels, and an animation playing.
//
// A host gives the view an image's bytes and declares it in each round. The view decodes the frame and the level that
// show, and keeps them until another shows. One thread uses a view.

#pragma once

#include "pr/blob.h"
#include "pr/image.h"
#include "pr/kit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An image, its decoded frame and the controls that choose it.
typedef struct PrImageView PrImageView;

// What a view draws with and decodes with. Both outlive the view.
typedef struct PrImageViewDesc {
    PrKit* kit;
    const PrImageCodecSet* codec;
} PrImageViewDesc;

// Creates a view that shows nothing, or frees one.
PrImageView* prImageViewCreate(const PrImageViewDesc* desc);
void prImageViewDestroy(PrImageView* PR_NULLABLE view);

// Shows the encoded image in `bytes`, which the view retains, from its first frame, playing if it is an animation, or
// shows nothing if `bytes` is null. Returns the status of opening the image (prImageDecoderOpen), and shows why the
// image does not show in the view on failure.
PrStatus prImageViewSetBytes(PrImageView* view, PrBlob* PR_NULLABLE bytes);

// The image's description, or null if no image is open.
const PrImageInfo* PR_NULLABLE prImageViewInfo(const PrImageView* view);

// Shows `frame` at mip level `level`, clamped to what the image has, and stops an animation.
void prImageViewShow(PrImageView* view, uint32_t frame, uint32_t level);

// The frame and the mip level that show.
uint32_t prImageViewShownFrame(const PrImageView* view);
uint32_t prImageViewShownLevel(const PrImageView* view);

// Declares the view in a round under `parent`, which it fills: a line that describes the image with its controls, and
// the image fitted below it, in the room that layout gives it. Advances an animation that plays to the frame of the
// round's time, and asks for a frame when the next one is due.
void prImageViewFrame(PrImageView* view, PrLayoutNode parent);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
