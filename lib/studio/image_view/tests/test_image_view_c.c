#include "pr/image_view.h"

int prTestImageViewCApi(void);

// The public header compiled as C.
int prTestImageViewCApi(void) {
    PrImageViewDesc desc = {0};
    return desc.kit != NULL;
}
