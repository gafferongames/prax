#include "pr/backend.h"

int prTestBackendCApi(void);

// The public header compiled as C.
int prTestBackendCApi(void) {
    const PrStringView names[] = {PR_SV("default"), PR_SV("memory")};
    PrBackendInterface interface = {.name = PR_SV("gpu"), .names = names, .count = 2, .built = 2u};
    int result = prBackendFromName(&interface, PR_SV("memory")) != 1;
    result |= !prBackendIsBuilt(&interface, 1);
    return result;
}
