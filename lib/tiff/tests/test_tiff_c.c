#include "pr/tiff.h"

int prTestTiffCApi(void);

// The public header compiled as C. Bytes without a header are refused.
int prTestTiffCApi(void) {
    static const uint8_t text[] = {'I', 'I', 0, 0};
    PrByteView bytes = {text, sizeof(text)};
    PrTiffDecoder* decoder = NULL;
    PrTiffLimits limits = prTiffLimitsDefault();
    int result = prTiffHasSignature(bytes) ||
        prTiffDecoderCreate(bytes, &limits, sizeof(text), &decoder) != PrStatus_NotSupported;
    prTiffDecoderDestroy(decoder);
    return result;
}
