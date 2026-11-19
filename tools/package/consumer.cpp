// A program over the libraries' public headers, as an application includes them: it makes and frees what it calls in
// the C API, on the backends that the package's build has, and keeps a function of the HTML engine's QuickJS layer, so
// that the link resolves that layer and everything that it links. It also reads the shader set that its build compiled
// for lib/render with a material type of its own, converts its own vertices into the layout of the attributes that the
// type declares, and keeps the function that creates a renderer with it.

#include "pr/draw_list.h"
#include "pr/font_hinter.h"
#include "pr/html_quickjs.h"
#include "pr/render.h"
#include "pr/render_shaders.h"
#include "pr/text.h"

#include "consumer/consumer_materials.h"
#include "render_shaders/ConsumerShaders.h"

#include <stdio.h>

using namespace pr;

int main() {
    PrDrawList* list = prDrawListCreate();
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrFontHinter* hinter = prFontHinterCreate(PrFontHinterBackend_Default);
    PrJsHtmlEngine* (*volatile create)(const PrJsHtmlEngineDesc*) = &prJsHtmlEngineCreate;
    // The set has the library's material types and the consumer's, at the id that the set's header gives it, whose
    // record is the size that C gives it, and a library for each pass on a build with a GPU backend.
    const PrRenderShaderSet* shaders = consumerShaderSet();
    PrStatus (*volatile createRender)(PrGpuDevice*, const PrRenderOptions*, PrRender**) = &prRenderCreate;
#if defined(CONSUMER_HAS_SHADER_TARGETS)
    bool hasLibraries = shaders->handles[PrRenderShaderPass_Forward].size != 0;
#else
    bool hasLibraries = true;
#endif
    bool hasShaders = createRender != nullptr && hasLibraries && shaders->materialTypeCount == kConsumerMaterialCount &&
        kConsumerMaterialStripes < shaders->materialTypeCount &&
        shaders->materialTypes[kConsumerMaterialStripes].parameterBytes == sizeof(ConsumerStripesParameters);
    // The consumer holds its distances as doubles, which the layout's fill converts into the float that the type
    // declares.
    const PrRenderVertexLayout* layout = &shaders->layouts[shaders->materialTypes[kConsumerMaterialStripes].layout];
    const double distances[3] = {0, 0.5, 2.25};
    const PrRenderAttributeSource source{
        PrRenderSemantic_Custom, 0, "along",
        {distances, 3, sizeof(double), {.component = PrMeshComponent_Float64, .count = 1}},
    };
    float along[3] = {};
    uint32_t missing = 1;
    bool converted = layout->attributeCount == 1 && layout->stride == sizeof(float) &&
        prRenderVertexAttributeFromName(layout, "along") == kConsumerStripesAttributeAlong &&
        prRenderVertexLayoutFill(layout, &source, 1, 3, along, &missing) == PrStatus_Ok && missing == 0 &&
        along[1] == 0.5f && along[2] == 2.25f;
    hasShaders = hasShaders && converted;
    printf("consumer: a shader set of %u material types, with a forward library of %zu bytes\n",
        shaders->materialTypeCount, shaders->handles[PrRenderShaderPass_Forward].size);
    bool ok = shaper != nullptr && create != nullptr && hasShaders;
    prFontHinterDestroy(hinter);
    prTextShaperDestroy(shaper);
    prDrawListRelease(list);
    printf("consumer: %s\n",
        ok ? "ok" : shaper == nullptr ? "no shaper" : create == nullptr ? "no HTML engine" : "no shader set");
    return ok ? 0 : 1;
}
