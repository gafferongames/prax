// Deflate (rfc 1951): decoding, a piece of input at a time, and the encoding of a whole input at once. Deflate carries
// no framing of its own; lib/compress frames it as zlib and gzip.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A decoder. It holds the decoding state and the history that back-references read. It keeps no reference to the views
// that a read is given. One thread uses a decoder at a time.
typedef struct PrDeflateDecoder PrDeflateDecoder;

// What a read needs next, or that the stream ended.
enum {
    PrDeflateProgress_Input,    // more input
    PrDeflateProgress_Output,   // more room for output
    PrDeflateProgress_Done,     // the stream ended
};
typedef uint8_t PrDeflateProgress;

// The result of a read.
typedef struct PrDeflateRead {
    size_t consumed;            // number of input bytes read
    size_t written;             // number of output bytes written
    PrDeflateProgress progress;
} PrDeflateRead;

// Creates a decoder at the start of a stream.
PrDeflateDecoder* prDeflateDecoderCreate(void);

// Starts a new stream, keeping the storage.
void prDeflateDecoderReset(PrDeflateDecoder* decoder);

// Frees the decoder.
void prDeflateDecoderDestroy(PrDeflateDecoder* PR_NULLABLE decoder);

// Decodes from `input` into up to `capacity` bytes at `output`, and writes the result to `*out`. `final` means no input
// follows `input`. The stream ends on a byte boundary, and the bytes after it are left unread.
//
// Returns InvalidData if the stream is corrupt, and Truncated if the final input ends inside the stream. Output may
// have been written before a failure, and the decoder then reads no further until it is reset. Once the stream ends,
// every read reports Done until the decoder is reset.
PrStatus prDeflateDecoderRead(PrDeflateDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
    size_t capacity, PrDeflateRead* out);

// How hard the encoder works to make its output small, against how long it takes.
enum {
    PrDeflateLevel_Fast,
    PrDeflateLevel_Default,
    PrDeflateLevel_Small,
};
typedef uint8_t PrDeflateLevel;

// Returns the largest number of bytes that prDeflateEncode writes for `size` bytes of input.
size_t prDeflateEncodeBound(size_t size);

// Encodes `input` as one stream into `output`, and returns the number of bytes written. `output` must have room for the
// number of bytes that prDeflateEncodeBound returns. Each block is stored, coded with the fixed codes, or coded with
// its own dynamic codes, whichever is smallest.
size_t prDeflateEncode(PrDeflateLevel level, PrByteView input, uint8_t* output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
