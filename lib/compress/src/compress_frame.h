// The zlib (rfc 1950) and gzip (rfc 1952) frames around Deflate, which a set reads and writes around the Deflate of a
// codec.

#pragma once

#include "pr/compress_backend.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true for the formats that frame Deflate: zlib and gzip.
inline bool compressIsFramed(PrCompressFormat format) {
    return format == PrCompressFormat_Zlib || format == PrCompressFormat_Gzip;
}

// Creates a decoder of `format`, zlib or gzip, that reads the frame and decodes the Deflate inside it with decoders of
// `codec`, which must decode Deflate and outlive the decoder.
PrStatus compressFrameDecoderCreate(PrCompressFormat format, const PrCompressCodec* codec,
    PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);

// Returns the size of the frame of `format` around its Deflate: its header and its trailer.
size_t compressFrameSize(PrCompressFormat format);

// Writes the header of `format` for an encoding at `level` to `output`, and returns its size.
size_t compressFrameWriteHeader(PrCompressFormat format, PrCompressLevel level, uint8_t* output);

// Writes the trailer of `format` for the encoding of `input` to `output`, and returns its size.
size_t compressFrameWriteTrailer(PrCompressFormat format, PrByteView input, uint8_t* output);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
