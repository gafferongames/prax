#include "pr/usd_view.h"

int prTestUsdViewCApi(void);

// The public header compiled as C.
int prTestUsdViewCApi(void) {
    PrUsdValue value = {0};
    value.type = PrUsdType_Block;
    PrTextBuffer text = {0};
    prUsdViewValueText(&value, &text);
    int result = text.size == 0;
    prTextBufferReset(&text);
    return result;
}
