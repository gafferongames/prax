#include "pr/gltf.h"
#include "pr/gltf_model.h"

int prTestGltfCApi(void);

// The public headers compile as C. An asset with one node reads, and the node has no mesh.
int prTestGltfCApi(void) {
    PrStringView text = PR_SV("{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"name\":\"root\"}]}");
    PrByteView bytes = {(const uint8_t*)text.data, text.size};
    PrBlob* file = prBlobCopy(bytes);
    PrGltf* gltf = NULL;
    int result = prGltfCreate(file, NULL, &gltf) != PrStatus_Ok;
    prBlobRelease(file);
    if (gltf == NULL)
        return 1;
    result |= prGltfNodeCount(gltf) != 1;
    result |= prGltfNodes(gltf)[0].mesh != kPrGltfNone;
    result |= prGltfNodes(gltf)[0].name.size != 4;
    result |= prGltfDefaultScene(gltf) != kPrGltfNone;
    result |= !prGltfExtensionIsSupported(PR_SV("KHR_materials_unlit"));
    prGltfDestroy(gltf);
    return result;
}
