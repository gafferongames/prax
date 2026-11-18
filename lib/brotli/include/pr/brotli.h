// Brotli (rfc 7932): decoding a stream a piece of input at a time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A decoder. It holds the decoding state, the window of output that back-references read, and the input of an
// incomplete step, which is at most a few bytes. One thread uses a decoder at a time.
typedef struct PrBrotliDecoder PrBrotliDecoder;

// What a read needs next, or that the stream ended.
enum {
    PrBrotliProgress_Input,     // more input
    PrBrotliProgress_Output,    // more room for output
    PrBrotliProgress_Done,      // the stream ended
};
typedef uint8_t PrBrotliProgress;

// The result of a read.
typedef struct PrBrotliRead {
    size_t consumed;            // number of input bytes read
    size_t written;             // number of output bytes written
    PrBrotliProgress progress;
} PrBrotliRead;

// Creates a decoder at the start of a stream.
PrBrotliDecoder* prBrotliDecoderCreate(void);

// Starts a new stream, keeping the storage.
void prBrotliDecoderReset(PrBrotliDecoder* decoder);

// Frees the decoder.
void prBrotliDecoderDestroy(PrBrotliDecoder* PR_NULLABLE decoder);

// Decodes from `input` into up to `capacity` bytes at `output`, and writes the result to `*read`. `final` means no
// input follows `input`. Bytes after the stream's end are left unread.
//
// Returns InvalidData if the stream is corrupt, and Truncated if the final input ends inside the stream. Output may
// have been written before a failure, and the decoder then reads no further until it is reset. Once the stream ends,
// every read reports Done until the decoder is reset.
PrStatus prBrotliDecoderRead(PrBrotliDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
    size_t capacity, PrBrotliRead* read);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
