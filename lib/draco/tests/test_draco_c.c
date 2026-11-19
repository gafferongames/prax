#include "pr/draco.h"

int prTestDracoCApi(void);

// The public header compiled as C. A stream that is not Draco's is refused, and the limits have their defaults.
int prTestDracoCApi(void) {
    static const uint8_t bytes[] = {'D', 'R', 'A', 'C'};
    PrByteView stream = {bytes, sizeof(bytes)};
    PrDracoLimits limits = prDracoLimitsDefault();
    PrDraco* draco = NULL;
    int result = prDracoCreate(stream, &limits, &draco) != PrStatus_InvalidData;
    result |= draco != NULL;
    result |= limits.maxFaces != 1u << 24;
    result |= prDracoComponentTypeSize(PrDracoComponentType_Float64) != 8;
    result |= kPrDracoNone != 0xffffffffu;
    prDracoDestroy(draco);
    return result;
}
