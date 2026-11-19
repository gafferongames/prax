#include "pr/usd.h"
#include "pr/usd_geom.h"
#include "pr/usd_stage.h"

int prTestUsdCApi(void);

// The public headers compile as C. A layer with one prim reads, and the prim is the layer spec's only child.
int prTestUsdCApi(void) {
    PrStringView text = PR_SV("#usda 1.0\ndef Xform \"World\"\n{\n}\n");
    PrByteView bytes = {(const uint8_t*)text.data, text.size};
    PrBlob* file = prBlobCopy(bytes);
    PrUsdLayer* layer = NULL;
    int result = prUsdLayerCreate(file, NULL, &layer) != PrStatus_Ok;
    prBlobRelease(file);
    if (layer == NULL)
        return 1;
    result |= prUsdLayerSpecCount(layer) != 2;
    result |= prUsdLayerSpecs(layer)[kPrUsdLayerSpec].prims.count != 1;
    result |= prUsdLayerFindSpec(layer, PR_SV("/World")) != 1;
    result |= prUsdLayerSpecs(layer)[1].form != PrUsdSpecForm_Prim;
    result |= prUsdLayerFormat(layer) != PrUsdFormat_Text;
    result |= prUsdTypeSize(PrUsdType_Float3) != 12;
    result |= !prUsdGeomKindIsXformable(PrUsdGeomKind_Mesh) || PrUsdPreviewInput_Count != 13;
    prUsdLayerDestroy(layer);
    return result;
}
