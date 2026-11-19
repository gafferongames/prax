#include "pr/model.h"

int prTestModelCApi(void);

// The public header compiled as C. Checks the descriptions that a caller fills in.
int prTestModelCApi(void) {
    PrModelOptions options = prModelOptionsDefault();
    PrRenderScene none = {0};
    PrModelPlacementDesc placement = prModelPlacementDescDefault(none);
    PrBox3 box = {.min = {0, 0, 0}, .max = {1, 2, 3}};
    PrModelProgress progress = {.meshCount = 2};
    prModelDestroy(NULL);
    prModelPlacementDestroy(NULL);
    return options.pool != NULL || !placement.addsLights ||
        placement.root.m[2][2] != 1 || placement.visibility != 0xffffffffu || box.max.z != 3 || progress.meshCount != 2;
}
