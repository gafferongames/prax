// emjc decoding.
//
// An image is a 16-byte header, then an LZFSE stream of four planes: the alpha of each pixel, a filter for each row,
// three residuals for each pixel, and an appendix. A pixel's color is coded as (base, p, q), a reversible transform of
// (r, g, b) in the manner of YCoCg-R: p = r - b, q = g - b - p / 2 with the half rounded toward zero, and base = g - q
// / 2 with the half rounded away from zero. A row's filter predicts each of its pixel's components from the pixels left
// of and above it, and the residuals are the differences from the predictions. A residual's byte holds its magnitude
// above the offset of its component, doubled, with the low bit set for a negative one. The appendix gives the offsets
// of the components whose residuals exceed what a byte holds: each of its bytes skips its high six bits' worth of
// components, then gives the next component an offset of 128 times its low two bits.

#include "pr/emjc.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// An emjc image, as its header describes it. `stream` refers to the bytes that the header was read from.
struct EmjcImage {
    PrByteView stream;              // the LZFSE stream of the image's planes
    uint32_t width;
    uint32_t height;
    uint32_t appendixSize;          // the bytes of the offsets of the residuals that exceed a byte
};

constexpr uint8_t kSignature[4] = {'e', 'm', 'j', '1'};
constexpr uint32_t kHeaderSize = 16;

// The filters of a row. Each predicts a component from the same component of the pixels left of it and above it, and
// uses whichever of them is there at the image's edges.
enum {
    EmjcFilter_None,
    EmjcFilter_Gradient,    // the left pixel if the base changes less to it from the pixel above left, else the upper
    EmjcFilter_Left,
    EmjcFilter_Up,
    EmjcFilter_Average,     // the mean of left and upper, rounded toward zero from a half above
    EmjcFilter_Count,
};
typedef uint8_t EmjcFilter;

// The components whose residuals have an offset, read from the appendix in order.
struct Offsets {
    const uint8_t* at;
    const uint8_t* end;
    uint64_t next;          // the component of the next offset, or past the last component once the appendix ends
    int32_t value;
};

void advance(Offsets& o, uint64_t start) {
    o.next = ~uint64_t(0);
    if (o.at == o.end)
        return;
    o.next = start + (*o.at >> 2);
    o.value = 128 * (*o.at & 3);
    ++o.at;
}

// Returns the residual of component `index`, whose byte is `coded`.
inline int32_t residual(Offsets& o, uint64_t index, uint8_t coded) {
    int32_t offset = 0;
    if (index == o.next) {
        offset = o.value;
        advance(o, index + 1);
    }
    const int32_t magnitude = coded >> 1;
    return coded & 1 ? -magnitude - offset : magnitude + offset;
}

inline int32_t absolute(int32_t value) {
    return value < 0 ? -value : value;
}

// Returns half of `value` rounded toward zero, and half rounded away from it. The two sum to `value`.
inline int32_t halfToward(int32_t value) {
    return value / 2;
}
inline int32_t halfAway(int32_t value) {
    return value - value / 2;
}

bool hasSignature(PrByteView bytes) {
    return bytes.size >= sizeof(kSignature) && prMemEquals(bytes.data, kSignature, sizeof(kSignature));
}

PrStatus readImage(PrByteView bytes, EmjcImage* out) {
    if (bytes.size < kHeaderSize || !hasSignature(bytes))
        return PrStatus_InvalidData;
    out->width = prMemReadLeU16(bytes.data + 8);
    out->height = prMemReadLeU16(bytes.data + 10);
    out->appendixSize = prMemReadLeU16(bytes.data + 12);
    out->stream = PrByteView{bytes.data + kHeaderSize, bytes.size - kHeaderSize};
    return out->width != 0 && out->height != 0 ? PrStatus_Ok : PrStatus_InvalidData;
}

uint64_t planesSizeOf(const EmjcImage& image) {
    const uint64_t pixels = uint64_t(image.width) * image.height;
    return pixels * 4 + image.height + image.appendixSize;
}

bool decodeImage(const PrCompressCodecSet* compress, const EmjcImage& image, uint8_t* pixels, size_t pixelsSize) {
    const uint32_t width = image.width;
    const uint32_t height = image.height;
    const size_t count = size_t(width) * height;
    if (pixelsSize != count * 4)
        return false;
    const size_t planesSize = size_t(planesSizeOf(image));
    uint8_t* planes = static_cast<uint8_t*>(prMemoryAlloc(planesSize + 1));
    PrCompressRead read{};
    const PrStatus status = prCompressCodecSetDecode(compress, PrCompressFormat_Lzfse, image.stream, planes, planesSize + 1,
        &read);
    if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != planesSize) {
        prMemoryFree(planes);
        return false;
    }
    const uint8_t* alpha = planes;
    const uint8_t* filters = alpha + count;
    const uint8_t* residuals = filters + height;
    Offsets offsets{residuals + count * 3, residuals + count * 3 + image.appendixSize, 0, 0};
    advance(offsets, 0);

    // The (base, p, q) of the row above and of this row.
    int32_t* rows = static_cast<int32_t*>(prMemoryAlloc(sizeof(int32_t) * 6 * width));
    int32_t* above = rows;
    int32_t* row = rows + 3 * width;
    bool valid = true;
    for (uint32_t y = 0; y < height && valid; ++y) {
        const EmjcFilter filter = filters[y];
        valid = filter < EmjcFilter_Count;
        for (uint32_t x = 0; x < width && valid; ++x) {
            const size_t index = size_t(y) * width + x;
            int32_t* c = row + 3 * x;
            for (uint32_t k = 0; k < 3; ++k)
                c[k] = residual(offsets, index * 3 + k, residuals[index * 3 + k]);
            const int32_t* left = x > 0 ? c - 3 : nullptr;
            const int32_t* up = y > 0 ? above + 3 * x : nullptr;
            const int32_t* from = nullptr;
            if (filter == EmjcFilter_Left)
                from = left;
            else if (filter == EmjcFilter_Up)
                from = up;
            else if (filter != EmjcFilter_None)
                from = left != nullptr ? left : up;
            if (filter == EmjcFilter_Gradient && left != nullptr && up != nullptr) {
                const int32_t corner = up[-3];
                if (absolute(left[0] - corner) < absolute(up[0] - corner))
                    from = up;
            }
            if (filter == EmjcFilter_Average && left != nullptr && up != nullptr) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] += (left[k] + up[k] + 1) / 2;
            } else if (from != nullptr) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] += from[k];
            }
            const int32_t base = c[0];
            const int32_t p = c[1];
            const int32_t q = c[2];
            uint8_t* out = pixels + index * 4;
            // Components out of range, which no image that the transform made has, wrap.
            out[0] = uint8_t(base + halfAway(p) - halfToward(q));
            out[1] = uint8_t(base + halfAway(q));
            out[2] = uint8_t(base - halfToward(p) - halfToward(q));
            out[3] = alpha[index];
        }
        int32_t* swap = above;
        above = row;
        row = swap;
    }
    prMemoryFree(rows);
    prMemoryFree(planes);
    return valid;
}

} // namespace

// An emjc image open for reading. The scratch pixels hold the decoded image, tightly packed, before it is copied out.
struct EmjcDecoder {
    PrEmjcInfo info;
    EmjcImage image;
    const PrCompressCodecSet* compress;
    uint8_t* scratch;
    size_t pixelsSize;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrEmjcDecoder : EmjcDecoder {};

bool prEmjcHasSignature(PrByteView bytes) {
    return hasSignature(bytes);
}

PrEmjcLimits prEmjcLimitsDefault(void) {
    return PrEmjcLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prEmjcDecoderCreate(PrByteView bytes, const PrCompressCodecSet* compress,
        const PrEmjcLimits* PR_NULLABLE limits, uint64_t held, PrEmjcDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    PR_ASSERT(held >= bytes.size);
    const PrEmjcLimits budget = limits != nullptr ? *limits : prEmjcLimitsDefault();
    if (!hasSignature(bytes) || !prCompressCodecSetDecodes(compress, PrCompressFormat_Lzfse))
        return PrStatus_NotSupported;
    if (budget.work == 0)
        return PrStatus_Limit;
    EmjcImage image{};
    PrStatus status = readImage(bytes, &image);
    if (status != PrStatus_Ok)
        return status;
    const uint64_t pixels = uint64_t(image.width) * image.height;
    // The planes and the decoded pixels take four bytes a pixel each, and the components of two rows 24 bytes a pixel
    // of a row.
    const uint64_t working = held + sizeof(PrEmjcDecoder) + pixels * 8 + image.height + image.appendixSize +
        24 * uint64_t(image.width);
    if (pixels > budget.pixels || pixels > budget.work || pixels * 4 > budget.outputBytes ||
            working > budget.workingBytes)
        return PrStatus_Limit;
    PrEmjcDecoder* decoder = allocZeroed<PrEmjcDecoder>();
    decoder->info = PrEmjcInfo{.extent = PrUVec2{image.width, image.height}, .workingBytes = working};
    decoder->image = image;
    decoder->compress = compress;
    decoder->pixelsSize = size_t(pixels) * 4;
    decoder->scratch = static_cast<uint8_t*>(prMemoryAlloc(decoder->pixelsSize));
    *out = decoder;
    return PrStatus_Ok;
}

void prEmjcDecoderDestroy(PrEmjcDecoder* PR_NULLABLE decoder) {
    if (decoder == nullptr)
        return;
    prMemoryFree(decoder->scratch);
    prMemoryFree(decoder);
}

const PrEmjcInfo* prEmjcDecoderInfo(const PrEmjcDecoder* decoder) {
    return &decoder->info;
}

PrStatus prEmjcDecoderRead(PrEmjcDecoder* decoder, PrPixels output) {
    PR_ASSERT(output.extent.x == decoder->info.extent.x && output.extent.y == decoder->info.extent.y);
    const size_t rowBytes = size_t(output.extent.x) * 4;
    PR_ASSERT(output.stride >= rowBytes);
    if (!decodeImage(decoder->compress, decoder->image, decoder->scratch, decoder->pixelsSize))
        return PrStatus_InvalidData;
    for (uint32_t row = 0; row < output.extent.y; ++row)
        prMemCopy(output.data + size_t(row) * output.stride, decoder->scratch + size_t(row) * rowBytes, rowBytes);
    return PrStatus_Ok;
}
