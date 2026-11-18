#include "pr/lzfse.h"

int prTestLzfseCApi(void);

// The public header compiled as C. A stream of one stored block and the end of stream decodes to the block's bytes.
int prTestLzfseCApi(void) {
    static const uint8_t stored[] = {'b', 'v', 'x', '-', 2, 0, 0, 0, 'h', 'i', 'b', 'v', 'x', '$'};
    uint8_t out[2] = {0};
    PrLzfseDecoder* decoder = prLzfseDecoderCreate();
    PrLzfseRead read;
    PrByteView input = {stored, sizeof(stored)};
    PrStatus status = prLzfseDecoderRead(decoder, input, true, out, sizeof(out), &read);
    prLzfseDecoderDestroy(decoder);
    return status == PrStatus_Ok && read.progress == PrLzfseProgress_Done && out[1] == 'i' ? 0 : 1;
}
