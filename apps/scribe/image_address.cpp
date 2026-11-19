// The address handler of images: a step names a frame, a mip level, or both.

#include "apps/scribe/image_address.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/construct.h"
#include "pr/string_ops.h"

namespace scribe {

namespace {

struct ImageHandler : AddressHandler {
    const PrImageCodecSet* codec = nullptr;
};

bool imageOpens(const AddressHandler* handler, PrMimeFormat format) {
    PrMimeFormatInfo info = prMimeFormatInfo(format);
    return info.kind == PrMimeKind_Image &&
        prImageCodecSetSupportsType(static_cast<const ImageHandler*>(handler)->codec, info.type);
}

PrStatus imageOpen(AddressHandler* handler, AddressNode* node, void* PR_NULLABLE* PR_NONNULL state) {
    PrImageDecoder* decoder = prImageDecoderCreate(static_cast<ImageHandler*>(handler)->codec);
    PrImageSource source{};
    // The node holds its bytes for as long as the decoder lives.
    source.memory = prBlobBytes(addressNodeBytes(node));
    PrStatus status = prImageDecoderOpen(decoder, source, nullptr);
    if (status != PrStatus_Ok) {
        prImageDecoderDestroy(decoder);
        return status == PrStatus_Truncated || status == PrStatus_NotSupported ? PrStatus_InvalidData : status;
    }
    *state = decoder;
    return PrStatus_Ok;
}

void imageClose(AddressHandler*, void* PR_NULLABLE state) {
    prImageDecoderDestroy(static_cast<PrImageDecoder*>(state));
}

// Reads a decimal number of at most nine digits, the whole of `text`. Returns false for anything else.
bool readNumber(PrStringView text, uint32_t* out) {
    if (text.size == 0 || text.size > 9)
        return false;
    uint32_t value = 0;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsDigit(text.data[i]))
            return false;
        value = value * 10 + uint32_t(text.data[i] - '0');
    }
    *out = value;
    return true;
}

PrStatus imageStep(AddressHandler*, AddressNode*, void* PR_NULLABLE state, PrStringView step, uint64_t,
        AddressStepResult* out) {
    const PrImageInfo* info = prImageDecoderInfo(static_cast<const PrImageDecoder*>(state));
    bool hasFrame = false;
    bool hasLevel = false;
    uint32_t frame = 0;
    uint32_t level = 0;
    // Named dimensions, each `name=value`, joined by `&`, each at most once.
    PrStringView rest = step;
    do {
        size_t amp = prStringFindByte(rest, '&');
        PrStringView pair = prStringHead(rest, amp);
        rest = amp == rest.size ? PrStringView{""} : prStringFrom(rest, amp + 1);
        size_t equals = prStringFindByte(pair, '=');
        PrStringView name = prStringHead(pair, equals);
        PrStringView value = equals == pair.size ? PrStringView{""} : prStringFrom(pair, equals + 1);
        bool* seen = nullptr;
        uint32_t* number = nullptr;
        if (name == "frame") {
            seen = &hasFrame;
            number = &frame;
        } else if (name == "level") {
            seen = &hasLevel;
            number = &level;
        }
        if (seen == nullptr || *seen || !readNumber(value, number))
            return PrStatus_Syntax;
        *seen = true;
    } while (rest.size != 0);
    if (frame >= info->frameCount || level >= info->levelCount)
        return PrStatus_NotFound;
    out->kind = AddressNodeKind_Location;
    return PrStatus_Ok;
}

void imageDestroy(AddressHandler* handler) {
    ImageHandler* image = static_cast<ImageHandler*>(handler);
    destroy(image);
}

} // namespace

AddressHandler* imageAddressHandlerCreate(const PrImageCodecSet* codec) {
    ImageHandler* handler = create<ImageHandler>();
    handler->destroyFn = imageDestroy;
    handler->opensFn = imageOpens;
    handler->openFn = imageOpen;
    handler->closeFn = imageClose;
    handler->stepFn = imageStep;
    handler->codec = codec;
    return handler;
}

} // namespace scribe
