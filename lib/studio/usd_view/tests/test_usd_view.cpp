// Studio's USD structure view.

#include "pr/string.h"
#include "pr/usd_stage.h"
#include "pr/usd_view.h"

#include "tools/harness/blob_fixture.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

extern "C" int prTestUsdViewCApi(void);

using namespace harness;

namespace pr_impl {

namespace {

// A root layer with a variant set and a reference to a second layer.
constexpr PrStringView kRoot = R"(#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Z"
)

def Xform "World"
{
    def Mesh "Car" (
        variants = {
            string color = "red"
        }
        prepend variantSets = "color"
        prepend references = @wheel.usda@
    )
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), (2, 2, 2)]
        token purpose = "render"
        rel material = </World/Look>
        variantSet "color" = {
            "red" {
                color3f displayColor = (1, 0, 0)
            }
        }
    }
    def Scope "Look"
    {
    }
}
)";

constexpr PrStringView kWheel = R"(#usda 1.0
(
    defaultPrim = "Wheel"
)

def Xform "Wheel"
{
    float radius = 0.5
}
)";

PrBlob* PR_NULLABLE readLayer(void*, PrStringView location) {
    PrStringView text = location == "root.usda" ? kRoot : location == "wheel.usda" ? kWheel : PrStringView{};
    if (text.data == nullptr)
        return nullptr;
    return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
}

String valueText(PrUsdValue value) {
    PrTextBuffer text = {};
    prUsdViewValueText(&value, &text);
    String result(prTextBufferView(&text));
    prTextBufferReset(&text);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestUsdViewCApi() == 0);
}

TEST_CASE("a structure view shows a prim's properties and opinions, and the stage's layers") {
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.open = [](void* PR_NULLABLE context, PrStringView location) -> const PrBlobSource* PR_NULLABLE {
        return harnessBlobSource(readLayer(context, location));
    };
    PrUsdStage* stage = nullptr;
    REQUIRE(prUsdStageCreate("root.usda", &options, &stage) == PrStatus_Ok);
    CHECK(prUsdStageErrorCount(stage) == 0);
    PrUsdPrimIndex car = prUsdStageFindPrim(stage, "/World/Car");
    REQUIRE(car != kPrUsdNone);

    KitFixture f;
    PrUsdView* view = prUsdViewCreate(f.kit);
    PrUsdPrimIndex prim = kPrUsdPseudoRoot;
    uint32_t property = kPrUsdNone;
    f.build = [&](PrLayoutNode root) {
        prUsdViewFrame(view, root, {.stage = stage, .prim = prim, .property = &property});
    };
    f.frame();
    CHECK(f.paintedCommands() > 0);
    prim = car;
    f.frame();
    // A property that the view does not show is no choice.
    property = prUsdStageFindProperty(stage, prUsdStageFindPrim(stage, "/World/Look"), "missing");
    f.frame();
    CHECK(property == kPrUsdNone);
    property = prUsdStageFindProperty(stage, car, "points");
    REQUIRE(property != kPrUsdNone);
    f.frame();
    CHECK(property == prUsdStageFindProperty(stage, car, "points"));

    PrTextBuffer label = {};
    prUsdViewPrimLabel(stage, car, &label);
    CHECK(prTextBufferView(&label) == "Car  Mesh");
    prTextBufferReset(&label);

    PrUsdScratch* scratch = prUsdScratchCreate();
    PrUsdValue value{};
    REQUIRE(prUsdStageAttributeValue(stage, car, property, kPrF64Nan, scratch, &value));
    CHECK(valueText(value) == "[5] (0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0), ...");
    uint32_t radius = prUsdStageFindProperty(stage, car, "radius");
    REQUIRE(prUsdStageAttributeValue(stage, car, radius, kPrF64Nan, scratch, &value));
    CHECK(valueText(value) == "0.5");
    uint32_t purpose = prUsdStageFindProperty(stage, car, "purpose");
    REQUIRE(prUsdStageAttributeValue(stage, car, purpose, kPrF64Nan, scratch, &value));
    CHECK(valueText(value) == "\"render\"");
    prUsdScratchDestroy(scratch);

    prUsdViewDestroy(view);
    prUsdStageDestroy(stage);
}
