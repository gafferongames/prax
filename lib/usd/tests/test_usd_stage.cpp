#include "pr/array.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/raw_string.h"
#include "pr/time.h"
#include "pr/usd_stage.h"
#include "pr/vec.h"

#include "usd/tests/usd_fixture.h"

using namespace harness;

namespace pr_impl {

namespace {

// The layers of a test, by location, which a stage reads through `read`.
struct Layers {
    Array<String> locations;
    Array<String> texts;
    Array<String> opened;

    // Adds the text layer that `body` makes after the header, at `/name`.
    Layers& add(PrStringView name, PrStringView body) {
        String location("/");
        location.append(name);
        locations.push(move(location));
        // A test writes a body on one line, and the format wants a line break before a closing brace.
        String text("#usda 1.0\n");
        for (size_t i = 0; i < body.size; ++i) {
            if (body.data[i] == ' ' && i + 1 < body.size && body.data[i + 1] == '}')
                text.append("\n");
            else
                text.append(PrStringView{body.data + i, 1});
        }
        texts.push(move(text));
        return *this;
    }

    static PrBlob* PR_NULLABLE read(void* PR_NULLABLE context, PrStringView location) {
        Layers* layers = static_cast<Layers*>(context);
        layers->opened.push(String(location));
        for (uint32_t i = 0; i < layers->locations.count(); ++i) {
            if (layers->locations[i] == location) {
                const String& text = layers->texts[i];
                return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.count()});
            }
        }
        return nullptr;
    }
};

// A stage composed from a test's layers, with what the tests ask of it as text.
struct Composed {
    PrUsdStage* PR_NULLABLE stage = nullptr;
    PrUsdScratch* scratch = prUsdScratchCreate();

    Composed(Layers& layers, PrUsdStageOptions options = prUsdStageOptionsDefault()) {
        options.open = [](void* PR_NULLABLE context, PrStringView location) -> const PrBlobSource* PR_NULLABLE {
            return harnessBlobSource(Layers::read(context, location));
        };
        options.openContext = &layers;
        REQUIRE(prUsdStageCreate("/root.usda", &options, &stage) == PrStatus_Ok);
    }
    Composed(const Composed&) = delete;
    ~Composed() {
        prUsdStageDestroy(stage);
        prUsdScratchDestroy(scratch);
    }

    PrUsdPrimIndex find(PrStringView path) const {
        return prUsdStageFindPrim(stage, path);
    }

    const PrUsdPrim& prim(PrStringView path) const {
        PrUsdPrimIndex index = find(path);
        REQUIRE(index != kPrUsdNone);
        return prUsdStagePrims(stage)[index];
    }

    String layerName(uint32_t layer) const {
        PrStringView name = prFilePathName(prUsdStageLayerLocation(stage, layer));
        return String(PrStringView{name.data, name.size - prFilePathExtension(name).size});
    }

    // The opinions of a prim, strongest first, each as `layer:/spec/path`.
    String opinions(PrStringView path) const {
        const PrUsdPrim& p = prim(path);
        String text;
        for (uint32_t i = 0; i < p.opinionCount; ++i) {
            PrUsdOpinion opinion = prUsdStageOpinion(stage, p.firstOpinion + i);
            if (i != 0)
                text.append(" ");
            text.append(layerName(opinion.layer));
            text.append(":");
            text.append(prUsdLayerSpecs(prUsdStageLayer(stage, opinion.layer))[opinion.spec].path);
        }
        return text;
    }

    String children(PrStringView path) const {
        String text;
        for (PrUsdPrimIndex child = prim(path).firstChild; child != kPrUsdNone;
                child = prUsdStagePrims(stage)[child].nextSibling) {
            if (text.count() != 0)
                text.append(" ");
            text.append(prUsdStagePrims(stage)[child].name);
        }
        return text;
    }

    String properties(PrStringView path) const {
        const PrUsdPrim& p = prim(path);
        String text;
        for (uint32_t i = 0; i < p.propertyCount; ++i) {
            if (i != 0)
                text.append(" ");
            text.append(prUsdStageProperties(stage)[p.firstProperty + i].name);
        }
        return text;
    }

    uint32_t property(PrStringView path, PrStringView name) const {
        uint32_t index = prUsdStageFindProperty(stage, find(path), name);
        REQUIRE(index != kPrUsdNone);
        return index;
    }

    // The value of a `double` attribute at a time, or at the default time for a NaN.
    double number(PrStringView path, PrStringView name, double time) const {
        PrUsdValue value{};
        REQUIRE(prUsdStageAttributeValue(stage, find(path), property(path, name), time, scratch, &value));
        REQUIRE(value.type == PrUsdType_Double);
        return *static_cast<const double*>(value.data);
    }

    bool hasValue(PrStringView path, PrStringView name, double time) const {
        PrUsdValue value{};
        return prUsdStageAttributeValue(stage, find(path), property(path, name), time, scratch, &value);
    }

    // The elements of a resolved array of strings, tokens or paths, with spaces between them.
    String joined(const PrUsdValue& value) const {
        String text;
        for (uint32_t i = 0; i < value.count; ++i) {
            if (i != 0)
                text.append(" ");
            text.append(static_cast<const PrStringView*>(value.data)[i]);
        }
        return text;
    }

    String targets(PrStringView path, PrStringView name, bool forward = false) const {
        PrUsdValue value{};
        prUsdStageTargets(stage, find(path), property(path, name), forward, scratch, &value, nullptr);
        return joined(value);
    }

    bool hasError(PrUsdErrorKind kind) const {
        for (uint32_t i = 0; i < prUsdStageErrorCount(stage); ++i) {
            if (prUsdStageErrors(stage)[i].kind == kind)
                return true;
        }
        return false;
    }
};

const double kDefaultTime = kPrF64Nan;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a stage's prims are depth-first, with the pseudo-root first") {
    Layers layers;
    layers.add("root.usda", "def Xform \"A\" { def \"B\" { def \"C\" {} }\ndef \"D\" {} }\ndef \"E\" {}\n");
    Composed c(layers);
    REQUIRE(prUsdStagePrimCount(c.stage) == 6);
    const PrUsdPrim* prims = prUsdStagePrims(c.stage);
    CHECK(prims[kPrUsdPseudoRoot].name == "");
    CHECK(prims[kPrUsdPseudoRoot].parent == kPrUsdNone);
    CHECK(prims[kPrUsdPseudoRoot].end == 6);
    CHECK(c.children("/") == "A E");
    CHECK(c.children("/A") == "B D");
    CHECK(c.prim("/A").typeName == "Xform");
    CHECK(c.prim("/A").end == c.find("/E"));
    CHECK(c.prim("/A/B/C").parent == c.find("/A/B"));
    CHECK(c.find("/A/Missing") == kPrUsdNone);
    PrTextBuffer path = {};
    prUsdStagePrimPath(c.stage, c.find("/A/B/C"), &path);
    CHECK(prTextBufferView(&path) == "/A/B/C");
    prTextBufferReset(&path);
    CHECK(prUsdStageErrorCount(c.stage) == 0);
    CHECK(prUsdStageLayerCount(c.stage) == 1);
}

TEST_CASE("sublayers make a layer stack, strongest first, each layer read once") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./a.usda@, @./b.usda@])\ndef \"P\" {}\n");
    layers.add("a.usda", "(subLayers = [@./c.usda@])\nover \"P\" {}\n");
    layers.add("b.usda", "(subLayers = [@./c.usda@])\nover \"P\" {}\n");
    layers.add("c.usda", "over \"P\" {}\n");
    Composed c(layers);
    CHECK(c.opinions("/P") == "root:/P a:/P c:/P b:/P c:/P");
    CHECK(layers.opened.count() == 4);
}

TEST_CASE("a sublayer's offset maps its times to its parent's, and composes down the stack") {
    // The specification's example of sublayer offsets (usd-core/#sublayer-time-offsets): a time of the sublayer, scaled
    // and then offset, is a time of the layer that brings it in.
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./a.usda@ (offset = 5; scale = 2), @./b.usda@])\n");
    layers.add("a.usda", "(subLayers = [@./c.usda@ (offset = 10; scale = 3)])\n");
    layers.add("b.usda", "def \"P\" { double y.timeSamples = { 1: 1 } }\n");
    layers.add("c.usda", "def \"P\" { double x.timeSamples = { 0: 0, 10: 100 } }\n");
    Composed c(layers);
    const PrUsdPrim& prim = c.prim("/P");
    PrUsdOpinion deep = prUsdStageOpinion(c.stage, prim.firstOpinion);
    CHECK(deep.offset.offset == 25.0);
    CHECK(deep.offset.scale == 6.0);
    CHECK(c.number("/P", "x", 25.0) == 0.0);
    CHECK(c.number("/P", "x", 85.0) == 100.0);
    CHECK(c.number("/P", "x", 55.0) == 50.0);
    const double* times = nullptr;
    REQUIRE(prUsdStageAttributeTimes(c.stage, c.find("/P"), c.property("/P", "x"), -1e9, 1e9, c.scratch, &times) == 2);
    CHECK(times[0] == 25.0);
    CHECK(times[1] == 85.0);
    CHECK(prUsdStageAttributeTimes(c.stage, c.find("/P"), c.property("/P", "x"), 30.0, 90.0, c.scratch, &times) == 1);
    CHECK(times[0] == 85.0);
}

TEST_CASE("a layer with another time scale is scaled to the layer that brings it in") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./fast.usda@])\ndef \"R\" (references = @./ref.usda@</R>) {}\n");
    layers.add("fast.usda", "(timeCodesPerSecond = 48)\ndef \"P\" { double x.timeSamples = { 0: 0, 48: 48 } }\n");
    layers.add("ref.usda", "(framesPerSecond = 12)\ndef \"R\" { double x.timeSamples = { 0: 0, 12: 12 } }\n");
    Composed c(layers);
    CHECK(prUsdStageTimeCodesPerSecond(c.stage) == 24.0);
    CHECK(c.number("/P", "x", 24.0) == 48.0);
    CHECK(c.number("/P", "x", 12.0) == 24.0);
    CHECK(c.number("/R", "x", 24.0) == 12.0);
}

TEST_CASE("a reference's times are scaled from its target's time scale to that of the layer that authors it") {
    // The sublayer runs at 48 time codes per second in a stack that runs at 24, and the targets run at 24. The arcs
    // that the sublayer authors scale their targets' times to its own, and its retiming halves them again
    // (usd-core/#reference-and-payload-offset-adjustment). An arc within the layer stack is scaled as well.
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./fast.usda@])\ndef \"Src\" { double x.timeSamples = { 0: 0, 24: 24 } }\n");
    layers.add("fast.usda", "(timeCodesPerSecond = 48)\ndef \"Out\" (references = @./ref.usda@</R> (offset = 4)) {}\n"
        "def \"In\" (references = </Src>) {}\n");
    layers.add("ref.usda", "def \"R\" { double x.timeSamples = { 0: 0, 24: 24 } }\n");
    Composed c(layers);
    CHECK(c.number("/Out", "x", 2.0) == 0.0);
    CHECK(c.number("/Out", "x", 26.0) == 24.0);
    CHECK(c.number("/In", "x", 12.0) == 12.0);
    CHECK(c.number("/In", "x", 24.0) == 24.0);
}

TEST_CASE("a sublayer that cannot be used is an error, and the stack goes on without it") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./missing.usda@, @./a.usda@, @./bad.usda@ (scale = -1)])\ndef \"P\" {}\n");
    layers.add("a.usda", "(subLayers = [@./root.usda@])\nover \"P\" {}\n");
    layers.add("bad.usda", "over \"P\" {}\n");
    Composed c(layers);
    CHECK(c.opinions("/P") == "root:/P a:/P bad:/P");
    CHECK(c.hasError(PrUsdErrorKind_SublayerUnreadable));
    CHECK(c.hasError(PrUsdErrorKind_SublayerCycle));
    CHECK(c.hasError(PrUsdErrorKind_InvalidOffset));
    CHECK(prUsdStageErrorCount(c.stage) == 3);
    PrUsdOpinion bad = prUsdStageOpinion(c.stage, c.prim("/P").firstOpinion + 2);
    CHECK(bad.offset.scale == 1.0);
}

TEST_CASE("references bring the opinions of their targets, in the order of their list") {
    // The specification's example (usd-core/#references).
    Layers layers;
    layers.add("root.usda", "def \"LocalRef\" {}\n"
        "def \"Root\" (references = [</LocalRef>, @./ref_1.usda@</Ref_1>, @./ref_2.usda@]) {}\n");
    layers.add("ref_1.usda", "def \"Ref_1\" (references = @./ref_1_1.usda@</Ref_1_1>) { def \"Child\" {} }\n");
    layers.add("ref_1_1.usda", "def \"Ref_1_1\" { def \"Child\" {}\ndef \"Other\" {} }\n");
    layers.add("ref_2.usda", "(defaultPrim = \"Ref_2\")\ndef \"Ref_2\" {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Root") == "root:/Root root:/LocalRef ref_1:/Ref_1 ref_1_1:/Ref_1_1 ref_2:/Ref_2");
    CHECK(c.opinions("/Root/Child") == "ref_1:/Ref_1/Child ref_1_1:/Ref_1_1/Child");
    CHECK(c.children("/Root") == "Child Other");
    CHECK(prUsdStageOpinion(c.stage, c.prim("/Root").firstOpinion + 2).arc == PrUsdArc_Reference);
    CHECK(prUsdStageOpinion(c.stage, c.prim("/Root").firstOpinion).arc == PrUsdArc_Root);
}

TEST_CASE("a reference that cannot be followed is an error, and the others compose") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (references = [@./missing.usda@</X>, @./ref.usda@</Missing>, @./ref.usda@,"
        " @./ref.usda@</R> (scale = 0)]) {}\n");
    layers.add("ref.usda", "def \"R\" {}\n");
    Composed c(layers);
    CHECK(c.opinions("/A") == "root:/A ref:/R");
    CHECK(c.hasError(PrUsdErrorKind_ArcUnreadable));
    CHECK(c.hasError(PrUsdErrorKind_ArcUnresolved));
    CHECK(c.hasError(PrUsdErrorKind_ArcInvalidPath));
    CHECK(c.hasError(PrUsdErrorKind_InvalidOffset));
    CHECK(prUsdStageErrors(c.stage)[0].prim == "/A");
}

TEST_CASE("a reference's offset retimes the layers that it brings") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (references = @./ref.usda@</R> (offset = 10; scale = 0.5)) {}\n");
    layers.add("ref.usda", "def \"R\" { double x.timeSamples = { 0: 0, 4: 400, 16: 1600 } }\n");
    Composed c(layers);
    // A time of the stage maps to the layer by undoing the offset: (12 - 10) / 0.5 is 4.
    CHECK(c.number("/A", "x", 12.0) == 400.0);
    CHECK(c.number("/A", "x", 18.0) == 1600.0);
    CHECK(c.number("/A", "x", 11.0) == 200.0);
}

TEST_CASE("payloads compose as references do, where they are loaded") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (payload = @./pay.usda@</P>) { def \"Local\" {} }\ndef \"B\" (payload = @./pay.usda@</P>) {}\n");
    layers.add("pay.usda", "def \"P\" { def \"Child\" {} }\n");
    {
        Composed c(layers);
        CHECK(c.opinions("/A") == "root:/A pay:/P");
        CHECK(c.children("/A") == "Child Local");
        CHECK((c.prim("/A").flags & PrUsdPrimFlag_HasPayload) != 0);
        CHECK((c.prim("/A").flags & PrUsdPrimFlag_Loaded) != 0);
        CHECK(prUsdStageOpinion(c.stage, c.prim("/A").firstOpinion + 1).arc == PrUsdArc_Payload);
    }
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.load = PrUsdLoad_None;
    {
        Composed c(layers, options);
        CHECK(c.opinions("/A") == "root:/A");
        CHECK(c.children("/A") == "Local");
        CHECK((c.prim("/A").flags & PrUsdPrimFlag_HasPayload) != 0);
        CHECK((c.prim("/A").flags & PrUsdPrimFlag_Loaded) == 0);
        CHECK((c.prim("/A/Local").flags & PrUsdPrimFlag_Loaded) == 0);
    }
    PrStringView loaded = "/B";
    options.loadPaths = &loaded;
    options.loadPathCount = 1;
    Composed c(layers, options);
    CHECK(c.opinions("/A") == "root:/A");
    CHECK(c.opinions("/B") == "root:/B pay:/P");
    CHECK((c.prim("/B/Child").flags & PrUsdPrimFlag_Loaded) != 0);
}

TEST_CASE("an inherit is implied in every layer stack above the one that authors it") {
    // The specification's example (usd-core/#inherits).
    Layers layers;
    layers.add("root.usda", "def \"Root\" (references = @./ref.usda@</Ref>) {}\n"
        "class \"Inherit_1\" {}\nclass \"Inherit_2\" {}\n");
    layers.add("ref.usda", "class \"Inherit_1\" (inherits = </Inherit_2>) {}\nclass \"Inherit_2\" {}\n"
        "def \"Ref\" (inherits = </Inherit_1>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Root") == "root:/Root root:/Inherit_1 root:/Inherit_2 ref:/Ref ref:/Inherit_1 ref:/Inherit_2");
    CHECK(prUsdStageOpinion(c.stage, c.prim("/Root").firstOpinion + 1).arc == PrUsdArc_Inherit);
}

TEST_CASE("an inherit of a prim below a root prim brings what that prim's ancestors bring it") {
    Layers layers;
    layers.add("root.usda", "def \"Model\" (references = @./model.usda@</Model>) {\n"
        "    over \"Class\" { double x = 1 }\n}\n");
    layers.add("model.usda", "def \"Model\" {\n    class \"Class\" { double x = 2\n double y = 2 }\n"
        "    def \"Instance\" (inherits = </Model/Class>) { double y = 3 }\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/Model/Instance") == "root:/Model/Class model:/Model/Instance model:/Model/Class");
    CHECK(c.number("/Model/Instance", "x", kDefaultTime) == 1.0);
    CHECK(c.number("/Model/Instance", "y", kDefaultTime) == 3.0);
}

TEST_CASE("specializes are weaker than every other opinion of the prim") {
    // The specification's example 6 (usd-core/#example-6-specializes-strength-ordering).
    Layers layers;
    layers.add("root.usda", "def \"Root\" (references = @./ref.usda@</Ref>) {}\n"
        "class \"Class_1\" {}\nclass \"Class_2\" {}\n");
    layers.add("ref.usda", "class \"Class_1\" (specializes = </Class_2>) {}\nclass \"Class_2\" {}\n"
        "def \"Ref\" (specializes = </Class_1>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Root") == "root:/Root ref:/Ref root:/Class_1 ref:/Class_1 root:/Class_2 ref:/Class_2");
    CHECK(prUsdStageOpinion(c.stage, c.prim("/Root").firstOpinion + 2).arc == PrUsdArc_Specialize);
}

TEST_CASE("arcs of different kinds order as LIVERPS has them") {
    // The specification's example 2 (usd-core/#example-2-composition-arc-ordering), with a payload and a specializes.
    Layers layers;
    layers.add("root.usda", "class \"Inherit\" {}\nclass \"Special\" {}\ndef \"Root\" (\n"
        "    specializes = </Special>\n    payload = @./pay.usda@</Pay>\n    references = @./ref.usda@</Ref>\n"
        "    inherits = </Inherit>\n    variants = { string v = \"a\" }\n    prepend variantSets = \"v\"\n) {\n"
        "    variantSet \"v\" = { \"a\" {} }\n}\n");
    layers.add("ref.usda", "class \"Inherit\" {}\ndef \"Ref\" (\n    inherits = </Inherit>\n"
        "    variants = { string v = \"a\" }\n    prepend variantSets = \"v\"\n) {\n    variantSet \"v\" = { \"a\" {} }\n}\n");
    layers.add("pay.usda", "def \"Pay\" {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Root") ==
        "root:/Root root:/Inherit root:/Root{v=a} ref:/Ref ref:/Inherit ref:/Ref{v=a} pay:/Pay root:/Special");
}

TEST_CASE("arcs of one kind order by depth, then authored before implied, then by their list") {
    Layers layers;
    // Example 3: the arc of the prim is stronger than the arc of its parent.
    layers.add("root.usda", "def \"Root\" (references = @./root_ref.usda@</RootRef>) {\n"
        "    def \"Child\" (references = @./child_ref.usda@</ChildRef>) {}\n}\n"
        // Example 4: an authored inherit is stronger than an implied one.
        "class \"RootClass\" {}\nclass \"RefClass\" {}\n"
        "def \"Implied\" (inherits = </RootClass>\n references = @./ref.usda@</Ref>) {}\n"
        // Example 5: siblings are in the order of their list.
        "def \"Ref1\" {}\ndef \"Ref2\" {}\ndef \"Siblings\" (references = [</Ref1>, </Ref2>]) {}\n");
    layers.add("root_ref.usda", "def \"RootRef\" { def \"Child\" {} }\n");
    layers.add("child_ref.usda", "def \"ChildRef\" {}\n");
    layers.add("ref.usda", "class \"RefClass\" {}\ndef \"Ref\" (inherits = </RefClass>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Root/Child") == "root:/Root/Child child_ref:/ChildRef root_ref:/RootRef/Child");
    CHECK(c.opinions("/Implied") == "root:/Implied root:/RootClass root:/RefClass ref:/Ref ref:/RefClass");
    CHECK(c.opinions("/Siblings") == "root:/Siblings root:/Ref1 root:/Ref2");
}

TEST_CASE("a list op of arcs composes across the layers of a stack") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./weak.usda@])\ndef \"A\" {}\ndef \"B\" {}\ndef \"C\" {}\n"
        "over \"P\" (prepend references = </C>\n delete references = </B>) {}\n");
    layers.add("weak.usda", "def \"P\" (references = [</A>, </B>]) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/P") == "root:/P weak:/P root:/C root:/A");
}

TEST_CASE("a variant set composes the variant that the strongest selection names") {
    Layers layers;
    layers.add("root.usda", "def \"Model\" (\n    references = @./model.usda@</Model>\n"
        "    variants = { string look = \"red\" }\n) {}\n"
        "def \"Default\" (references = @./model.usda@</Model>) {}\n"
        "def \"None\" (references = @./model.usda@</Model>\n variants = { string look = \"\" }) {}\n");
    layers.add("model.usda", "def \"Model\" (\n    variants = { string look = \"blue\" }\n"
        "    prepend variantSets = [\"look\", \"size\"]\n) {\n"
        "    variantSet \"look\" = {\n        \"red\" { def \"Red\" {} }\n        \"blue\" { def \"Blue\" {} }\n    }\n"
        "    variantSet \"size\" = { \"big\" { def \"Big\" {} } }\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/Model") == "root:/Model model:/Model model:/Model{look=red}");
    CHECK(c.children("/Model") == "Red");
    CHECK(c.opinions("/Model/Red") == "model:/Model{look=red}Red");
    CHECK(c.opinions("/Default") == "root:/Default model:/Model model:/Model{look=blue}");
    CHECK(c.opinions("/None") == "root:/None model:/Model");
    CHECK(prUsdStagePrimVariantSelection(c.stage, c.find("/Model"), "look") == "red");
    CHECK(prUsdStagePrimVariantSelection(c.stage, c.find("/Model"), "size") == "");
    CHECK(prUsdStagePrimVariantSelection(c.stage, c.find("/None"), "look") == "");
    PrUsdValue sets{};
    REQUIRE(prUsdStagePrimVariantSets(c.stage, c.find("/Model"), c.scratch, &sets));
    CHECK(c.joined(sets) == "look size");
    CHECK(!prUsdStagePrimVariantSets(c.stage, c.find("/Model/Red"), c.scratch, &sets));
    CHECK(prUsdStageOpinion(c.stage, c.prim("/Model").firstOpinion + 2).arc == PrUsdArc_Variant);
}

TEST_CASE("a variant set without a selection takes the stage's fallback, and a variant may select another") {
    Layers layers;
    layers.add("root.usda", "def \"Model\" (prepend variantSets = [\"size\", \"look\"]) {\n"
        "    variantSet \"size\" = {\n        \"big\" (variants = { string look = \"red\" }) {}\n        \"small\" {}\n    }\n"
        "    variantSet \"look\" = { \"red\" { def \"Red\" {} } }\n}\n");
    {
        Composed c(layers);
        CHECK(c.opinions("/Model") == "root:/Model");
    }
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    const PrUsdVariantSelection fallbacks[2] = {{"size", "missing"}, {"size", "big"}};
    options.variantFallbacks = fallbacks;
    options.variantFallbackCount = 2;
    Composed c(layers, options);
    CHECK(c.opinions("/Model") == "root:/Model root:/Model{size=big} root:/Model{look=red}");
    CHECK(prUsdStagePrimVariantSelection(c.stage, c.find("/Model"), "size") == "big");
    CHECK(c.children("/Model") == "Red");
}

TEST_CASE("the session's selection of a prim's variant is stronger than every layer's") {
    Layers layers;
    layers.add("root.usda", "def \"Model\" (\n    references = @./model.usda@</Model>\n"
        "    variants = { string look = \"red\" }\n) {}\n"
        "def \"Other\" (references = @./model.usda@</Model>) {}\n");
    layers.add("model.usda", "def \"Model\" (\n    variants = { string look = \"blue\" }\n"
        "    prepend variantSets = \"look\"\n) {\n"
        "    variantSet \"look\" = {\n        \"red\" { def \"Red\" {} }\n        \"blue\" { def \"Blue\" {} }\n"
        "        \"green\" { def \"Green\" {} }\n    }\n}\n");
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    const PrUsdPrimVariantSelection selections[3] = {
        {"/Model", "look", "green"}, {"/Other", "look", ""}, {"/Missing", "look", "red"},
    };
    options.variantSelections = selections;
    options.variantSelectionCount = 3;
    Composed c(layers, options);
    CHECK(c.opinions("/Model") == "root:/Model model:/Model model:/Model{look=green}");
    CHECK(c.children("/Model") == "Green");
    CHECK(prUsdStagePrimVariantSelection(c.stage, c.find("/Model"), "look") == "green");
    // An empty selection selects no variant, over the referenced layer's.
    CHECK(c.opinions("/Other") == "root:/Other model:/Model");
    CHECK(prUsdStageErrorCount(c.stage) == 0);
}

TEST_CASE("a relocate moves a prim, with the opinions that its source composes") {
    // The specification's example (usd-core/#relocates).
    Layers layers;
    layers.add("root.usda", "(relocates = { </Root/Child>: </Root/Relocated_Child> })\n"
        "def \"Root\" (references = @./ref.usda@</Ref>) {\n    over \"Relocated_Child\" { double x = 1 }\n}\n");
    layers.add("ref.usda", "def \"Ref\" {\n    def \"Child\" { double x = 2\n        def \"Grand\" {}\n    }\n"
        "    def \"Other\" { rel toChild = </Ref/Child>\n        rel toGrand = </Ref/Child/Grand> }\n}\n");
    Composed c(layers);
    CHECK(c.children("/Root") == "Relocated_Child Other");
    CHECK(c.opinions("/Root/Relocated_Child") == "root:/Root/Relocated_Child ref:/Ref/Child");
    CHECK(c.opinions("/Root/Relocated_Child/Grand") == "ref:/Ref/Child/Grand");
    CHECK(c.find("/Root/Child") == kPrUsdNone);
    CHECK(c.number("/Root/Relocated_Child", "x", kDefaultTime) == 1.0);
    // A path of the source layer stack maps to where the relocate put it.
    CHECK(c.targets("/Root/Other", "toChild") == "/Root/Relocated_Child");
    CHECK(c.targets("/Root/Other", "toGrand") == "/Root/Relocated_Child/Grand");
    CHECK(prUsdStageErrorCount(c.stage) == 0);
}

TEST_CASE("a relocate that breaks a rule is an error, and so is a spec at a relocate's source") {
    Layers layers;
    layers.add("root.usda", "(relocates = {\n    </Root>: </Other>,\n    </A/B>: </A/B/C>,\n    </A/D>: </A>,\n"
        "    </A/E>: </A/E>,\n    </A/F>: </A/Twice>,\n    </A/G>: </A/Twice>,\n    </M/Old>: </M/New>\n})\n"
        "def \"Root\" {}\ndef \"A\" { def \"B\" {}\ndef \"F\" {}\ndef \"G\" {} }\n"
        "def \"M\" (references = @./ref.usda@</M>) { over \"Old\" {} }\n");
    layers.add("ref.usda", "def \"M\" { def \"Old\" {} }\n");
    Composed c(layers);
    uint32_t invalid = 0;
    for (uint32_t i = 0; i < prUsdStageErrorCount(c.stage); ++i)
        invalid += prUsdStageErrors(c.stage)[i].kind == PrUsdErrorKind_InvalidRelocate ? 1 : 0;
    CHECK(invalid == 6);
    CHECK(c.children("/A") == "B F G");
    CHECK(c.children("/M") == "New");
    CHECK(c.opinions("/M/New") == "ref:/M/Old");
    CHECK(c.hasError(PrUsdErrorKind_OpinionAtRelocateSource));
}

TEST_CASE("an arc to where its own site comes from is a cycle, and is ignored") {
    Layers layers;
    layers.add("root.usda", "def \"Self\" (references = </Self>) {}\n"
        "def \"Parent\" { def \"Child\" (references = </Parent>) {} }\n"
        "def \"Inner\" (inherits = </Inner/Class>) { class \"Class\" {} }\n"
        "def \"A\" (references = </B>) {}\ndef \"B\" (references = </A>) {}\n"
        "def \"Fine\" (references = </Self>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/Self") == "root:/Self");
    CHECK(c.opinions("/Parent/Child") == "root:/Parent/Child");
    CHECK(c.opinions("/Inner") == "root:/Inner");
    CHECK(c.opinions("/A") == "root:/A root:/B");
    CHECK(c.opinions("/Fine") == "root:/Fine root:/Self");
    CHECK(c.hasError(PrUsdErrorKind_ArcCycle));
    // A long chain of arcs is followed to a bound, and composition ends.
    PrTextBuffer chain = {};
    for (uint32_t i = 0; i < 3000; ++i) {
        prTextBufferAppend(&chain, "def \"P");
        prTextBufferAppendU64(&chain, i, 10);
        prTextBufferAppend(&chain, "\" (references = </P");
        prTextBufferAppendU64(&chain, i + 1, 10);
        prTextBufferAppend(&chain, ">) {}\n");
    }
    Layers deep;
    deep.add("root.usda", prTextBufferView(&chain));
    prTextBufferReset(&chain);
    Composed d(deep);
    CHECK(d.hasError(PrUsdErrorKind_ArcCycle));
    CHECK(d.prim("/P0").opinionCount < 3000);
}

TEST_CASE("children order by their specs, weakest first, with each layer's reorder applied") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./weak.usda@])\ndef \"P\" {\n    reorder nameChildren = [\"C\", \"A\"]\n"
        "    def \"New\" {}\n    over \"B\" {}\n}\n");
    layers.add("weak.usda", "def \"P\" { def \"A\" {}\ndef \"B\" {}\ndef \"C\" {} }\n");
    Composed c(layers);
    // The weak layer gives A B C, the strong one adds New, and its reorder moves C with what follows it before A.
    CHECK(c.children("/P") == "C New A B");
}

TEST_CASE("properties order by name, with the strongest reorder applied") {
    Layers layers;
    layers.add("root.usda", "def \"P\" (references = </Q>) {\n    reorder properties = [\"zeta\", \"alpha\"]\n"
        "    double zeta = 1\n    double B = 1\n    custom rel alpha\n}\n"
        "def \"Q\" {\n    reorder properties = [\"b\", \"a10\"]\n    double a10 = 1\n    double a2 = 1\n    double b = 1\n"
        "    double _x = 1\n}\n");
    Composed c(layers);
    CHECK(c.properties("/Q") == "b a10 _x a2");
    CHECK(c.properties("/P") == "zeta alpha _x a2 a10 B b");
    uint32_t alpha = c.property("/P", "alpha");
    CHECK(prUsdStageProperties(c.stage)[alpha].form == PrUsdSpecForm_Relationship);
    CHECK(prUsdStageFindProperty(c.stage, c.find("/P"), "missing") == kPrUsdNone);
}

TEST_CASE("a prim's specifier, type and activity resolve by their rules") {
    Layers layers;
    layers.add("root.usda", "class \"Class\" { def \"InClass\" {} }\n"
        "def \"Inherits\" (inherits = </Class>) {}\n"
        "over \"Over\" (references = </Typed>) {}\n"
        "over \"OnlyOver\" { def \"Child\" {} }\n"
        "def Mesh \"Typed\" { }\n"
        "def Cube \"Retyped\" (references = </Typed>) {}\n"
        "def \"Off\" (active = false) { def \"Child\" {} }\n"
        "def \"On\" (references = </Off>\n active = true) {}\n");
    Composed c(layers);
    CHECK(c.prim("/Class").specifier == PrUsdSpecifier_Class);
    CHECK((c.prim("/Class").flags & PrUsdPrimFlag_Abstract) != 0);
    CHECK((c.prim("/Class/InClass").flags & PrUsdPrimFlag_Abstract) != 0);
    CHECK(c.prim("/Class/InClass").specifier == PrUsdSpecifier_Def);
    // Inheriting a class does not make a prim one.
    CHECK(c.prim("/Inherits").specifier == PrUsdSpecifier_Def);
    CHECK((c.prim("/Inherits").flags & PrUsdPrimFlag_Abstract) == 0);
    CHECK(c.prim("/Over").specifier == PrUsdSpecifier_Def);
    CHECK(c.prim("/Over").typeName == "Mesh");
    CHECK(c.prim("/OnlyOver").specifier == PrUsdSpecifier_Over);
    CHECK((c.prim("/OnlyOver").flags & PrUsdPrimFlag_Defined) == 0);
    CHECK((c.prim("/OnlyOver/Child").flags & PrUsdPrimFlag_Defined) == 0);
    CHECK(c.prim("/Retyped").typeName == "Cube");
    CHECK((c.prim("/Off").flags & PrUsdPrimFlag_Active) == 0);
    CHECK(c.children("/Off") == "");
    CHECK((c.prim("/On").flags & PrUsdPrimFlag_Active) != 0);
    CHECK(c.children("/On") == "Child");
    PrUsdValue value{};
    REQUIRE(prUsdStagePrimField(c.stage, c.find("/Over"), "specifier", c.scratch, &value));
    CHECK(usdScalar<PrUsdSpecifier>(value) == PrUsdSpecifier_Def);
    REQUIRE(prUsdStagePrimField(c.stage, c.find("/Over"), "typeName", c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "Mesh");
    CHECK(!prUsdStagePrimField(c.stage, c.find("/Inherits"), "typeName", c.scratch, &value));
}

TEST_CASE("instances with the same arcs share one prototype") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (instanceable = true\n references = @./asset.usda@</Asset>) {\n"
        "    over \"Geom\" { double local = 1 }\n    double own = 1\n}\n"
        "def \"B\" (instanceable = true\n references = @./asset.usda@</Asset>) {}\n"
        "def \"C\" (instanceable = true\n references = [@./asset.usda@</Asset>, </Extra>]) {}\n"
        "def \"NoArcs\" (instanceable = true) { def \"Child\" {} }\n"
        "def \"Extra\" { def \"More\" {} }\n");
    layers.add("asset.usda", "def \"Asset\" { def \"Geom\" { double size = 2\n rel self = </Asset/Geom> } }\n");
    {
        Composed c(layers);
        REQUIRE(prUsdStagePrototypeCount(c.stage) == 2);
        const PrUsdPrim& a = c.prim("/A");
        const PrUsdPrim& b = c.prim("/B");
        CHECK((a.flags & PrUsdPrimFlag_Instance) != 0);
        CHECK(a.firstChild == kPrUsdNone);
        CHECK(a.prototype == b.prototype);
        CHECK(a.prototype == prUsdStagePrototype(c.stage, 0));
        CHECK(c.prim("/C").prototype == prUsdStagePrototype(c.stage, 1));
        CHECK(c.properties("/A") == "own");
        // A prim under an instance is in the prototype, with the opinions of the instance's arcs alone.
        CHECK(c.find("/A/Geom") == kPrUsdNone);
        const PrUsdPrim& prototype = prUsdStagePrims(c.stage)[a.prototype];
        CHECK(prototype.name == "__Prototype_1");
        CHECK(prototype.parent == kPrUsdNone);
        CHECK((prototype.flags & PrUsdPrimFlag_InPrototype) != 0);
        CHECK(c.children("/__Prototype_1") == "Geom");
        CHECK(c.opinions("/__Prototype_1/Geom") == "asset:/Asset/Geom");
        CHECK(c.properties("/__Prototype_1/Geom") == "self size");
        CHECK(c.targets("/__Prototype_1/Geom", "self") == "/__Prototype_1/Geom");
        CHECK(c.children("/__Prototype_2") == "More Geom");
        PrTextBuffer path = {};
        prUsdStagePrimPath(c.stage, c.find("/__Prototype_1/Geom"), &path);
        CHECK(prTextBufferView(&path) == "/__Prototype_1/Geom");
        prTextBufferReset(&path);
        // A prim that is instanceable without an arc is no instance.
        CHECK((c.prim("/NoArcs").flags & PrUsdPrimFlag_Instance) == 0);
        CHECK(c.children("/NoArcs") == "Child");
    }
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.expandInstances = true;
    Composed c(layers, options);
    CHECK(prUsdStagePrototypeCount(c.stage) == 0);
    CHECK(c.opinions("/A/Geom") == "root:/A/Geom asset:/Asset/Geom");
}

TEST_CASE("metadata resolves to the strongest opinion, with dictionaries and list ops combined") {
    Layers layers;
    layers.add("root.usda", "(upAxis = \"Z\"\n metersPerUnit = 0.01\n defaultPrim = \"P\"\n startTimeCode = 1\n endTimeCode = 9)\n"
        "def \"P\" (\n    references = </Q>\n    kind = \"group\"\n"
        "    customData = { int a = 1\n dictionary nested = { int x = 1 } }\n"
        "    prepend apiSchemas = [\"FirstAPI\"]\n    delete apiSchemas = [\"GoneAPI\"]\n) {\n"
        "    double plain = 1\n}\n"
        "def \"Q\" (\n    kind = \"component\"\n    documentation = \"weak\"\n"
        "    customData = { int a = 2\n int b = 2\n dictionary nested = { int x = 2\n int y = 2 } }\n"
        "    apiSchemas = [\"GoneAPI\", \"LastAPI\"]\n) {\n    custom double plain = 2 (documentation = \"doc\")\n}\n");
    Composed c(layers);
    PrUsdValue value{};
    REQUIRE(prUsdStagePrimField(c.stage, c.find("/P"), "kind", c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "group");
    REQUIRE(prUsdStagePrimField(c.stage, c.find("/P"), "documentation", c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "weak");
    CHECK(!prUsdStagePrimField(c.stage, c.find("/P"), "comment", c.scratch, &value));

    REQUIRE(prUsdStagePrimField(c.stage, c.find("/P"), "customData", c.scratch, &value));
    const PrUsdDictionary& data = usdScalar<PrUsdDictionary>(value);
    REQUIRE(data.count == 3);
    CHECK(data.entries[0].key == "a");
    CHECK(usdScalar<int32_t>(data.entries[0].value) == 1);
    CHECK(usdScalar<int32_t>(data.entries[1].value) == 2);
    const PrUsdDictionary& nested = usdScalar<PrUsdDictionary>(data.entries[2].value);
    REQUIRE(nested.count == 2);
    CHECK(usdScalar<int32_t>(nested.entries[0].value) == 1);
    CHECK(usdScalar<int32_t>(nested.entries[1].value) == 2);

    REQUIRE(prUsdStagePrimField(c.stage, c.find("/P"), "apiSchemas", c.scratch, &value));
    const PrUsdListOp& schemas = usdScalar<PrUsdListOp>(value);
    CHECK(schemas.isExplicit);
    CHECK(c.joined(schemas.items[PrUsdListOpList_Explicit]) == "FirstAPI LastAPI");

    uint32_t plain = c.property("/P", "plain");
    REQUIRE(prUsdStagePropertyField(c.stage, c.find("/P"), plain, "custom", c.scratch, &value));
    CHECK(usdScalar<bool>(value));
    REQUIRE(prUsdStagePropertyField(c.stage, c.find("/P"), plain, "documentation", c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "doc");
    REQUIRE(prUsdStagePropertyField(c.stage, c.find("/P"), plain, "typeName", c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "double");

    // The stage's own metadata is the root layer's.
    REQUIRE(prUsdStageField(c.stage, "upAxis", &value));
    CHECK(usdScalar<PrStringView>(value) == "Z");
    REQUIRE(prUsdStageField(c.stage, "metersPerUnit", &value));
    CHECK(usdScalar<double>(value) == 0.01);
    REQUIRE(prUsdStageField(c.stage, "defaultPrim", &value));
    CHECK(usdScalar<PrStringView>(value) == "P");
    REQUIRE(prUsdStageField(c.stage, "endTimeCode", &value));
    CHECK(usdScalar<double>(value) == 9.0);
    CHECK(!prUsdStageField(c.stage, "comment", &value));
    prUsdScratchClear(c.scratch);
}

TEST_CASE("an attribute resolves to its strongest default, or at a time to its strongest samples or default") {
    Layers layers;
    layers.add("root.usda", "def \"P\" (references = </Q>) {\n    double strongDefault = 5\n"
        "    double blocked = None\n    double blockedSample.timeSamples = { 0: 1, 10: None, 20: 3 }\n}\n"
        "def \"Q\" {\n    double strongDefault.timeSamples = { 0: 0, 10: 10 }\n"
        "    double both = 7\n    double both.timeSamples = { 0: 0, 10: 10 }\n"
        "    double blocked = 2\n    double none\n"
        "    double blockedSample = 9\n}\n");
    Composed c(layers);
    // A stronger default hides weaker samples.
    CHECK(c.number("/P", "strongDefault", 5.0) == 5.0);
    CHECK(c.number("/Q", "strongDefault", 5.0) == 5.0);
    CHECK(!c.hasValue("/Q", "strongDefault", kDefaultTime));
    // Samples before a default of the same spec at a time, and the default at the default time.
    CHECK(c.number("/P", "both", 4.0) == 4.0);
    CHECK(c.number("/P", "both", kDefaultTime) == 7.0);
    // Samples hold before the first and after the last.
    CHECK(c.number("/P", "both", -5.0) == 0.0);
    CHECK(c.number("/P", "both", 50.0) == 10.0);
    // A block says that there is no value, whatever is weaker.
    CHECK(!c.hasValue("/P", "blocked", kDefaultTime));
    CHECK(!c.hasValue("/P", "blocked", 1.0));
    CHECK(!c.hasValue("/P", "none", 1.0));
    CHECK(c.number("/P", "blockedSample", 5.0) == 1.0);
    CHECK(!c.hasValue("/P", "blockedSample", 10.0));
    CHECK(!c.hasValue("/P", "blockedSample", 15.0));
    CHECK(c.number("/P", "blockedSample", 20.0) == 3.0);
}

TEST_CASE("samples interpolate linearly for the types that do, and hold for the others") {
    Layers layers;
    layers.add("root.usda", "def \"P\" {\n"
        "    float f.timeSamples = { 0: 0, 10: 1 }\n"
        "    int i.timeSamples = { 0: 0, 10: 10 }\n"
        "    token t.timeSamples = { 0: \"a\", 10: \"b\" }\n"
        "    float3 v.timeSamples = { 0: (0, 10, 100), 10: (10, 20, 200) }\n"
        "    double[] a.timeSamples = { 0: [0, 2], 10: [10, 4] }\n"
        "    double[] ragged.timeSamples = { 0: [1], 10: [10, 4] }\n"
        "    quatf q.timeSamples = { 0: (1, 0, 0, 0), 10: (0, 1, 0, 0) }\n"
        "    matrix2d m.timeSamples = { 0: ((0, 0), (0, 0)), 10: ((2, 4), (6, 8)) }\n"
        "    half h.timeSamples = { 0: 0, 10: 1 }\n}\n");
    Composed c(layers);
    PrUsdPrimIndex prim = c.find("/P");
    PrUsdValue value{};
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "f"), 2.5, c.scratch, &value));
    CHECK(usdScalar<float>(value) == 0.25f);
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "i"), 9.0, c.scratch, &value));
    CHECK(usdScalar<int32_t>(value) == 0);
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "t"), 9.0, c.scratch, &value));
    CHECK(usdScalar<PrStringView>(value) == "a");
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "v"), 5.0, c.scratch, &value));
    CHECK(usdScalar<PrVec3>(value).y == 15.0f);
    CHECK(usdScalar<PrVec3>(value).z == 150.0f);
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "a"), 5.0, c.scratch, &value));
    REQUIRE(value.count == 2);
    CHECK(static_cast<const double*>(value.data)[0] == 5.0);
    CHECK(static_cast<const double*>(value.data)[1] == 3.0);
    // Arrays of different lengths do not interpolate.
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "ragged"), 5.0, c.scratch, &value));
    CHECK(value.count == 1);
    // A quaternion turns along the arc: halfway from the identity to a half turn about x is a quarter turn.
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "q"), 5.0, c.scratch, &value));
    CHECK(usdScalar<PrQuat>(value).x == doctest::Approx(0.70710678));
    CHECK(usdScalar<PrQuat>(value).w == doctest::Approx(0.70710678));
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "m"), 5.0, c.scratch, &value));
    CHECK(static_cast<const double*>(value.data)[3] == 4.0);
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/P", "h"), 5.0, c.scratch, &value));
    CHECK(prFloat16ToFloat(usdScalar<PrFloat16>(value)) == 0.5f);

    // A value read into the caller's storage has the type that the caller names, and fits.
    float f = 0;
    uint32_t count = 0;
    CHECK(prUsdStageAttributeRead(c.stage, prim, c.property("/P", "f"), 5.0, PrUsdType_Float, &f, 1, &count));
    CHECK(f == 0.5f);
    CHECK(count == 1);
    double two[2] = {};
    CHECK(prUsdStageAttributeRead(c.stage, prim, c.property("/P", "a"), 10.0, PrUsdType_Double, two, 2, &count));
    CHECK(two[1] == 4.0);
    CHECK(!prUsdStageAttributeRead(c.stage, prim, c.property("/P", "a"), 10.0, PrUsdType_Double, two, 1, &count));
    CHECK(count == 2);
    CHECK(!prUsdStageAttributeRead(c.stage, prim, c.property("/P", "a"), 10.0, PrUsdType_Float, two, 2, &count));
}

TEST_CASE("targets and connections map into the stage's namespace, and combine across opinions") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (references = @./ref.usda@</Ref>) {\n"
        "    prepend rel many = </A/Local>\n    delete rel many = </A/Two>\n    rel replaced = </A/Local>\n"
        "    def \"Local\" {}\n}\n");
    layers.add("ref.usda", "def \"Ref\" {\n    rel many = [</Ref/One>, </Ref/Two>]\n    rel outside = </Elsewhere>\n"
        "    rel replaced = </Elsewhere>\n"
        "    rel forwarded = [</Ref.many>, </Ref/One.size>]\n    rel loop = </Ref.loop>\n    rel none\n"
        "    double in.connect = </Ref/One.size>\n    def \"One\" { double size = 1 }\n    def \"Two\" {}\n}\n"
        "def \"Elsewhere\" {}\n");
    Composed c(layers);
    CHECK(c.targets("/A", "many") == "/A/Local /A/One");
    // A path outside what the reference maps is no path of the stage, and is left out.
    CHECK(c.targets("/A", "outside") == "");
    CHECK(c.targets("/A", "in") == "/A/One.size");
    CHECK(c.targets("/A", "forwarded") == "/A.many /A/One.size");
    CHECK(c.targets("/A", "forwarded", true) == "/A/Local /A/One /A/One.size");
    // A relationship that forwards to itself ends.
    CHECK(c.targets("/A", "loop", true) == "");
    PrUsdValue value{};
    uint32_t invalid = 7;
    CHECK(!prUsdStageTargets(c.stage, c.find("/A"), c.property("/A", "none"), false, c.scratch, &value, &invalid));
    CHECK(invalid == 0);
    CHECK(prUsdStageTargets(c.stage, c.find("/A"), c.property("/A", "outside"), false, c.scratch, &value, &invalid));
    CHECK(invalid == 1);
    CHECK(value.count == 0);
    // An explicit list replaces the weaker opinions' paths, which are then not counted where they do not map.
    CHECK(prUsdStageTargets(c.stage, c.find("/A"), c.property("/A", "replaced"), false, c.scratch, &value, &invalid));
    CHECK(invalid == 0);
    CHECK(c.targets("/A", "replaced") == "/A/Local");
    PrUsdOpinion specs[4];
    REQUIRE(prUsdStagePropertyOpinions(c.stage, c.find("/A"), c.property("/A", "many"), specs, 4) == 2);
    CHECK(prUsdLayerSpecs(prUsdStageLayer(c.stage, specs[1].layer))[specs[1].spec].path == "/Ref.many");
}

TEST_CASE("a spec of the other form than a property's strongest is no opinion of it, and is an error") {
    Layers layers;
    layers.add("root.usda", "def \"P\" (references = </Q>) {\n    double x = 1\n    double y = 1\n}\n"
        "def \"Q\" {\n    rel x = </Q>\n    uniform double y = 2\n}\n");
    Composed c(layers);
    PrUsdOpinion specs[2];
    CHECK(prUsdStagePropertyOpinions(c.stage, c.find("/P"), c.property("/P", "x"), specs, 2) == 1);
    CHECK(prUsdStagePropertyOpinions(c.stage, c.find("/P"), c.property("/P", "y"), specs, 2) == 2);
    CHECK(c.hasError(PrUsdErrorKind_InconsistentProperty));
    CHECK(prUsdStageErrorCount(c.stage) == 1);
    CHECK(c.number("/P", "x", kDefaultTime) == 1.0);
    // The weakest spec of the name gives the variability, and a relationship's is uniform.
    PrUsdValue value{};
    REQUIRE(prUsdStagePropertyField(c.stage, c.find("/P"), c.property("/P", "x"), "variability", c.scratch, &value));
    CHECK(usdScalar<PrUsdVariability>(value) == PrUsdVariability_Uniform);
    REQUIRE(prUsdStagePropertyField(c.stage, c.find("/P"), c.property("/P", "y"), "variability", c.scratch, &value));
    CHECK(usdScalar<PrUsdVariability>(value) == PrUsdVariability_Uniform);
}

TEST_CASE("an attribute says where its value comes from, between which samples, and whether it varies") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (references = @./sub/ref.usda@</R> (offset = 10)) {\n    double still = 1\n}\n");
    layers.add("sub/ref.usda", "def \"R\" {\n    asset file = @./tex.png@\n    double x.timeSamples = { 0: 0, 4: 4 }\n"
        "    double one.timeSamples = { 0: 0 }\n    timecode when = 5\n    timecode whens.timeSamples = { 0: 1, 4: 3 }\n}\n");
    Composed c(layers);
    PrUsdPrimIndex prim = c.find("/A");
    PrRawString location = prUsdStageAttributeAssetResolve(c.stage, prim, c.property("/A", "file"), kDefaultTime, "./tex.png");
    CHECK(prRawStringView(&location) == "/sub/tex.png");
    prRawStringReset(&location);
    double lower = 0;
    double upper = 0;
    REQUIRE(prUsdStageAttributeBracket(c.stage, prim, c.property("/A", "x"), 12.0, &lower, &upper));
    CHECK(lower == 10.0);
    CHECK(upper == 14.0);
    REQUIRE(prUsdStageAttributeBracket(c.stage, prim, c.property("/A", "x"), 14.0, &lower, &upper));
    CHECK(lower == 14.0);
    CHECK(upper == 14.0);
    REQUIRE(prUsdStageAttributeBracket(c.stage, prim, c.property("/A", "x"), 0.0, &lower, &upper));
    CHECK(lower == 10.0);
    CHECK(upper == 10.0);
    CHECK(!prUsdStageAttributeBracket(c.stage, prim, c.property("/A", "still"), 1.0, &lower, &upper));
    CHECK(prUsdStageAttributeVaries(c.stage, prim, c.property("/A", "x")));
    CHECK(!prUsdStageAttributeVaries(c.stage, prim, c.property("/A", "one")));
    CHECK(!prUsdStageAttributeVaries(c.stage, prim, c.property("/A", "still")));
    // A time code is retimed with the layer that holds it.
    PrUsdValue value{};
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/A", "when"), kDefaultTime, c.scratch, &value));
    CHECK(usdScalar<double>(value) == 15.0);
    REQUIRE(prUsdStageAttributeValue(c.stage, prim, c.property("/A", "whens"), 12.0, c.scratch, &value));
    CHECK(usdScalar<double>(value) == 12.0);
}

TEST_CASE("a path through an instance finds the prim of its prototype") {
    Layers layers;
    layers.add("root.usda", "def \"A\" (instanceable = true\n references = </Asset>) {}\n"
        "def \"B\" (instanceable = true\n references = </Asset>) {}\n"
        "def \"Asset\" {\n    def \"Geom\" {\n        def \"Mesh\" {}\n    }\n}\n");
    Composed c(layers);
    PrUsdPrimIndex mesh = prUsdStageFindProxy(c.stage, "/A/Geom/Mesh");
    REQUIRE(mesh != kPrUsdNone);
    CHECK(mesh == c.find("/__Prototype_1/Geom/Mesh"));
    CHECK(prUsdStageFindProxy(c.stage, "/B/Geom/Mesh") == mesh);
    CHECK(prUsdStageFindProxy(c.stage, "/A") == c.find("/A"));
    CHECK(prUsdStageFindProxy(c.stage, "/__Prototype_1/Geom") == c.find("/__Prototype_1/Geom"));
    CHECK(prUsdStageFindProxy(c.stage, "/Asset/Geom") == c.find("/Asset/Geom"));
    CHECK(prUsdStageFindProxy(c.stage, "/A/Missing") == kPrUsdNone);
}

TEST_CASE("a scene path names a prim, a property, a variant set or a variant of the stage") {
    Layers layers;
    layers.add("root.usda", "def \"World\" {\n    def \"Car\" (\n        variants = {\n            string color = \"red\"\n"
        "        }\n        prepend variantSets = \"color\"\n    ) {\n        float size = 1\n"
        "        variantSet \"color\" = {\n            \"red\" {\n                def \"Paint\" {}\n            }\n"
        "            \"blue\" {\n            }\n        }\n    }\n}\n"
        "def \"A\" (instanceable = true\n references = </Asset>) {}\ndef \"Asset\" {\n    def \"Geom\" {}\n}\n");
    Composed c(layers);
    PrUsdPrimIndex car = c.find("/World/Car");
    struct Found {
        PrStringView path;
        PrUsdPrimIndex prim;
    };
    const Found found[] = {
        {"/World/Car", car}, {"/World/Car.size", car}, {"/World/Car{color=red}", car}, {"/World/Car{color=blue}", car},
        {"/World/Car{color=}", car}, {"/World/Car{color=red}Paint", c.find("/World/Car/Paint")},
        {"/World/Car/Paint", c.find("/World/Car/Paint")}, {"/", kPrUsdPseudoRoot},
        {"/A/Geom", prUsdStageFindProxy(c.stage, "/A/Geom")},
    };
    for (const Found& each : found) {
        CAPTURE(each.path);
        PrUsdPrimIndex prim = kPrUsdNone;
        CHECK(prUsdStageFindPath(c.stage, each.path, &prim) == PrStatus_Ok);
        CHECK(prim == each.prim);
    }
    const PrStringView missing[] = {"/World/Truck", "/World/Car.normals", "/World/Car{color=green}", "/World/Car{size=}"};
    for (PrStringView path : missing) {
        CAPTURE(path);
        PrUsdPrimIndex prim = 0;
        CHECK(prUsdStageFindPath(c.stage, path, &prim) == PrStatus_NotFound);
        CHECK(prim == kPrUsdNone);
    }
    const PrStringView malformed[] = {
        "/World/Car{color", "/World//Car", "/World/9Car", "/World/Car/", "//World", "/World/{color=red}",
        "/World/Car.", "/World/Car.9points", "/World/Car{color=r=d}", "/World/Ca-r", "World/Car",
    };
    for (PrStringView path : malformed) {
        CAPTURE(path);
        PrUsdPrimIndex prim = kPrUsdNone;
        CHECK(prUsdStageFindPath(c.stage, path, &prim) == PrStatus_Syntax);
    }
}

TEST_CASE("a site's variant sets combine across the layers of its stack") {
    Layers layers;
    layers.add("root.usda", "(subLayers = [@./weak.usda@])\nover \"P\" (delete variantSets = \"gone\"\n"
        "    prepend variantSets = \"first\") {}\n");
    layers.add("weak.usda", "def \"P\" (variantSets = [\"gone\", \"kept\"]) {}\n");
    Composed c(layers);
    PrUsdValue sets{};
    REQUIRE(prUsdStagePrimVariantSets(c.stage, c.find("/P"), c.scratch, &sets));
    CHECK(c.joined(sets) == "first kept");
}

TEST_CASE("the variant of an arc's ancestor takes a selection from a class that the layer stacks above imply") {
    // The inherit of </Rig/Left/Anim> composes </Rig/Left>, which inherits a class that references a prim with a
    // variant set. The root layer selects the variant on its own prim of the class, which the prim has only once the
    // arc has joined its graph and the class is implied there.
    Layers layers;
    layers.add("root.usda", "def \"Char\" (references = @./rig.usda@</Rig>) {\n"
        "    over \"Sym\" (variants = { string style = \"one\" }) {}\n}\n");
    layers.add("rig.usda", "def \"Rig\" {\n    class \"Sym\" (references = @./leg.usda@</Leg>) {}\n"
        "    def \"Left\" (inherits = </Rig/Sym>) {}\n    def \"User\" (inherits = </Rig/Left/Anim>) {}\n}\n");
    layers.add("leg.usda", "def \"Leg\" (variantSets = \"style\") {\n    variantSet \"style\" = {\n"
        "        \"one\" { def \"Anim\" { double one = 1 } }\n        \"two\" { def \"Anim\" { double two = 2 } }\n"
        "    }\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/Char/User") == "rig:/Rig/User leg:/Leg{style=one}Anim");
    CHECK(c.properties("/Char/User") == "one");
    CHECK(c.opinions("/Char/Left") == "root:/Char/Sym rig:/Rig/Left rig:/Rig/Sym leg:/Leg leg:/Leg{style=one}");
}

TEST_CASE("the variants of an arc's ancestors compose under the arc, a variant in a variant too") {
    Layers layers;
    layers.add("root.usda", "def \"Lib\" (variantSets = [\"a\", \"b\"]\n    variants = { string a = \"x\" }) {\n"
        "    variantSet \"a\" = {\n        \"x\" (variantSets = \"in\"\n"
        "            variants = {\n string b = \"y\"\n string in = \"z\" }) {\n            over \"Thing\" { double a = 1 }\n"
        "            variantSet \"in\" = { \"z\" { over \"Thing\" { double in = 1 } } }\n        }\n    }\n"
        "    variantSet \"b\" = { \"y\" { over \"Thing\" { double b = 1 } } }\n"
        "    def \"Thing\" { double thing = 1 }\n}\n"
        "def \"User\" (references = </Lib/Thing>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/User") ==
        "root:/User root:/Lib/Thing root:/Lib{a=x}Thing root:/Lib{a=x}{in=z}Thing root:/Lib{b=y}Thing");
}

TEST_CASE("an arc into an instance takes the variants of the ancestors above the instance, and not its own") {
    // The instance's own node gives a prim under it nothing, and neither do the variants that the node lists.
    Layers layers;
    layers.add("root.usda", "def \"W\" (variantSets = \"w\"\n    variants = { string w = \"p\" }) {\n"
        "    variantSet \"w\" = { \"p\" { over \"I\" { over \"Child\" { double w = 1 } } } }\n"
        "    def \"I\" (instanceable = true\n        variantSets = \"m\"\n        variants = { string m = \"a\" }\n"
        "        references = </Asset>) {\n"
        "        variantSet \"m\" = { \"a\" { over \"Child\" { double m = 1 } } }\n    }\n}\n"
        "def \"Asset\" { def \"Child\" { double asset = 1 } }\n"
        "def \"User\" (references = </W/I/Child>) {}\n");
    Composed c(layers);
    CHECK(c.opinions("/User") == "root:/User root:/W{w=p}I/Child root:/Asset/Child");
}

TEST_CASE("a prim's properties are found by name and by the prefix of a namespace, in the order of bytes") {
    // The order of the prim's properties is the reorder's and then OpenUSD's order of names, which is not the order of
    // bytes: `Primvars:z` comes among the names that begin with `primvars:`.
    Layers layers;
    layers.add("root.usda", "def \"P\" (references = </Q>) {\n    reorder properties = [\"xformOp:translate\"]\n"
        "    double primvars:st = 1\n    double primvars:Z = 1\n    double Primvars:z = 1\n"
        "    double xformOp:translate = 1\n    double inputs:a = 1\n}\n"
        "def \"Q\" {\n    double primvars:displayColor = 1\n    double primvar = 1\n    double primvars = 1\n"
        "    double primvars:st:indices = 1\n    rel xformOp:z\n}\ndef \"Empty\" {}\n");
    Composed c(layers);
    PrUsdPrimIndex prim = c.find("/P");
    auto listed = [&](PrUsdPrimIndex of, PrStringView prefix) {
        const uint32_t* indices = nullptr;
        uint32_t count = prUsdStagePropertiesWithPrefix(c.stage, of, prefix, &indices);
        CHECK((indices == nullptr) == (count == 0));
        String text;
        for (uint32_t i = 0; i < count; ++i) {
            if (i != 0)
                text.append(" ");
            text.append(prUsdStageProperties(c.stage)[indices[i]].name);
        }
        return text;
    };
    CHECK(prUsdStageProperties(c.stage)[c.prim("/P").firstProperty].name == "xformOp:translate");
    CHECK(listed(prim, "primvars:") == "primvars:Z primvars:displayColor primvars:st primvars:st:indices");
    CHECK(listed(prim, "primvars:st") == "primvars:st primvars:st:indices");
    CHECK(listed(prim, "xformOp:") == "xformOp:translate xformOp:z");
    CHECK(listed(prim, "inputs:") == "inputs:a");
    CHECK(listed(prim, "outputs:") == "");
    CHECK(listed(prim, "zz") == "");
    CHECK(listed(prim, "") == "Primvars:z inputs:a primvar primvars primvars:Z primvars:displayColor primvars:st "
        "primvars:st:indices xformOp:translate xformOp:z");
    CHECK(listed(c.find("/Empty"), "primvars:") == "");
    for (uint32_t i = 0; i < c.prim("/P").propertyCount; ++i) {
        uint32_t property = c.prim("/P").firstProperty + i;
        CHECK(prUsdStageFindProperty(c.stage, prim, prUsdStageProperties(c.stage)[property].name) == property);
    }
    CHECK(prUsdStageFindProperty(c.stage, prim, "primvars:") == kPrUsdNone);
    CHECK(prUsdStageFindProperty(c.stage, prim, "primvars:s") == kPrUsdNone);
    CHECK(prUsdStageFindProperty(c.stage, c.find("/Empty"), "a") == kPrUsdNone);
}

TEST_CASE("a prim with twenty thousand arcs of a kind composes in time that grows with them, not with their square") {
    // The arcs of one site are met strongest first, so each takes its place after the last without a walk of the
    // others, and the classes that another layer stack implies are found by their keys.
    const uint32_t count = 20000;
    PrTextBuffer root = {};
    PrTextBuffer asset = {};
    for (uint32_t i = 0; i < count; ++i) {
        prTextBufferAppend(&root, "def \"T");
        prTextBufferAppendU64(&root, i, 10);
        prTextBufferAppend(&root, "\" {}\n");
        prTextBufferAppend(&asset, "class \"C");
        prTextBufferAppendU64(&asset, i, 10);
        prTextBufferAppend(&asset, "\" {}\n");
    }
    auto list = [&](PrTextBuffer* out, PrStringView prim, PrStringView field, PrStringView target) {
        prTextBufferAppend(out, "def \"");
        prTextBufferAppend(out, prim);
        prTextBufferAppend(out, "\" (");
        prTextBufferAppend(out, field);
        prTextBufferAppend(out, " = [");
        for (uint32_t i = 0; i < count; ++i) {
            prTextBufferAppend(out, i == 0 ? "</" : ", </");
            prTextBufferAppend(out, target);
            prTextBufferAppendU64(out, i, 10);
            prTextBufferAppend(out, ">");
        }
        prTextBufferAppend(out, "]) {}\n");
    };
    list(&root, "Refs", "references", "T");
    list(&asset, "Inherits", "inherits", "C");
    list(&asset, "Specializes", "specializes", "C");
    prTextBufferAppend(&root, "def \"A\" (references = @./asset.usda@</Inherits>) {}\n"
        "def \"B\" (references = @./asset.usda@</Specializes>) {}\n");
    Layers layers;
    layers.add("root.usda", prTextBufferView(&root));
    layers.add("asset.usda", prTextBufferView(&asset));
    prTextBufferReset(&root);
    prTextBufferReset(&asset);
    PrTimePerf start = prTimePerfNow();
    Composed c(layers);
    double milliseconds = prTimePerfMilliseconds(start, prTimePerfNow());
    // The bound is hundreds of times what the composition takes, and a small part of what a walk for each arc would.
    CHECK(milliseconds < 20000.0);
    auto spec = [&](PrStringView path, uint32_t index) {
        PrUsdOpinion opinion = prUsdStageOpinion(c.stage, c.prim(path).firstOpinion + index);
        return prUsdLayerSpecs(prUsdStageLayer(c.stage, opinion.layer))[opinion.spec].path;
    };
    REQUIRE(c.prim("/Refs").opinionCount == count + 1);
    CHECK(spec("/Refs", 1) == "/T0");
    CHECK(spec("/Refs", 2) == "/T1");
    CHECK(spec("/Refs", count) == "/T19999");
    const PrStringView classed[] = {"/A", "/B"};
    for (PrStringView path : classed) {
        REQUIRE(c.prim(path).opinionCount == count + 2);
        CHECK(spec(path, 2) == "/C0");
        CHECK(spec(path, count + 1) == "/C19999");
    }
}

TEST_CASE("a target maps by each arc in turn, and is left out where one arc does not map it back") {
    // The class connects to the prims that a relocate makes of each of its two instances. In an instance, the path of
    // that instance's own prim is the image of the class's prim, so the inherit does not map it, and the reference
    // above the inherit, which alone would map it, does not bring it back.
    Layers layers;
    layers.add("root.usda", "def \"Face\" (references = @./rig.usda@</Rig>) {}\n");
    layers.add("rig.usda", "(relocates = {\n    </Rig/L/Anim/Brow>: </Rig/Anim/L>,\n    </Rig/R/Anim/Brow>: </Rig/Anim/R>\n})\n"
        "def \"Rig\" {\n    class \"Sym\" {\n        def \"Sculpt\" {\n            double amount = 1\n"
        "            add double amount.connect = [</Rig/Anim/R.in>, </Rig/Anim/L.in>]\n        }\n"
        "        def \"Anim\" { def \"Brow\" {} }\n    }\n"
        "    def \"L\" (inherits = </Rig/Sym>) {}\n    def \"R\" (inherits = </Rig/Sym>) {}\n    def \"Anim\" {}\n}\n");
    Composed c(layers);
    CHECK(c.targets("/Face/Sym/Sculpt", "amount") == "/Face/Anim/R.in /Face/Anim/L.in");
    CHECK(c.targets("/Face/L/Sculpt", "amount") == "/Face/Anim/R.in");
    CHECK(c.targets("/Face/R/Sculpt", "amount") == "/Face/Anim/L.in");
}

TEST_CASE("a relocate to a site that another relocate replaced is not followed") {
    // </Char> references </Base>, whose </Base/Anim/C> is a relocate's target. </Char/Anim/C> is the target of another
    // relocate, which replaces what the reference brought, so the first relocate brings it nothing.
    Layers layers;
    layers.add("root.usda", "(relocates = {\n    </Base/RigB/C>: </Base/Anim/C>,\n    </Char/RigA/C>: </Char/Anim/C>\n})\n"
        "def \"Base\" (references = @./base.usda@</Rigs>) {\n    over \"Anim\" { over \"C\" {} }\n}\n"
        "def \"Char\" (references = </Base>) {\n    over \"Anim\" { over \"C\" {} }\n}\n");
    layers.add("base.usda", "def \"Rigs\" {\n    def \"RigA\" { def \"C\" {} }\n    def \"RigB\" { def \"C\" {} }\n"
        "    over \"Anim\" {}\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/Base/Anim/C") == "root:/Base/Anim/C base:/Rigs/RigB/C");
    CHECK(c.opinions("/Char/Anim/C") == "root:/Char/Anim/C base:/Rigs/RigA/C");
}

TEST_CASE("a class that two arcs imply at one site is implied from the stronger arc") {
    // The reference and the payload of </A> each bring a prim that specializes its sibling, and each implies the
    // sibling of </A>'s own prim. The class is the stronger arc's, so it comes before that arc's own.
    Layers layers;
    PrStringView base = "def \"Base\" {\n    def \"Looks\" {\n"
        "        def \"Satin\" (specializes = </Base/Looks/Weave>) {}\n        def \"Weave\" {}\n    }\n}\n";
    String root("def \"A\" (references = </Base>\n    payload = @./pay.usda@</A>) {\n"
        "    over \"Looks\" {\n        over \"Satin\" {}\n        over \"Weave\" {}\n    }\n}\n");
    root.append(base);
    String pay("def \"A\" (references = </Base>) {}\n");
    pay.append(base);
    layers.add("root.usda", root);
    layers.add("pay.usda", pay);
    Composed c(layers);
    CHECK(c.opinions("/A/Looks/Satin") == "root:/A/Looks/Satin root:/Base/Looks/Satin pay:/Base/Looks/Satin "
        "root:/A/Looks/Weave root:/Base/Looks/Weave pay:/Base/Looks/Weave");
}

TEST_CASE("the classes under the copy of a specializes arc are implied where the arc was authored") {
    // </H> specializes </S>, </S/Hair> specializes </S/Base>, and </S/Base/Inner> specializes </S/Base/Class>. That
    // last arc is authored under the copy at the root of the arc to </S/Base>, and the node that holds the copy's
    // placeholder has the class too, at </S/Hair/Class>, and through it the root has it at </H/Hair/Class>.
    Layers layers;
    layers.add("root.usda", "def \"S\" {\n    def \"Hair\" (specializes = </S/Base>) {\n"
        "        over \"Class\" {}\n        over \"Inner\" {}\n    }\n"
        "    def \"Base\" {\n        def \"Class\" {}\n        def \"Inner\" (specializes = </S/Base/Class>) {}\n    }\n}\n"
        "def \"H\" (specializes = </S>) {\n    over \"Hair\" {\n        over \"Class\" {}\n        over \"Inner\" {}\n"
        "    }\n    over \"Base\" {\n        over \"Class\" {}\n        over \"Inner\" {}\n    }\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/H/Hair/Inner") == "root:/H/Hair/Inner root:/S/Hair/Inner root:/H/Base/Inner root:/S/Base/Inner "
        "root:/H/Hair/Class root:/S/Hair/Class root:/H/Base/Class root:/S/Base/Class");
}

TEST_CASE("the classes under a specializes arc that the prim's ancestor authors are implied in the root") {
    // A variant of a specialized prim authors a specializes arc, and the prim that specializes its parent has the class
    // too, at the path that the parent's arc maps it to.
    Layers layers;
    layers.add("root.usda", "class \"Class\" {\n    class \"Base\" { double x = 1 }\n"
        "    def \"Prim\" (variants = { string v = \"a\" }\n        prepend variantSets = \"v\") {\n"
        "        variantSet \"v\" = { \"a\" (prepend specializes = </Class/Base>) { } }\n    }\n}\n"
        "def \"A\" (prepend specializes = </Class>) {\n    over \"Base\" { double x = 2 }\n}\n");
    Composed c(layers);
    CHECK(c.opinions("/A/Prim") == "root:/Class/Prim root:/Class/Prim{v=a} root:/A/Base root:/Class/Base");
    CHECK(c.number("/A/Prim", "x", kDefaultTime) == 2.0);
}
