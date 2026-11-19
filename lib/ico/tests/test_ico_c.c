#include "pr/ico.h"

int prTestIcoCApi(void);

// The public header compiled as C. Bytes without a directory are refused.
int prTestIcoCApi(void) {
    static const uint8_t text[] = {0, 0, 2, 0, 1, 0};
    PrByteView bytes = {text, sizeof(text)};
    PrIcoDecoder* decoder = NULL;
    PrIcoLimits limits = prIcoLimitsDefault();
    int result = prIcoHasSignature(bytes) ||
        prIcoDecoderCreate(bytes, &limits, sizeof(text), &decoder) != PrStatus_NotSupported;
    prIcoDecoderDestroy(decoder);
    return result;
}
