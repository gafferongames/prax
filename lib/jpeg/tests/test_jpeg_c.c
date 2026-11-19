#include "pr/jpeg.h"

int prTestJpegCApi(void);

// The public header compiled as C. Bytes without the start of image are refused.
int prTestJpegCApi(void) {
    static const uint8_t text[] = {'j', 'p', 'e', 'g'};
    PrByteView bytes = {text, sizeof(text)};
    PrJpegDecoder* decoder = NULL;
    PrJpegLimits limits = prJpegLimitsDefault();
    int result = prJpegHasSignature(bytes) || prJpegDecoderCreate(bytes, &limits, sizeof(text), &decoder) !=
        PrStatus_NotSupported;
    prJpegDecoderDestroy(decoder);
    return result;
}
