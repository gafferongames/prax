#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_records.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_shaders.h"
#include "pr/render_view.h"

int prTestRenderCApi(void);

// The public headers compiled as C. Checks the descriptions a caller fills in, and the layout of the records.
int prTestRenderCApi(void) {
    PrRenderGeometryVertex vertex = {.position = {1, 2, 3}, .normal = {0, 32767, 0, 0}};
    PrRenderMeshPart part = {.firstIndex = 0, .indexCount = 3};
    PrRenderMesh none = {0};
    PrRenderInstanceDesc instance = prRenderInstanceDescDefault(none);
    PrRenderViewDesc view = {.samples = 4, .extent = {64, 32}};
    PrRenderMaterialRecord record = {.variant = 1};
    PrRenderEnvironment black = {0};
    PrRenderSceneEnvironment environment = prRenderSceneEnvironmentDefault(black);
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    PrRenderDisplayOptions display = prRenderDisplayOptionsDefault();
    PrRenderMaterialFlags flags = PrRenderMaterialFlag_DoubleSided | PrRenderMaterialFlag_ReadsBehind;
    prRenderDestroy(NULL);
    prRenderUpdateDiscard(NULL);
    return sizeof(vertex) != kPrRenderGeometryBytes || part.indexCount != 3 || instance.mesh.id != 0 ||
        view.extent.x != 64 || sizeof(record) != kPrRenderMaterialBytes ||
        sizeof(PrRenderInstanceRecord) != kPrRenderInstanceBytes || environment.intensity != 1 ||
        sky.sunIrradiance != kPrRenderSunIlluminance || display.headroom != 1 || flags != 3 ||
        prRenderGeometryLayout()->stride != kPrRenderGeometryBytes;
}
