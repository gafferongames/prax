#include "pr/brotli.h"

int prTestBrotliCApi(void);

// The public header compiled as C. The smallest stream, an empty last metablock, decodes to nothing.
int prTestBrotliCApi(void) {
    static const uint8_t empty[] = {0x06};
    PrBrotliDecoder* decoder = prBrotliDecoderCreate();
    PrBrotliRead read;
    PrByteView input = {empty, sizeof(empty)};
    PrStatus status = prBrotliDecoderRead(decoder, input, true, NULL, 0, &read);
    prBrotliDecoderDestroy(decoder);
    return status == PrStatus_Ok && read.progress == PrBrotliProgress_Done && read.written == 0 ? 0 : 1;
}
