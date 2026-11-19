// LZFSE: decoding Apple's LZ77 with finite state entropy coding, and its LZVN and stored blocks, a piece of input at a
// time. Apple's reference implementation (github.com/lzfse/lzfse) is the format's specification.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A decoder. It holds the decoding state, the output of earlier reads that back-references read, and a compressed block
// that spans reads. One thread uses a decoder at a time.
typedef struct PrLzfseDecoder PrLzfseDecoder;

// What a read needs next, or that the stream ended.
enum {
    PrLzfseProgress_Input,      // more input
    PrLzfseProgress_Output,     // more room for output
    PrLzfseProgress_Done,       // the stream ended
};
typedef uint8_t PrLzfseProgress;

// The result of a read.
typedef struct PrLzfseRead {
    size_t consumed;            // number of input bytes read
    size_t written;             // number of output bytes written
    PrLzfseProgress progress;
} PrLzfseRead;

// Creates a decoder at the start of a stream.
PrLzfseDecoder* prLzfseDecoderCreate(void);

// Starts a new stream, keeping the storage.
void prLzfseDecoderReset(PrLzfseDecoder* decoder);

// Frees the decoder.
void prLzfseDecoderDestroy(PrLzfseDecoder* PR_NULLABLE decoder);

// Decodes from `input` into up to `capacity` bytes at `output`, and writes the result to `*read`. `final` means no
// input follows `input`. Bytes after the stream's end are left unread.
//
// Returns InvalidData if the stream is corrupt, and Truncated if the final input ends inside the stream. Output may
// have been written before a failure, and the decoder then reads no further until it is reset. Once the stream ends,
// every read reports Done until the decoder is reset.
PrStatus prLzfseDecoderRead(PrLzfseDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
    size_t capacity, PrLzfseRead* read);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
