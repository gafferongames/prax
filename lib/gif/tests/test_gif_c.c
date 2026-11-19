#include "pr/gif.h"

int prTestGifCApi(void);

// The public header compiled as C. Bytes without the signature are refused.
int prTestGifCApi(void) {
    static const uint8_t text[] = {'G', 'I', 'F', '9', '9', 'z'};
    PrByteView bytes = {text, sizeof(text)};
    PrGifDecoder* decoder = NULL;
    PrGifLimits limits = prGifLimitsDefault();
    int result = prGifHasSignature(bytes) ||
        prGifDecoderCreate(bytes, &limits, sizeof(text), &decoder) != PrStatus_NotSupported;
    prGifDecoderDestroy(decoder);
    return result;
}
