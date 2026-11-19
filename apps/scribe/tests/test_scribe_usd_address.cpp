// The address handler of USD layers and packages: scene paths of their stages, a package's files, and what a layer
// reads beside it.

#include "pr/compress_select.h"
#include "pr/string.h"

#include "apps/scribe/tests/address_fixture.h"
#include "apps/scribe/usd_address.h"
#include "apps/scribe/zip_address.h"
#include "tools/harness/zip_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace scribe;

namespace {

// A layer with a prim of a variant set, a property, and a reference to a layer beside it.
constexpr PrStringView kScene = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Car" (
        variants = {
            string color = "red"
        }
        prepend variantSets = "color"
        prepend references = @./parts/wheel.usda@
    )
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        variantSet "color" = {
            "red" {
                def Scope "Paint"
                {
                }
            }
            "blue" {
            }
        }
    }
}
)";

// The layer that the scene references.
constexpr PrStringView kWheel = R"(#usda 1.0
(
    defaultPrim = "Wheel"
)

def Xform "Wheel"
{
    float radius = 0.5
}
)";

// The handlers of USD and of ZIP archives, over a codec of their own.
struct UsdAddresses {
    PrCompressCodecSet* codec = prCompressCodecSetCreateDefault();
    AddressHandler* handlers[2] = {zipAddressHandlerCreate(codec), usdAddressHandlerCreate(codec)};
    AddressFixture f{handlers};

    UsdAddresses() = default;
    UsdAddresses(const UsdAddresses&) = delete;
    UsdAddresses& operator=(const UsdAddresses&) = delete;
    ~UsdAddresses() {
        addressResolverDestroy(f.resolver);
        f.resolver = nullptr;
        for (AddressHandler* handler : f.handlers)
            addressHandlerDestroy(handler);
        f.handlers.clear();
        prCompressCodecSetDestroy(codec);
    }
};

} // namespace

TEST_CASE("a scene path names a location of a layer's stage") {
    UsdAddresses u;
    u.f.set("file:///assets/scene.usda", kScene);
    u.f.set("file:///assets/parts/wheel.usda", kWheel);
    AddressNode* node = nullptr;
    REQUIRE(u.f.resolve("file:///assets/scene.usda#/World/Car", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Location);
    AddressNode* layer = addressNodeParent(node);
    CHECK(addressNodeFormat(layer) == PrMimeFormat_Usda);
    const PrUsdStage* stage = usdAddressStage(layer);
    REQUIRE(stage != nullptr);
    // The reference beside the layer composed.
    CHECK(prUsdStageErrorCount(stage) == 0);
    CHECK(prUsdStageLayerCount(stage) == 2);
    addressNodeRelease(node);

    CHECK(u.f.status("file:///assets/scene.usda#/World/Car.points") == PrStatus_Ok);
    CHECK(u.f.status("file:///assets/scene.usda#/World/Car.radius") == PrStatus_Ok);
    CHECK(u.f.status("file:///assets/scene.usda#/World/Car{color=red}Paint") == PrStatus_Ok);
    uint32_t failed = 0;
    CHECK(u.f.status("file:///assets/scene.usda#/World/Truck", &failed) == PrStatus_NotFound);
    CHECK(failed == 0);
    CHECK(u.f.status("file:///assets/scene.usda#/World/Car{color") == PrStatus_Syntax);
    // A layer is no package, so a step that is not a path names nothing in it.
    CHECK(u.f.status("file:///assets/scene.usda#wheel.usda") == PrStatus_Syntax);
}

TEST_CASE("a layer's stage reads a layer that a path names above the layer's folder") {
    UsdAddresses u;
    u.f.set("file:///assets/tests/scene.usda", "#usda 1.0\nover \"Car\" (\n    references = @../parts/wheel.usda@\n)\n"
        "{\n}\n");
    u.f.set("file:///assets/parts/wheel.usda", kWheel);
    AddressNode* node = nullptr;
    REQUIRE(u.f.resolve("file:///assets/tests/scene.usda", &node) == PrStatus_Ok);
    const PrUsdStage* stage = usdAddressStage(node);
    REQUIRE(stage != nullptr);
    CHECK(prUsdStageErrorCount(stage) == 0);
    CHECK(prUsdStageLayerCount(stage) == 2);
    // The reference defines the prim that the layer only overrides.
    PrUsdPrimIndex car = prUsdStageFindPrim(stage, "/Car");
    REQUIRE(car != kPrUsdNone);
    CHECK((prUsdStagePrims(stage)[car].flags & PrUsdPrimFlag_Defined) != 0);
    addressNodeRelease(node);
}

TEST_CASE("a package's steps are its files, or scene paths of its default layer's stage") {
    UsdAddresses u;
    ZipFixtureFile files[] = {{"scene.usda", kScene}, {"parts/wheel.usda", kWheel}};
    u.f.set("file:///assets/car.usdz", zipFixture(files));
    AddressNode* node = nullptr;
    REQUIRE(u.f.resolve("file:///assets/car.usdz#/World/Car.points", &node) == PrStatus_Ok);
    AddressNode* package = addressNodeParent(node);
    CHECK(addressNodeFormat(package) == PrMimeFormat_Usdz);
    const PrUsdStage* stage = usdAddressStage(package);
    REQUIRE(stage != nullptr);
    CHECK(prUsdStageErrorCount(stage) == 0);
    CHECK(prUsdStageFindPrim(stage, "/World/Car") != kPrUsdNone);
    REQUIRE(addressNodeChildCount(package) == 2);
    CHECK(addressNodeChild(package, 1).step == "parts/wheel.usda");
    addressNodeRelease(node);

    // A file of the package is a layer of its own, which composes a stage with itself as the root.
    CHECK(u.f.status("file:///assets/car.usdz#parts/wheel.usda!/Wheel.radius") == PrStatus_Ok);
    REQUIRE(u.f.resolve("file:///assets/car.usdz#scene.usda", &node) == PrStatus_Ok);
    CHECK(addressNodeFormat(node) == PrMimeFormat_Usda);
    stage = usdAddressStage(node);
    REQUIRE(stage != nullptr);
    // Its reference is read beside it, in the package.
    CHECK(prUsdStageErrorCount(stage) == 0);
    CHECK(prUsdStageFindProperty(stage, prUsdStageFindPrim(stage, "/World/Car"), "radius") != kPrUsdNone);
    // The opener of the stage's model reads what the stage read.
    const PrBlobSource* opened = usdAddressOpen(node, "parts/wheel.usda");
    CHECK(opened != nullptr);
    prBlobSourceRelease(opened);
    addressNodeRelease(node);
    CHECK(u.f.status("file:///assets/car.usdz#missing.usda") == PrStatus_NotFound);
}

TEST_CASE("a deep chain reaches a property of a layer in a package in archives") {
    UsdAddresses u;
    ZipFixtureFile layers[] = {{"scene.usda", kScene}, {"parts/wheel.usda", kWheel}};
    String package = zipFixture(layers);
    ZipFixtureFile inner[] = {{"assets/car.usdz", package}};
    String innerZip = zipFixture(inner, u.codec);
    ZipFixtureFile outer[] = {{"inner.zip", innerZip, true}};
    u.f.set("file:///outer.zip", zipFixture(outer, u.codec));
    AddressNode* node = nullptr;
    PrStringView text = "file:///outer.zip#inner.zip!assets/car.usdz!scene.usda!/World/Car{color=red}Paint";
    REQUIRE(u.f.resolve(text, &node) == PrStatus_Ok);
    CHECK(addressText(addressNodeAddress(node)) == text);
    // Each step's resource, from the location up.
    PrMimeFormat formats[] = {
        PrMimeFormat_None, PrMimeFormat_Usda, PrMimeFormat_Usdz, PrMimeFormat_Zip,
        PrMimeFormat_Zip,
    };
    AddressNode* at = node;
    for (PrMimeFormat format : formats) {
        REQUIRE(at != nullptr);
        CHECK(addressNodeFormat(at) == format);
        at = addressNodeParent(at);
    }
    CHECK(at == nullptr);
    addressNodeRelease(node);
    uint32_t failed = 0;
    CHECK(u.f.status("file:///outer.zip#inner.zip!assets/car.usdz!scene.usda!/World/Car/Hood", &failed) ==
        PrStatus_NotFound);
    CHECK(failed == 3);
}
