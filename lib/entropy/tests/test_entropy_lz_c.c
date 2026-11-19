#include "pr/entropy_lz.h"

int prTestEntropyCApi(void);

// Copies a match through the C API, and returns zero if it repeats the pattern.
int prTestEntropyCApi(void) {
    uint8_t buffer[16 + kPrEntropyLzMatchSlack] = {1, 2, 3};
    prEntropyLzCopyMatch(buffer + 3, 3, 12);
    for (uint32_t i = 0; i < 15; ++i) {
        if (buffer[i] != (uint8_t)(i % 3 + 1))
            return 1;
    }
    return 0;
}
