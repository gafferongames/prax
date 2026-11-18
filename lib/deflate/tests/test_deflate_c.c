#include "pr/deflate.h"

int prTestDeflateCApi(void);

// The public headers compiled as C. A stream encoded whole decodes back to its input.
int prTestDeflateCApi(void) {
    static const uint8_t text[] = {'h', 'e', 'l', 'l', 'o', ',', ' ', 'h', 'e', 'l', 'l', 'o'};
    uint8_t encoded[64];
    PrByteView input = {text, sizeof(text)};
    size_t size = prDeflateEncode(PrDeflateLevel_Default, input, encoded);
    PrDeflateDecoder* decoder = prDeflateDecoderCreate();
    uint8_t decoded[sizeof(text)];
    PrDeflateRead read;
    PrByteView stream = {encoded, size};
    PrStatus status = prDeflateDecoderRead(decoder, stream, true, decoded, sizeof(decoded), &read);
    prDeflateDecoderDestroy(decoder);
    return status == PrStatus_Ok && read.progress == PrDeflateProgress_Done && read.written == sizeof(text) ? 0 : 1;
}
