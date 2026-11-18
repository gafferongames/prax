// The selector for lib/image's codecs.

#include "pr/image_select.h"

#include "pr/count_of.h"
#if defined(PR_IMAGE_HAS_BUILTIN)
#include "pr/image_emjc.h"
#include "pr/image_gif.h"
#include "pr/image_ico.h"
#include "pr/image_jpeg.h"
#include "pr/image_png.h"
#include "pr/image_tiff.h"
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
#include "pr/image_imageio.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kNames[PrImageCodecBackend_Count] = {"default", "builtin", "imageio"};

#if defined(PR_IMAGE_HAS_BUILTIN)
constexpr uint64_t kBuiltin = 1u << PrImageCodecBackend_Builtin;
#else
constexpr uint64_t kBuiltin = 0;
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
constexpr uint64_t kWithImageio = kBuiltin | 1u << PrImageCodecBackend_ImageIO;
#else
constexpr uint64_t kWithImageio = kBuiltin;
#endif

// A format that Prax has: its media type, and the descriptor of its codecs.
struct ImageSelectFormat {
    PrStringView type;
    PrBackendInterface interface;
};

// The formats, in the order of a set of every format. ImageIO does not decode EMJC.
constexpr ImageSelectFormat kFormats[] = {
    {"image/png", {.name = "image_png", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/jpeg", {.name = "image_jpeg", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/gif", {.name = "image_gif", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/x-icon", {.name = "image_ico", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/tiff", {.name = "image_tiff", .names = kNames, .count = 3, .built = kWithImageio}},
    {"image/x-emjc", {.name = "image_emjc", .names = kNames, .count = 3, .built = kBuiltin}},
};

const ImageSelectFormat* PR_NULLABLE formatOf(PrStringView type) {
    for (const ImageSelectFormat& format : kFormats) {
        if (format.type == type)
            return &format;
    }
    return nullptr;
}

#if defined(PR_IMAGE_HAS_BUILTIN)
PrImageCodec* createBuiltin(uint32_t format, const PrCompressCodecSet* compress) {
    switch (format) {
        case 0: return prImagePngCreate(compress);
        case 1: return prImageJpegCreate();
        case 2: return prImageGifCreate();
        case 3: return prImageIcoCreate();
        case 4: return prImageTiffCreate();
        default: return prImageEmjcCreate(compress);
    }
}
#endif

// Returns the backend that Default stands for without a choice for `format`: Prax's own, or else the first that the
// build includes.
PrImageCodecBackend defaultBackend(const ImageSelectFormat& format) {
    for (uint32_t backend = PrImageCodecBackend_Builtin; backend < PrImageCodecBackend_Count; ++backend) {
        if (prBackendIsBuilt(&format.interface, backend))
            return PrImageCodecBackend(backend);
    }
    return PrImageCodecBackend_Count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* PR_NULLABLE prImageCodecSelectInterface(PrStringView type) {
    const ImageSelectFormat* format = formatOf(type);
    return format != nullptr ? &format->interface : nullptr;
}

PrImageCodec* PR_NULLABLE prImageCodecCreate(PrImageCodecBackend backend, PrStringView type,
        const PrCompressCodecSet* compress) {
    const ImageSelectFormat* format = formatOf(type);
    if (format == nullptr)
        return nullptr;
    if (backend == PrImageCodecBackend_Default) {
        uint32_t chosen = 0;
        if (prBackendFromEnvironment(&format->interface, &chosen, nullptr) != PrStatus_Ok)
            return nullptr;
        backend = PrImageCodecBackend(chosen);
        if (backend == PrImageCodecBackend_Default)
            backend = defaultBackend(*format);
    }
    if (!prBackendIsBuilt(&format->interface, backend))
        return nullptr;
    switch (backend) {
#if defined(PR_IMAGE_HAS_BUILTIN)
        case PrImageCodecBackend_Builtin: return createBuiltin(uint32_t(format - kFormats), compress);
#endif
#if defined(PR_IMAGE_HAS_IMAGEIO)
        case PrImageCodecBackend_ImageIO: return prImageImageIOCreate(type);
#endif
        default: return nullptr;
    }
}

PrImageCodecSet* PR_NULLABLE prImageCodecSetCreateDefault(const PrCompressCodecSet* compress) {
    // A format with no backend in the build is left out.
    PrImageCodec* codecs[countOf(kFormats)] = {};
    uint32_t count = 0;
    for (const ImageSelectFormat& format : kFormats) {
        if (!prBackendIsBuilt(&format.interface, PrImageCodecBackend_Default))
            continue;
        PrImageCodec* codec = prImageCodecCreate(PrImageCodecBackend_Default, format.type, compress);
        if (codec == nullptr) {
            for (uint32_t made = 0; made < count; ++made)
                prImageCodecDestroy(codecs[made]);
            return nullptr;
        }
        codecs[count++] = codec;
    }
    return prImageCodecSetCreate(codecs, count);
}
