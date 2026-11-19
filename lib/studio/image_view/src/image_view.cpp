// Studio's image view: an encoded image fitted to the view, with its frames and mip levels, and an animation playing.

#include "pr/image_view.h"

#include "pr/alloc.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_image.h"
#include "pr/kit_text.h"
#include "pr/mem.h"
#include "pr/text_buffer.h"
#include "pr/texture.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// The height of the line that describes the image, in px.
constexpr float kBarHeight = 30;

// The shortest time that a frame of an animation shows, in milliseconds, so that an animation whose frames say they
// take no time still plays at a pace that can be seen.
constexpr uint32_t kMinFrameMs = 20;

struct ImageView {
    PrKit* kit;
    const PrImageCodecSet* codec;
    PrImageDecoder* decoder;
    PrBlob* PR_NULLABLE bytes;
    PrStatus status;                        // of opening `bytes`
    bool open;
    uint32_t frame;
    uint32_t level;
    bool playing;
    double frameStart;                      // seconds on the monotonic clock when the frame that plays began to show

    // The decoded frame and level that show, or null before they decode or if they do not.
    const PrTextureSource* PR_NULLABLE shown;
    uint32_t shownFrame;
    uint32_t shownLevel;
    PrStatus shownStatus;
};

double nowSeconds() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// Returns how long a frame shows, in seconds.
double durationOf(ImageView* view, uint32_t frame) {
    PrImageFrameInfo info{};
    if (prImageDecoderFrameInfo(view->decoder, frame, &info) != PrStatus_Ok)
        return 0.1;
    uint32_t ms = info.durationMs < kMinFrameMs ? kMinFrameMs : info.durationMs;
    return ms / 1000.0;
}

// Returns true if the image is an animation, whose frames show in turn: frames that have durations.
bool isAnimation(ImageView* view) {
    const PrImageInfo* info = prImageDecoderInfo(view->decoder);
    if (info->frameCount < 2 || info->faceCount != 1)
        return false;
    PrImageFrameInfo frame{};
    return prImageDecoderFrameInfo(view->decoder, 0, &frame) == PrStatus_Ok && frame.durationMs != 0;
}

// Returns true if the image holds light beyond SDR white, which a view shows in extended linear sRGB: an image of
// floats.
bool holdsHdr(const PrImageInfo& info) {
    return info.format.sample == PrImageSample_F16 || info.format.sample == PrImageSample_F32;
}

// What a shown frame and level decode from: the image's bytes, which it holds a reference to, the codec, and which
// frame and level, in sRGB Rgba8 or, for an image of HDR, in linear sRGB half floats.
struct ShownImage {
    PrBlob* bytes;
    const PrImageCodecSet* codec;
    uint32_t frame;
    uint32_t level;
    bool hdr;
};

// Decodes a shown image's frame and level into `out`, of its stored extent, with straight alpha.
PrStatus decodeShownInto(const ShownImage& shown, uint8_t* out) {
    PrImageDecoder* decoder = prImageDecoderCreate(shown.codec);
    PrImageSource source{};
    source.memory = prBlobBytes(shown.bytes);
    PrStatus status = prImageDecoderOpen(decoder, source, nullptr);
    if (status == PrStatus_Truncated)
        status = PrStatus_Ok;
    const PrImageInfo* info = status == PrStatus_Ok ? prImageDecoderInfo(decoder) : nullptr;
    PrUVec2 extent = info != nullptr ? prImageDecoderLevelExtent(decoder, shown.level) : PrUVec2{};
    uint32_t shownBytes = shown.hdr ? 8 : 4;
    PrPixels output{.data = out, .stride = size_t(extent.x) * shownBytes, .extent = extent};
    if (info != nullptr && shown.level == 0 && !shown.hdr) {
        status = prImageDecoderReadRgba8(decoder, shown.frame, PrImageAlpha_Straight, PrImageOrientation_Identity, output);
    } else if (info != nullptr) {
        // The level decodes in the image's own format, and converts.
        uint32_t size = prImageFormatSize(&info->format);
        size_t levelStride = size_t(extent.x) * size;
        uint8_t* level = static_cast<uint8_t*>(prMemoryAlloc(levelStride * extent.y + 1));
        status = prImageDecoderReadLevel(decoder, shown.frame, shown.level,
            PrPixels{.data = level, .stride = levelStride, .extent = extent});
        PrImageFormat shownFormat{.model = PrColorModel_Rgb, .alpha = PrImageAlpha_Straight};
        shownFormat.sample = shown.hdr ? PrImageSample_F16 : PrImageSample_U8;
        shownFormat.color.encoding = PrImageColorEncoding_Named;
        shownFormat.color.space = shown.hdr ? PrColorSpace_SrgbLinear : PrColorSpace_Srgb;
        if (status == PrStatus_Ok || status == PrStatus_Truncated) {
            status = prImageConvert(PrPixelView{.data = level, .stride = levelStride, .extent = extent}, &info->format,
                output, &shownFormat, PrImageConversionFlag_AssumeSrgb | PrImageConversionFlag_ApproximateCmyk);
        }
        prMemoryFree(level);
    }
    prImageDecoderDestroy(decoder);
    return status;
}

// The orientation flags of each EXIF orientation, by PrImageOrientation.
constexpr PrTextureOrientationFlags kOrientations[] = {
    0,
    PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipY,
};

// Makes the image of the frame and the level that show, which decodes them again whenever it is read, so that nothing
// keeps their pixels but what draws them. The first level shows as the image's orientation displays it.
void decodeShown(ImageView* view) {
    if (view->shown != nullptr && view->shownFrame == view->frame && view->shownLevel == view->level)
        return;
    prTextureSourceRelease(view->shown);
    view->shown = nullptr;
    view->shownFrame = view->frame;
    view->shownLevel = view->level;
    const PrImageInfo* info = prImageDecoderInfo(view->decoder);
    bool hdr = holdsHdr(*info);
    ShownImage* shown = allocZeroed<ShownImage>();
    prBlobRetain(view->bytes);
    *shown = ShownImage{view->bytes, view->codec, view->frame, view->level, hdr};
    PrUVec2 extent = prImageDecoderLevelExtent(view->decoder, view->level);
    view->shownStatus = extent.x != 0 && extent.y != 0 ? PrStatus_Ok : PrStatus_InvalidData;
    uint32_t orientation = view->level == 0 ? uint32_t(info->orientation) : 0;
    PrTextureSourceDesc desc{
        .texture = PrTextureDesc{
            .kind = PrGpuTextureKind_2d,
            .extent = extent,
            .depth = 1,
            .layerCount = 1,
            .levelCount = 1,
            .format = PrGpuFormat(hdr ? PrGpuFormat_Rgba16F : PrGpuFormat_Rgba8Srgb),
            .alpha =
                PrTextureAlpha(info->format.alpha == PrImageAlpha_None ? PrTextureAlpha_None : PrTextureAlpha_Straight),
            .orientation =
                orientation < sizeof(kOrientations) ? kOrientations[orientation] : PrTextureOrientationFlags(0),
        },
        .read = [](void* PR_NULLABLE context, PrTextureRead* read) {
            PrStatus status = decodeShownInto(*static_cast<const ShownImage*>(context), prTextureReadStorage(read));
            prTextureReadComplete(read, status);
        },
        .release = [](void* PR_NULLABLE context) {
            ShownImage* held = static_cast<ShownImage*>(context);
            prBlobRelease(held->bytes);
            prMemoryFree(held);
        },
        .context = shown,
    };
    const PrTextureSource* source = prTextureSourceCreate(&desc);
    if (view->shownStatus == PrStatus_Ok)
        view->shown = source;
    else
        prTextureSourceRelease(source);
}

// Advances an animation that plays to the frame that the time `now` shows, and returns when the next frame is due.
double advance(ImageView* view, double now) {
    const PrImageInfo* info = prImageDecoderInfo(view->decoder);
    double due = view->frameStart + durationOf(view, view->frame);
    // A view that was not shown for a while does not race through the frames it missed.
    if (now - due > 1)
        view->frameStart = now;
    while (now >= due) {
        view->frame = (view->frame + 1) % info->frameCount;
        view->frameStart = due;
        due += durationOf(view, view->frame);
    }
    return due;
}

void appendStatus(PrTextBuffer* out, PrStatus status) {
    prTextBufferAppend(out, prStatusName(status));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrImageView : ImageView {};

PrImageView* prImageViewCreate(const PrImageViewDesc* desc) {
    PrImageView* view = allocZeroed<PrImageView>();
    view->kit = desc->kit;
    view->codec = desc->codec;
    view->decoder = prImageDecoderCreate(desc->codec);
    return view;
}

void prImageViewDestroy(PrImageView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    prTextureSourceRelease(view->shown);
    prImageDecoderDestroy(view->decoder);
    prBlobRelease(view->bytes);
    prMemoryFree(view);
}

PrStatus prImageViewSetBytes(PrImageView* view, PrBlob* PR_NULLABLE bytes) {
    prImageDecoderClose(view->decoder);
    prTextureSourceRelease(view->shown);
    view->shown = nullptr;
    if (bytes != nullptr)
        prBlobRetain(bytes);
    prBlobRelease(view->bytes);
    view->bytes = bytes;
    view->open = false;
    view->frame = 0;
    view->level = 0;
    view->playing = false;
    view->status = PrStatus_Ok;
    if (bytes == nullptr)
        return PrStatus_Ok;
    PrImageSource source{};
    source.memory = prBlobBytes(bytes);
    view->status = prImageDecoderOpen(view->decoder, source, nullptr);
    view->open = view->status == PrStatus_Ok || view->status == PrStatus_Truncated;
    if (view->open) {
        view->playing = isAnimation(view);
        view->frameStart = nowSeconds();
    }
    return view->status;
}

const PrImageInfo* PR_NULLABLE prImageViewInfo(const PrImageView* view) {
    return view->open ? prImageDecoderInfo(view->decoder) : nullptr;
}

void prImageViewShow(PrImageView* view, uint32_t frame, uint32_t level) {
    if (!view->open)
        return;
    const PrImageInfo* info = prImageDecoderInfo(view->decoder);
    view->frame = frame < info->frameCount ? frame : info->frameCount - 1;
    view->level = level < info->levelCount ? level : info->levelCount - 1;
    view->playing = false;
}

uint32_t prImageViewShownFrame(const PrImageView* view) {
    return view->frame;
}

uint32_t prImageViewShownLevel(const PrImageView* view) {
    return view->level;
}

void prImageViewFrame(PrImageView* view, PrLayoutNode parent) {
    PrKit* kit = view->kit;
    PrKitBoxStyle column{.place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed}};
    PrLayoutNode box = prKitBox(kit, parent, {}, column);
    PrKitTextStyle muted{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted};
    PrTextBuffer text = {};
    if (view->bytes == nullptr || !view->open) {
        if (view->bytes != nullptr) {
            prTextBufferAppend(&text, "The image does not open: ");
            appendStatus(&text, view->status);
        }
        prKitLabel(kit, box, {.text = prTextBufferView(&text)}, muted);
        prTextBufferReset(&text);
        return;
    }
    const PrImageInfo* info = prImageDecoderInfo(view->decoder);
    PrKitBoxStyle bar{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = 6,
        .padding = prKitEdgesAll(4),
        .place = {.height = prKitPx(kBarHeight), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode line = prKitBox(kit, box, {}, bar);
    prTextBufferAppendU64(&text, info->extent.x, 10);
    prTextBufferAppend(&text, " x ");
    prTextBufferAppendU64(&text, info->extent.y, 10);
    if (info->frameCount > 1) {
        prTextBufferAppend(&text, info->faceCount == 6 ? "   face " : "   frame ");
        prTextBufferAppendU64(&text, view->frame + 1, 10);
        prTextBufferAppend(&text, " of ");
        prTextBufferAppendU64(&text, info->frameCount, 10);
    }
    if (info->levelCount > 1) {
        prTextBufferAppend(&text, "   level ");
        prTextBufferAppendU64(&text, view->level, 10);
        prTextBufferAppend(&text, " of ");
        prTextBufferAppendU64(&text, info->levelCount, 10);
    }
    prKitLabel(kit, line, {.text = prTextBufferView(&text)}, muted);
    prTextBufferClear(&text);
    PrKitButtonStyle small{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    if (info->frameCount > 1) {
        if (prKitActivated(kit, prKitButton(kit, line, {.icon = PrStudioIconKind_ChevronLeft}, small)))
            prImageViewShow(view, (view->frame + info->frameCount - 1) % info->frameCount, view->level);
        PrStudioIconKind play = view->playing ? PrStudioIconKind_Pause : PrStudioIconKind_Play;
        if (prKitActivated(kit, prKitButton(kit, line, {.icon = play}, small))) {
            view->playing = !view->playing;
            view->frameStart = nowSeconds();
        }
        if (prKitActivated(kit, prKitButton(kit, line, {.icon = PrStudioIconKind_ChevronRight}, small)))
            prImageViewShow(view, (view->frame + 1) % info->frameCount, view->level);
    }
    if (info->levelCount > 1) {
        if (prKitActivated(kit, prKitButton(kit, line, {.icon = PrStudioIconKind_Minus}, small)) && view->level > 0)
            prImageViewShow(view, view->frame, view->level - 1);
        if (prKitActivated(kit, prKitButton(kit, line, {.icon = PrStudioIconKind_Plus}, small)))
            prImageViewShow(view, view->frame, view->level + 1);
    }
    PrKitBoxStyle area{.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .flex = PrKitFlex_Fill}};
    PrLayoutNode stage = prKitBox(kit, box, {}, area);
    if (view->playing) {
        double due = advance(view, nowSeconds());
        prUiRequestMotionAt(kit->ui, stage, due);
    }
    decodeShown(view);
    if (view->shown == nullptr) {
        prTextBufferAppend(&text, "The frame does not decode: ");
        appendStatus(&text, view->shownStatus);
        prKitLabel(kit, stage, {.text = prTextBufferView(&text)}, muted);
    } else {
        PrKitImageStyle fit{
            .fit = PrKitImageFit_ScaleDown,
            .place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed},
        };
        prKitImage(kit, stage, {.image = view->shown}, fit);
    }
    prTextBufferReset(&text);
}
