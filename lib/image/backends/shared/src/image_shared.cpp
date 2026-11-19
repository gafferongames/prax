#include "image/backends/shared/src/image_shared.h"

#include "pr/alloc.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

bool isTag(const uint8_t* bytes, const char* tag) {
    return bytes[0] == uint8_t(tag[0]) && bytes[1] == uint8_t(tag[1]) && bytes[2] == uint8_t(tag[2]) &&
        bytes[3] == uint8_t(tag[3]);
}

} // namespace

PrStatus imageReadSource(const PrImageSource* source, const PrImageLimits& limits, uint64_t readerBytes,
        uint8_t* PR_NULLABLE* PR_NONNULL out, size_t* size) {
    *out = nullptr;
    *size = 0;
    uint64_t length = prImageSourceSize(source);
    if (length > limits.inputBytes || length > size_t(-1) || length > limits.workingBytes ||
            readerBytes > limits.workingBytes - length)
        return PrStatus_Limit;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(size_t(length)));
    PrStatus status = prImageSourceRead(source, 0, bytes, size_t(length));
    if (status != PrStatus_Ok) {
        prMemoryFree(bytes);
        return status;
    }
    *out = bytes;
    *size = size_t(length);
    return PrStatus_Ok;
}

PrColorSpace imageCicpSpace(uint8_t primaries, uint8_t transfer, uint8_t matrix, uint8_t fullRange) {
    if (matrix != 0 || fullRange != 1)
        return PrColorSpace_Count;
    // Primaries: 1 is BT.709 and sRGB, 9 is BT.2020 and BT.2100, 12 is Display P3. Transfer: 13 is sRGB's, 8 is linear,
    // 16 is PQ, 18 is HLG, and 1, 6, 14 and 15 are BT.709's and BT.2020's.
    bool rec709Curve = transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15;
    switch (primaries) {
        case 1:
            return transfer == 13 ? PrColorSpace_Srgb : transfer == 8 ? PrColorSpace_SrgbLinear : PrColorSpace_Count;
        case 12:
            if (transfer == 13)
                return PrColorSpace_DisplayP3;
            return transfer == 8 ? PrColorSpace_DisplayP3Linear : PrColorSpace_Count;
        case 9:
            if (transfer == 16)
                return PrColorSpace_Rec2100Pq;
            if (transfer == 18)
                return PrColorSpace_Rec2100Hlg;
            if (transfer == 8)
                return PrColorSpace_Rec2100Linear;
            return rec709Curve ? PrColorSpace_Rec2020 : PrColorSpace_Count;
        default:
            return PrColorSpace_Count;
    }
}

PrColorSpace imageProfileCicpSpace(PrByteView profile) {
    const uint8_t* bytes = profile.data;
    size_t size = profile.size;
    if (size < 132)
        return PrColorSpace_Count;
    uint32_t tags = prMemReadBeU32(bytes + 128);
    for (uint32_t i = 0; i < tags && 132 + size_t(i + 1) * 12 <= size; ++i) {
        const uint8_t* entry = bytes + 132 + size_t(i) * 12;
        uint32_t offset = prMemReadBeU32(entry + 4);
        uint32_t length = prMemReadBeU32(entry + 8);
        if (!isTag(entry, "cicp") || length < 12 || offset > size || size - offset < 12)
            continue;
        const uint8_t* tag = bytes + offset;
        if (!isTag(tag, "cicp"))
            return PrColorSpace_Count;
        return imageCicpSpace(tag[8], tag[9], tag[10], tag[11]);
    }
    return PrColorSpace_Count;
}

} // namespace pr_impl
