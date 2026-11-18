#include "pr/emjc.h"

int prTestEmjcCApi(void);

// The public header compiled as C. Bytes without the signature are refused.
int prTestEmjcCApi(void) {
    static const uint8_t text[] = {'e', 'm', 'j', '0'};
    PrByteView bytes = {text, sizeof(text)};
    PrCompressCodecSet* compress = prCompressCodecSetCreate(NULL, 0);
    PrEmjcDecoder* decoder = NULL;
    PrEmjcLimits limits = prEmjcLimitsDefault();
    int result = prEmjcHasSignature(bytes) ||
        prEmjcDecoderCreate(bytes, compress, &limits, sizeof(text), &decoder) != PrStatus_NotSupported;
    prEmjcDecoderDestroy(decoder);
    prCompressCodecSetDestroy(compress);
    return result;
}
