// ICO decoding.

#include "pr/ico.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// An image chosen from an ICO directory, still encoded.
struct IcoImage {
    const uint8_t* bytes;           // the encoded image
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t headerSize;
    uint32_t paletteCount;
    uint16_t bitCount;
    bool png;                       // true if the image is a PNG instead of a bitmap
};

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool inspectBitmap(IcoImage& image) {
    if (image.size < 40) {
        return false;
    }
    image.headerSize = prMemReadLeU32(image.bytes);
    image.bitCount = uint16_t(prMemReadLeU16(image.bytes + 14));
    if ((image.headerSize != 40 && image.headerSize != 108 && image.headerSize != 124) ||
            image.headerSize > image.size || prMemReadLeU32(image.bytes + 4) != image.width ||
            prMemReadLeU32(image.bytes + 8) != image.height * 2 || prMemReadLeU16(image.bytes + 12) != 1 ||
            prMemReadLeU32(image.bytes + 16) != 0 || (image.bitCount != 1 && image.bitCount != 4 && image.bitCount != 8 &&
                image.bitCount != 24 && image.bitCount != 32)) {
        return false;
    }
    const uint32_t colors = prMemReadLeU32(image.bytes + 32);
    if (image.bitCount <= 8) {
        const uint32_t maximum = 1u << image.bitCount;
        if (colors > maximum) {
            return false;
        }
        image.paletteCount = colors == 0 ? maximum : colors;
    } else if (colors != 0) {
        return false;
    }
    const size_t rowBytes = ((size_t(image.width) * image.bitCount + 31) / 32) * 4;
    const size_t maskRowBytes = ((size_t(image.width) + 31) / 32) * 4;
    const size_t bitmapEnd = image.headerSize + image.paletteCount * 4 + rowBytes * image.height;
    return bitmapEnd <= image.size && (image.bitCount == 32 || maskRowBytes * image.height <= image.size - bitmapEnd);
}

// Chooses the largest complete image in `bytes` that the decoder reads. Among images of equal area, it chooses the one
// with the greatest bit depth. Returns false for a malformed directory or one without such an image.
bool selectImage(PrByteView bytes, IcoImage* out) {
    *out = {};
    if (bytes.size < 6 || prMemReadLeU16(bytes.data) != 0 || prMemReadLeU16(bytes.data + 2) != 1) {
        return false;
    }
    const uint32_t count = prMemReadLeU16(bytes.data + 4);
    const size_t directoryEnd = 6 + size_t(count) * 16;
    if (count == 0 || directoryEnd > bytes.size) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = bytes.data + 6 + size_t(i) * 16;
        const size_t length = prMemReadLeU32(entry + 8);
        const size_t offset = prMemReadLeU32(entry + 12);
        if (offset < directoryEnd || offset > bytes.size || length > bytes.size - offset) {
            return false;
        }
        IcoImage image = {};
        image.bytes = bytes.data + offset;
        image.size = length;
        image.width = entry[0] == 0 ? 256 : entry[0];
        image.height = entry[1] == 0 ? 256 : entry[1];
        image.png = length >= sizeof(kPngSignature) && prMemEquals(image.bytes, kPngSignature, sizeof(kPngSignature));
        if (image.png) {
            if (length < 29 || prMemReadBeU32(image.bytes + 16) != image.width ||
                    prMemReadBeU32(image.bytes + 20) != image.height) {
                continue;
            }
            image.bitCount = 32;
        } else if (!inspectBitmap(image)) {
            continue;
        }
        const uint32_t area = image.width * image.height;
        const uint32_t heldArea = out->width * out->height;
        if (area > heldArea || (area == heldArea && image.bitCount > out->bitCount)) {
            *out = image;
        }
    }
    return out->bytes != nullptr;
}

// Decodes the bitmap `image` into `pixels` as tightly packed, straight-alpha sRGB Rgba8. Returns false if `image` is a
// PNG or `pixelsSize` is not the size of the image.
bool decodeBitmap(const IcoImage& image, uint8_t* pixels, size_t pixelsSize) {
    if (image.png || pixelsSize != size_t(image.width) * image.height * 4) {
        return false;
    }
    const size_t rowBytes = ((size_t(image.width) * image.bitCount + 31) / 32) * 4;
    const size_t maskRowBytes = ((size_t(image.width) + 31) / 32) * 4;
    const uint8_t* palette = image.bytes + image.headerSize;
    const uint8_t* bitmap = palette + image.paletteCount * 4;
    const size_t maskOffset = image.headerSize + image.paletteCount * 4 + rowBytes * image.height;
    const bool hasMask = maskRowBytes * image.height <= image.size - maskOffset;
    bool hasAlpha = false;
    if (image.bitCount == 32) {
        for (uint32_t y = 0; y < image.height && !hasAlpha; ++y) {
            for (uint32_t x = 0; x < image.width; ++x) {
                hasAlpha = hasAlpha || bitmap[size_t(y) * rowBytes + size_t(x) * 4 + 3] != 0;
            }
        }
    }
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint32_t sourceY = image.height - 1 - y;
        const uint8_t* source = bitmap + size_t(sourceY) * rowBytes;
        const uint8_t* mask = hasMask ? image.bytes + maskOffset + size_t(sourceY) * maskRowBytes : nullptr;
        for (uint32_t x = 0; x < image.width; ++x) {
            const uint8_t* color = nullptr;
            if (image.bitCount <= 8) {
                const uint32_t bit = x * image.bitCount;
                const uint32_t index = (source[bit / 8] >> (8 - image.bitCount - bit % 8)) & ((1u << image.bitCount) - 1);
                if (index >= image.paletteCount) {
                    return false;
                }
                color = palette + index * 4;
            } else {
                color = source + size_t(x) * (image.bitCount / 8);
            }
            uint8_t* pixel = pixels + (size_t(y) * image.width + x) * 4;
            pixel[0] = color[2];
            pixel[1] = color[1];
            pixel[2] = color[0];
            // Legacy 32-bit icons with all-zero alpha use the AND mask.
            if (hasAlpha)
                pixel[3] = color[3];
            else if (mask != nullptr && (mask[x / 8] & (0x80u >> (x % 8))) != 0)
                pixel[3] = 0;
            else
                pixel[3] = 255;
        }
    }
    return true;
}

} // namespace

// An icon open for reading. The scratch pixels hold a decoded bitmap, tightly packed, before it is copied out.
struct IcoDecoder {
    PrIcoInfo info;
    IcoImage image;
    uint8_t* PR_NULLABLE scratch;
    size_t pixelsSize;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrIcoDecoder : IcoDecoder {};

bool prIcoHasSignature(PrByteView bytes) {
    return bytes.size >= 6 && prMemReadLeU16(bytes.data) == 0 && prMemReadLeU16(bytes.data + 2) == 1;
}

PrIcoLimits prIcoLimitsDefault(void) {
    return PrIcoLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prIcoDecoderCreate(PrByteView bytes, const PrIcoLimits* PR_NULLABLE limits, uint64_t held,
        PrIcoDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(held >= bytes.size);
    const PrIcoLimits budget = limits != nullptr ? *limits : prIcoLimitsDefault();
    if (!prIcoHasSignature(bytes))
        return PrStatus_NotSupported;
    if (prMemReadLeU16(bytes.data + 4) > budget.work)
        return PrStatus_Limit;
    IcoImage image{};
    if (!selectImage(bytes, &image))
        return PrStatus_InvalidData;
    PrIcoInfo info{.extent = PrUVec2{image.width, image.height}};
    uint64_t working = held + sizeof(PrIcoDecoder);
    size_t pixelsSize = 0;
    if (image.png) {
        info.png = PrByteView{image.bytes, image.size};
    } else {
        const uint64_t pixels = uint64_t(image.width) * image.height;
        // The decoded bitmap takes four bytes a pixel, and its decode as many again.
        working += pixels * 8;
        if (pixels > budget.pixels || pixels * 2 > budget.work || pixels * 4 > budget.outputBytes)
            return PrStatus_Limit;
        pixelsSize = size_t(pixels) * 4;
    }
    if (working > budget.workingBytes)
        return PrStatus_Limit;
    info.workingBytes = working;
    PrIcoDecoder* decoder = allocZeroed<PrIcoDecoder>();
    decoder->info = info;
    decoder->image = image;
    decoder->pixelsSize = pixelsSize;
    decoder->scratch = pixelsSize != 0 ? static_cast<uint8_t*>(prMemoryAlloc(pixelsSize)) : nullptr;
    *out = decoder;
    return PrStatus_Ok;
}

void prIcoDecoderDestroy(PrIcoDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->scratch);
    prMemoryFree(decoder);
}

const PrIcoInfo* prIcoDecoderInfo(const PrIcoDecoder* decoder) {
    return &decoder->info;
}

PrStatus prIcoDecoderRead(PrIcoDecoder* decoder, PrPixels output) {
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    if (decoder->image.png)
        return PrStatus_NotSupported;
    const size_t rowBytes = size_t(output.extent.x) * 4;
    PR_ASSERT(output.stride >= rowBytes);
    if (!decodeBitmap(decoder->image, decoder->scratch, decoder->pixelsSize))
        return PrStatus_InvalidData;
    for (uint32_t row = 0; row < output.extent.y; ++row)
        prMemCopy(output.data + size_t(row) * output.stride, decoder->scratch + size_t(row) * rowBytes, rowBytes);
    return PrStatus_Ok;
}
