#include "pr/float16.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

#include "usd/tests/usd_fixture.h"

namespace pr_impl {

namespace {

// The status of reading the text layer that `body` makes after the header.
PrStatus statusOf(PrStringView body) {
    return usdReadText(body).status;
}

// The layer with one prim, /A, whose metadata is `metadata`.
UsdRead primWith(PrStringView metadata) {
    String text("def \"A\" (\n");
    text.append(metadata);
    text.append("\n)\n{\n}\n");
    return usdReadText(text);
}

// The layer with one prim, /A, that holds `contents`.
UsdRead primOf(PrStringView contents) {
    String text("def \"A\"\n{\n");
    text.append(contents);
    text.append("\n}\n");
    return usdReadText(text);
}

// The default value of the attribute /A.x, declared as `declaration`.
PrUsdValue defaultOf(const UsdRead& read) {
    REQUIRE(read.status == PrStatus_Ok);
    return read.value("/A.x", "default");
}

PrStringView stringOf(const PrUsdValue& value) {
    return usdScalar<PrStringView>(value);
}

const PrStringView* itemsOf(const PrUsdListOp& listOp, PrUsdListOpList list, uint32_t count) {
    REQUIRE(listOp.items[list].count == count);
    return static_cast<const PrStringView*>(listOp.items[list].data);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a text layer begins with a header that states a version the module reads") {
    UsdRead read = usdReadText("");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prUsdLayerFormat(read.layer) == PrUsdFormat_Text);
    CHECK(prUsdLayerVersion(read.layer) == "1.0");
    CHECK(prUsdLayerSpecCount(read.layer) == 1);
    CHECK(read.spec("/").form == PrUsdSpecForm_Layer);
    CHECK(read.spec("/").parent == kPrUsdNone);

    auto header = [](PrStringView text) {
        return usdReadBytes(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size}).status;
    };
    CHECK(header("#usda 1.0") == PrStatus_Ok);
    CHECK(header("#usda 1.1\n") == PrStatus_Ok);
    CHECK(header("#usda 1.0.32\n") == PrStatus_Ok);
    CHECK(header("#usda 2.0\n") == PrStatus_NotSupported);
    CHECK(header("#usda 1.4\n") == PrStatus_NotSupported);
    CHECK(header("#usda\n") == PrStatus_Syntax);
    CHECK(header("def \"A\" {}\n") == PrStatus_Syntax);
    CHECK(header("\xef\xbb\xbf#usda 1.0\n") == PrStatus_Syntax);
    CHECK(header("#usda 1.0 def \"A\" {}\n") == PrStatus_Syntax);
    CHECK(header("#usda 1.0\n\xff") == PrStatus_Syntax);
}

TEST_CASE("a line ends with a line feed, a carriage return, or both") {
    const PrStringView endCases[] = {"\n", "\r", "\r\n"};
    for (PrStringView end : endCases) {
        String text("#usda 1.0");
        text.append(end);
        text.append("def \"A\" {");
        text.append(end);
        text.append("int x = 1");
        text.append(end);
        text.append("int y = 2");
        text.append(end);
        text.append("}");
        text.append(end);
        UsdRead read = usdReadBytes(PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.count()});
        REQUIRE(read.status == PrStatus_Ok);
        CHECK(read.spec("/A").properties.count == 2);
    }
}

TEST_CASE("comments are padding, and a statement ends with its line or a semicolon") {
    UsdRead read = usdReadText("// a comment to the end of its line\n"
        "/* a comment\n   over lines */ def \"A\" ( /* inside */ active = true; kind = \"x\"; ) # trailing\n"
        "{\n"
        "    int x = 1; int y = 2 // after\n"
        "    int z = 3;\n"
        "}\n");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(read.spec("/A").properties.count == 3);
    CHECK(usdScalar<bool>(read.value("/A", "active")));

    CHECK(statusOf("def \"A\" { int x = 1; }\n") == PrStatus_Ok);
    CHECK(statusOf("def \"A\" {}") == PrStatus_Ok);
    // A property is followed by a separator, metadata is not divided by commas, and a prim ends its line.
    CHECK(statusOf("def \"A\" { int x = 1 }\n") == PrStatus_Syntax);
    CHECK(statusOf("def \"A\" (active = true, kind = \"x\") {}\n") == PrStatus_Syntax);
    CHECK(statusOf("def \"A\" (active = true kind = \"x\") {}\n") == PrStatus_Syntax);
    CHECK(statusOf("def \"A\" {} def \"B\" {}\n") == PrStatus_Syntax);
    CHECK(statusOf("/* never closed") == PrStatus_Syntax);
}

TEST_CASE("a layer's metadata reads into the layer spec's fields") {
    UsdRead read = usdReadText("(\n"
        "    \"a comment\"\n"
        "    doc = \"\"\"two\nlines\"\"\"\n"
        "    defaultPrim = \"World\"\n"
        "    upAxis = \"Y\"\n"
        "    metersPerUnit = 1\n"
        "    startTimeCode = -2.5\n"
        "    framesPerSecond = 30\n"
        "    subLayers = [\n"
        "        @./a.usda@ (offset = 5; scale = 2),\n"
        "        @b.usda@,\n"
        "        @c.usda@ (scale = 3\n scale = 4)\n"
        "    ]\n"
        "    relocates = { </A/B>: </A/C>, <D>: <E/F>, </G>: <> }\n"
        "    unknown = (1, 2)\n"
        "    unknownText = \"quoted\"\n"
        "    unknownDictionary = { int a = 1 }\n"
        ")\n"
        "reorder rootPrims = [\"B\", \"A\"]\n"
        "def \"A\" {}\n");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(stringOf(read.value("/", "comment")) == "a comment");
    CHECK(stringOf(read.value("/", "documentation")) == "two\nlines");
    CHECK(read.value("/", "defaultPrim").type == PrUsdType_Token);
    CHECK(stringOf(read.value("/", "upAxis")) == "Y");
    CHECK(read.value("/", "metersPerUnit").type == PrUsdType_Double);
    CHECK(usdScalar<double>(read.value("/", "metersPerUnit")) == 1.0);
    CHECK(usdScalar<double>(read.value("/", "startTimeCode")) == -2.5);

    REQUIRE(prUsdLayerSublayerCount(read.layer) == 3);
    const PrUsdSublayer* sublayers = prUsdLayerSublayers(read.layer);
    CHECK(sublayers[0].assetPath == "./a.usda");
    CHECK(sublayers[0].layerOffset.offset == 5);
    CHECK(sublayers[0].layerOffset.scale == 2);
    CHECK(sublayers[1].assetPath == "b.usda");
    CHECK(sublayers[1].layerOffset.offset == 0);
    CHECK(sublayers[1].layerOffset.scale == 1);
    CHECK(sublayers[2].layerOffset.scale == 4);
    CHECK(read.value("/", "subLayers").type == PrUsdType_Asset);

    const PrUsdRelocate* relocates = usdElements<PrUsdRelocate>(read.value("/", "layerRelocates"), 3);
    CHECK(relocates[0].source == "/A/B");
    CHECK(relocates[0].target == "/A/C");
    CHECK(relocates[1].source == "/D");
    CHECK(relocates[1].target == "/E/F");
    CHECK(relocates[2].target == "");

    // A field that the module knows no type for keeps its text, and a dictionary is a dictionary.
    CHECK(read.value("/", "unknown").type == PrUsdType_Unregistered);
    CHECK(stringOf(read.value("/", "unknown")) == "(1, 2)");
    CHECK(stringOf(read.value("/", "unknownText")) == "\"quoted\"");
    CHECK(read.value("/", "unknownDictionary").type == PrUsdType_Dictionary);

    const PrStringView* order = usdElements<PrStringView>(read.value("/", "primOrder"), 2);
    CHECK(order[0] == "B");
    CHECK(order[1] == "A");

    CHECK(statusOf("(\n framesPerSecond = 0\n)\n") == PrStatus_InvalidData);
    CHECK(statusOf("(\n upAxis = 3\n)\n") == PrStatus_InvalidData);
    CHECK(statusOf("(\n metersPerUnit = \"1\"\n)\n") == PrStatus_InvalidData);
    CHECK(statusOf("(\n relocates = { </A{v=x}B>: </C> }\n)\n") == PrStatus_InvalidData);
    CHECK(statusOf("(\n subLayers = [@a@ (customData = {})]\n)\n") == PrStatus_Syntax);
    CHECK(statusOf("(\n primChildren = [\"A\"]\n)\n") == PrStatus_InvalidData);
    CHECK(statusOf("(\n doc = \"never closed\n)\n") == PrStatus_Syntax);
}

TEST_CASE("prims read into specs under their parents, in the order of the file") {
    UsdRead read = usdReadText("def Xform \"World\"\n"
        "{\n"
        "    over \"B\" {\n"
        "    }\n"
        "    int size = 1\n"
        "    class Scope.Legacy \"A\"\n"
        "    {\n"
        "        def \"Inner\" {}\n"
        "    }\n"
        "    rel target\n"
        "    reorder nameChildren = [\"A\", \"B\"]\n"
        "    reorder properties = \"target\"\n"
        "}\n"
        "def \"M\xc3\xbcnchen\" {}\n");
    REQUIRE(read.status == PrStatus_Ok);
    const PrUsdSpec& root = read.spec("/");
    REQUIRE(root.prims.count == 2);
    const PrUsdSpec* specs = prUsdLayerSpecs(read.layer);
    const PrUsdSpecIndex* roots = prUsdLayerList(read.layer, root.prims);
    CHECK(specs[roots[0]].path == "/World");
    CHECK(specs[roots[1]].name == "M\xc3\xbcnchen");

    const PrUsdSpec& world = read.spec("/World");
    CHECK(world.form == PrUsdSpecForm_Prim);
    CHECK(world.parent == kPrUsdLayerSpec);
    CHECK(usdScalar<PrUsdSpecifier>(read.value("/World", "specifier")) == PrUsdSpecifier_Def);
    CHECK(stringOf(read.value("/World", "typeName")) == "Xform");
    REQUIRE(world.prims.count == 2);
    REQUIRE(world.properties.count == 2);
    const PrUsdSpecIndex* children = prUsdLayerList(read.layer, world.prims);
    CHECK(specs[children[0]].name == "B");
    CHECK(specs[children[1]].name == "A");
    CHECK(specs[children[0]].parent == prUsdLayerFindSpec(read.layer, "/World"));
    const PrUsdSpecIndex* properties = prUsdLayerList(read.layer, world.properties);
    CHECK(specs[properties[0]].form == PrUsdSpecForm_Attribute);
    CHECK(specs[properties[1]].form == PrUsdSpecForm_Relationship);

    CHECK(usdScalar<PrUsdSpecifier>(read.value("/World/B", "specifier")) == PrUsdSpecifier_Over);
    CHECK(!read.has("/World/B", "typeName"));
    CHECK(usdScalar<PrUsdSpecifier>(read.value("/World/A", "specifier")) == PrUsdSpecifier_Class);
    CHECK(stringOf(read.value("/World/A", "typeName")) == "Scope.Legacy");
    CHECK(read.spec("/World/A/Inner").path == "/World/A/Inner");

    // The orders are fields, and the lists of children are the order of the file.
    CHECK(usdElements<PrStringView>(read.value("/World", "primOrder"), 2)[0] == "A");
    CHECK(usdElements<PrStringView>(read.value("/World", "propertyOrder"), 1)[0] == "target");
    CHECK(!read.has("/World", "primChildren"));

    PrUsdSpecIndex worldIndex = prUsdLayerFindSpec(read.layer, "/World");
    CHECK(prUsdLayerFindChild(read.layer, worldIndex, PrUsdSpecForm_Prim, "A") ==
        prUsdLayerFindSpec(read.layer, "/World/A"));
    CHECK(prUsdLayerFindChild(read.layer, worldIndex, PrUsdSpecForm_Attribute, "target") ==
        prUsdLayerFindSpec(read.layer, "/World.target"));
    CHECK(prUsdLayerFindChild(read.layer, worldIndex, PrUsdSpecForm_Prim, "size") == kPrUsdNone);
    CHECK(prUsdLayerFindSpec(read.layer, "/Nowhere") == kPrUsdNone);
    CHECK(prUsdLayerFindField(read.layer, worldIndex, "active") == nullptr);

    CHECK(statusOf("def \"A\" {}\nover \"A\" {}\n") == PrStatus_InvalidData);
    CHECK(statusOf("def \"1A\" {}\n") == PrStatus_InvalidData);
    CHECK(statusOf("def \"A B\" {}\n") == PrStatus_InvalidData);
    CHECK(statusOf("def A {}\n") == PrStatus_Syntax);
    CHECK(statusOf("def \"A\" {\n") == PrStatus_Syntax);
    CHECK(statusOf("define \"A\" {}\n") == PrStatus_Syntax);
}

TEST_CASE("prims that nest past the reader's depth are refused as a limit") {
    String text;
    for (int i = 0; i < 300; ++i)
        text.append("def \"A\" {\n");
    for (int i = 0; i < 300; ++i)
        text.append("}\n");
    CHECK(statusOf(text) == PrStatus_Limit);
}

TEST_CASE("a prim's composition arcs read as list ops, as authored") {
    UsdRead read = primWith("    references = [\n"
        "        @./ref.usda@</Ref> (offset = 1; scale = 2; customData = { int q = 1 }),\n"
        "        </Sibling>,\n"
        "        @other.usda@,\n"
        "        <>\n"
        "    ]\n"
        "    prepend payload = @p.usda@ (\n offset = 3\n )\n"
        "    inherits = </Class>\n"
        "    append specializes = [<../Spec>, <Child>]\n"
        "    delete apiSchemas = [\"GoneAPI\"]\n"
        "    add clipSets = [\"x\"]\n"
        "    reorder inactiveIds = [3, -4]\n");
    REQUIRE(read.status == PrStatus_Ok);

    const PrUsdListOp& references = usdListOp(read.value("/A", "references"));
    CHECK(references.itemType == PrUsdType_Reference);
    CHECK(references.isExplicit);
    REQUIRE(references.items[PrUsdListOpList_Explicit].count == 4);
    const PrUsdReference* items = static_cast<const PrUsdReference*>(references.items[PrUsdListOpList_Explicit].data);
    CHECK(items[0].assetPath == "./ref.usda");
    CHECK(items[0].primPath == "/Ref");
    CHECK(items[0].layerOffset.offset == 1);
    CHECK(items[0].layerOffset.scale == 2);
    REQUIRE(items[0].customData != nullptr);
    CHECK(items[0].customData->count == 1);
    CHECK(items[1].assetPath == "");
    CHECK(items[1].primPath == "/Sibling");
    CHECK(items[1].layerOffset.scale == 1);
    CHECK(items[1].customData == nullptr);
    CHECK(items[2].assetPath == "other.usda");
    CHECK(items[2].primPath == "");
    CHECK(items[3].assetPath == "");
    CHECK(items[3].primPath == "");
    CHECK(references.items[PrUsdListOpList_Prepended].count == 0);

    const PrUsdListOp& payload = usdListOp(read.value("/A", "payload"));
    CHECK(payload.itemType == PrUsdType_Payload);
    CHECK(!payload.isExplicit);
    REQUIRE(payload.items[PrUsdListOpList_Prepended].count == 1);
    const PrUsdReference* payloads = static_cast<const PrUsdReference*>(payload.items[PrUsdListOpList_Prepended].data);
    CHECK(payloads[0].assetPath == "p.usda");
    CHECK(payloads[0].layerOffset.offset == 3);
    CHECK(payloads[0].customData == nullptr);

    // A relative path is made absolute against the prim that authors it.
    const PrUsdListOp& inherits = usdListOp(read.value("/A", "inheritPaths"));
    CHECK(inherits.isExplicit);
    CHECK(itemsOf(inherits, PrUsdListOpList_Explicit, 1)[0] == "/Class");
    const PrUsdListOp& specializes = usdListOp(read.value("/A", "specializes"));
    CHECK(!specializes.isExplicit);
    CHECK(itemsOf(specializes, PrUsdListOpList_Appended, 2)[0] == "/Spec");
    CHECK(itemsOf(specializes, PrUsdListOpList_Appended, 2)[1] == "/A/Child");

    CHECK(itemsOf(usdListOp(read.value("/A", "apiSchemas")), PrUsdListOpList_Deleted, 1)[0] == "GoneAPI");
    CHECK(usdListOp(read.value("/A", "apiSchemas")).itemType == PrUsdType_Token);
    CHECK(itemsOf(usdListOp(read.value("/A", "clipSets")), PrUsdListOpList_Added, 1)[0] == "x");
    const PrUsdListOp& ids = usdListOp(read.value("/A", "inactiveIds"));
    CHECK(ids.itemType == PrUsdType_Int64);
    REQUIRE(ids.items[PrUsdListOpList_Ordered].count == 2);
    CHECK(static_cast<const int64_t*>(ids.items[PrUsdListOpList_Ordered].data)[1] == -4);
}

TEST_CASE("the explicit list of a list op and the lists that edit replace each other") {
    UsdRead first = primWith("    payload = </Bar>\n    prepend payload = </Baz>\n    append payload = </Qux>");
    REQUIRE(first.status == PrStatus_Ok);
    const PrUsdListOp& edits = usdListOp(first.value("/A", "payload"));
    CHECK(!edits.isExplicit);
    CHECK(edits.items[PrUsdListOpList_Explicit].count == 0);
    CHECK(edits.items[PrUsdListOpList_Prepended].count == 1);
    CHECK(edits.items[PrUsdListOpList_Appended].count == 1);

    UsdRead second = primWith("    prepend payload = </Baz>\n    payload = </Bar>");
    REQUIRE(second.status == PrStatus_Ok);
    const PrUsdListOp& replaced = usdListOp(second.value("/A", "payload"));
    CHECK(replaced.isExplicit);
    CHECK(replaced.items[PrUsdListOpList_Prepended].count == 0);
    CHECK(replaced.items[PrUsdListOpList_Explicit].count == 1);

    // The same list assigned twice keeps the last, and None is an explicit list of nothing.
    UsdRead third = primWith("    prepend apiSchemas = [\"A\"]\n    prepend apiSchemas = [\"B\", \"C\"]\n"
        "    references = None\n    inherits = None");
    REQUIRE(third.status == PrStatus_Ok);
    CHECK(itemsOf(usdListOp(third.value("/A", "apiSchemas")), PrUsdListOpList_Prepended, 2)[0] == "B");
    CHECK(usdListOp(third.value("/A", "references")).isExplicit);
    CHECK(usdListOp(third.value("/A", "references")).items[PrUsdListOpList_Explicit].count == 0);
    CHECK(usdListOp(third.value("/A", "inheritPaths")).isExplicit);
}

TEST_CASE("a prim's arcs that break the specification's rules are refused") {
    CHECK(primWith("    references = <../Sibling>").status == PrStatus_InvalidData);
    CHECK(primWith("    references = </B.x>").status == PrStatus_InvalidData);
    CHECK(primWith("    references = </B{v=x}C>").status == PrStatus_InvalidData);
    CHECK(primWith("    references = @@").status == PrStatus_InvalidData);
    CHECK(primWith("    prepend references = None").status == PrStatus_InvalidData);
    CHECK(primWith("    prepend inherits = None").status == PrStatus_InvalidData);
    CHECK(primWith("    payload = @a@ (customData = {})").status == PrStatus_Syntax);
    CHECK(primWith("    inherits = <../../B>").status == PrStatus_InvalidData);
    CHECK(primWith("    inherits = </B{v=x}>").status == PrStatus_InvalidData);
    CHECK(primWith("    inherits = <not a path>").status == PrStatus_Syntax);
    CHECK(primWith("    prepend apiSchemas = [\"A\", \"B\", \"A\"]").status == PrStatus_InvalidData);
    CHECK(primWith("    prepend apiSchemas = \"A\"").status == PrStatus_Syntax);
    CHECK(primWith("    prepend active = [true]").status == PrStatus_InvalidData);
    CHECK(primWith("    references = [@a@ @b@]").status == PrStatus_Syntax);
}

TEST_CASE("a prim's other metadata reads by the type of each field") {
    UsdRead read = primWith("    \"a prim's comment\"\n"
        "    doc = 'single quotes'\n"
        "    kind = \"component\"\n"
        "    active = false\n"
        "    instanceable = 1\n"
        "    hidden = \"yes\"\n"
        "    displayName = \"the prim\"\n"
        "    customData = { string s = \"v\" }\n"
        "    assetInfo = None\n"
        "    permission = private\n"
        "    symmetryFunction = mirror\n"
        "    prefixSubstitutions = { \"a\": \"b\" }\n"
        "    prepend unknownList = [1, \"two\"]\n");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(stringOf(read.value("/A", "comment")) == "a prim's comment");
    CHECK(stringOf(read.value("/A", "documentation")) == "single quotes");
    CHECK(read.value("/A", "kind").type == PrUsdType_Token);
    CHECK(stringOf(read.value("/A", "kind")) == "component");
    CHECK(!usdScalar<bool>(read.value("/A", "active")));
    CHECK(usdScalar<bool>(read.value("/A", "instanceable")));
    CHECK(usdScalar<bool>(read.value("/A", "hidden")));
    CHECK(read.value("/A", "displayName").type == PrUsdType_String);
    CHECK(static_cast<const PrUsdDictionary*>(read.value("/A", "customData").data)->count == 1);
    CHECK(static_cast<const PrUsdDictionary*>(read.value("/A", "assetInfo").data)->count == 0);
    CHECK(usdScalar<PrUsdPermission>(read.value("/A", "permission")) == PrUsdPermission_Private);
    CHECK(stringOf(read.value("/A", "symmetryFunction")) == "mirror");
    CHECK(static_cast<const PrUsdDictionary*>(read.value("/A", "prefixSubstitutions").data)->count == 1);
    const PrUsdListOp& unknown = usdListOp(read.value("/A", "unknownList"));
    CHECK(unknown.itemType == PrUsdType_Unregistered);
    CHECK(itemsOf(unknown, PrUsdListOpList_Prepended, 2)[1] == "\"two\"");

    CHECK(primWith("    kind = component").status == PrStatus_Syntax);
    CHECK(primWith("    active = maybe").status == PrStatus_InvalidData);
    CHECK(primWith("    permission = secret").status == PrStatus_Syntax);
    CHECK(primWith("    specifier = \"def\"").status == PrStatus_InvalidData);
    CHECK(primWith("    relocates = { <B>: <C> }").status == PrStatus_Syntax);
}

TEST_CASE("a dictionary's entries are typed, nest, and are sorted by key") {
    UsdRead read = primWith("    customData = {\n"
        "        string zebra = \"last\"\n"
        "        token \"a key\" = \"quoted\"\n"
        "        int[] numbers = [1, 2]\n"
        "        double3 point = (1, 2, 3)\n"
        "        color3f tint = (0, 1, 0)\n"
        "        bool def = 0\n"
        "        asset file = @a.png@\n"
        "        dictionary nested = { dictionary deeper = { half h = 0.5 } }\n"
        "        int twice = 1; int twice = 2\n"
        "    }");
    REQUIRE(read.status == PrStatus_Ok);
    const PrUsdDictionary* dictionary = static_cast<const PrUsdDictionary*>(read.value("/A", "customData").data);
    REQUIRE(dictionary->count == 9);
    for (uint32_t i = 1; i < dictionary->count; ++i)
        CHECK(prStringCompare(dictionary->entries[i - 1].key, dictionary->entries[i].key) < 0);
    auto entry = [&](PrStringView key) -> const PrUsdValue& {
        for (uint32_t i = 0; i < dictionary->count; ++i) {
            if (dictionary->entries[i].key == key)
                return dictionary->entries[i].value;
        }
        FAIL("the dictionary has no entry of the key");
        return dictionary->entries[0].value;
    };
    CHECK(entry("zebra").type == PrUsdType_String);
    CHECK(entry("a key").type == PrUsdType_Token);
    CHECK(usdElements<int32_t>(entry("numbers"), 2)[1] == 2);
    CHECK(usdScalar<PrDVec3>(entry("point")) == PrDVec3{1, 2, 3});
    CHECK(entry("tint").type == PrUsdType_Float3);
    CHECK(!usdScalar<bool>(entry("def")));
    CHECK(entry("file").type == PrUsdType_Asset);
    CHECK(usdScalar<int32_t>(entry("twice")) == 2);
    const PrUsdDictionary* nested = static_cast<const PrUsdDictionary*>(entry("nested").data);
    REQUIRE(nested->count == 1);
    const PrUsdDictionary* deeper = static_cast<const PrUsdDictionary*>(nested->entries[0].value.data);
    CHECK(deeper->entries[0].value.type == PrUsdType_Half);

    CHECK(primWith("    customData = { string a:b = \"c\" }").status == PrStatus_Syntax);
    CHECK(primWith("    customData = { nonsense a = 1 }").status == PrStatus_InvalidData);
    CHECK(primWith("    customData = { int a = 1 int b = 2 }").status == PrStatus_Syntax);
    CHECK(primWith("    customData = { int a = \"one\" }").status == PrStatus_InvalidData);
}

TEST_CASE("an attribute's value has the type that its declaration names") {
    struct Case {
        PrStringView declaration;
        PrUsdType type;
        bool isArray;
        uint32_t count;
    };
    const Case cases[] = {
        {"bool x = true", PrUsdType_Bool, false, 1},
        {"uchar x = 255", PrUsdType_UChar, false, 1},
        {"int x = -7", PrUsdType_Int, false, 1},
        {"uint x = 4000000000", PrUsdType_UInt, false, 1},
        {"int64 x = -9223372036854775808", PrUsdType_Int64, false, 1},
        {"uint64 x = 18446744073709551615", PrUsdType_UInt64, false, 1},
        {"half x = 0.5", PrUsdType_Half, false, 1},
        {"float x = 0.1", PrUsdType_Float, false, 1},
        {"double x = 0.1", PrUsdType_Double, false, 1},
        {"timecode x = 24", PrUsdType_TimeCode, false, 1},
        {"string x = \"s\"", PrUsdType_String, false, 1},
        {"token x = \"t\"", PrUsdType_Token, false, 1},
        {"asset x = @a.png@", PrUsdType_Asset, false, 1},
        {"pathExpression x = \"//a\"", PrUsdType_PathExpression, false, 1},
        {"int2 x = (1, 2)", PrUsdType_Int2, false, 1},
        {"int3 x = (1, 2, 3)", PrUsdType_Int3, false, 1},
        {"int4 x = (1, 2, 3, 4)", PrUsdType_Int4, false, 1},
        {"half2 x = (1, 2)", PrUsdType_Half2, false, 1},
        {"half3 x = (1, 2, 3)", PrUsdType_Half3, false, 1},
        {"half4 x = (1, 2, 3, 4)", PrUsdType_Half4, false, 1},
        {"float2 x = (1, 2)", PrUsdType_Float2, false, 1},
        {"float3 x = (1, 2, 3)", PrUsdType_Float3, false, 1},
        {"float4 x = (1, 2, 3, 4)", PrUsdType_Float4, false, 1},
        {"double2 x = (1, 2)", PrUsdType_Double2, false, 1},
        {"double3 x = (1, 2, 3)", PrUsdType_Double3, false, 1},
        {"double4 x = (1, 2, 3, 4)", PrUsdType_Double4, false, 1},
        {"quath x = (1, 0, 0, 0)", PrUsdType_Quath, false, 1},
        {"quatf x = (1, 0, 0, 0)", PrUsdType_Quatf, false, 1},
        {"quatd x = (1, 0, 0, 0)", PrUsdType_Quatd, false, 1},
        {"matrix2d x = ((1, 2), (3, 4))", PrUsdType_Matrix2d, false, 1},
        {"matrix3d x = ((1, 2, 3), (4, 5, 6), (7, 8, 9))", PrUsdType_Matrix3d, false, 1},
        {"matrix4d x = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 6, 7, 1))", PrUsdType_Matrix4d, false, 1},
        {"color3f x = (1, 0, 0)", PrUsdType_Float3, false, 1},
        {"point3d x = (1, 0, 0)", PrUsdType_Double3, false, 1},
        {"texCoord2h x = (1, 0)", PrUsdType_Half2, false, 1},
        {"frame4d x = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))", PrUsdType_Matrix4d, false, 1},
        {"int[] x = [1, 2, 3,]", PrUsdType_Int, true, 3},
        {"int[] x = []", PrUsdType_Int, true, 0},
        {"float3[] x = [(1, 2, 3), (4, 5, 6)]", PrUsdType_Float3, true, 2},
        {"token [] x = [\"a\", \"b\"]", PrUsdType_Token, true, 2},
        {"asset[] x = [@a@, @@@b@c@@@]", PrUsdType_Asset, true, 2},
        {"matrix4d[] x = [((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))]", PrUsdType_Matrix4d, true, 1},
        {"quatf[] x = [(1, 0, 0, 0), (0, 1, 0, 0)]", PrUsdType_Quatf, true, 2},
        {"timecode[] x = [1, 2.5]", PrUsdType_TimeCode, true, 2},
        {"bool[] x = [true, 0, \"on\"]", PrUsdType_Bool, true, 3},
    };
    for (const Case& test : cases) {
        CAPTURE(String(test.declaration));
        UsdRead read = primOf(test.declaration);
        PrUsdValue value = defaultOf(read);
        CHECK(value.type == test.type);
        CHECK(value.isArray == test.isArray);
        CHECK(value.count == test.count);
        CHECK(usdScalar<bool>(read.value("/A.x", "custom")) == false);
        CHECK(usdScalar<PrUsdVariability>(read.value("/A.x", "variability")) == PrUsdVariability_Varying);
    }
}

TEST_CASE("numbers read to the bit of their type") {
    // A number is read as a double, correctly rounded, and narrowed to the attribute's type.
    CHECK(usdScalar<float>(defaultOf(primOf("float x = 0.1"))) == 0.1f);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = 0.1"))) == 0.1);
    CHECK(prFloat16ToFloat(usdScalar<PrFloat16>(defaultOf(primOf("half x = 0.1")))) == 0.0999755859375f);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = .5"))) == 0.5);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = 5."))) == 5.0);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = -.5e-2"))) == -0.005);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = 1E3"))) == 1000.0);
    CHECK(usdScalar<double>(defaultOf(primOf("double x = 18446744073709551616"))) == 18446744073709551616.0);
    CHECK(usdScalar<float>(defaultOf(primOf("float x = 16777217"))) == 16777216.0f);
    CHECK(usdScalar<float>(defaultOf(primOf("float x = inf"))) == kPrF32Infinity);
    CHECK(usdScalar<float>(defaultOf(primOf("float x = -inf"))) == -kPrF32Infinity);
    float nan = usdScalar<float>(defaultOf(primOf("float x = nan")));
    CHECK(nan != nan);
    double negativeZero = usdScalar<double>(defaultOf(primOf("double x = -0")));
    CHECK(negativeZero == 0);
    CHECK(1 / negativeZero < 0);
    CHECK(usdScalar<int32_t>(defaultOf(primOf("int x = -0"))) == 0);
    // A number with a fraction loses it in an integer.
    CHECK(usdScalar<int32_t>(defaultOf(primOf("int x = 1.9"))) == 1);
    CHECK(usdScalar<PrIVec2>(defaultOf(primOf("int2 x = (1.5, -2.5)"))) == PrIVec2{1, -2});

    // A quaternion's real part is first in the text and last in the value, and a matrix is the transpose of the text's
    // rows.
    PrQuat quaternion = usdScalar<PrQuat>(defaultOf(primOf("quatf x = (1, 2, 3, 4)")));
    CHECK(quaternion.w == 1);
    CHECK(quaternion.x == 2);
    CHECK(quaternion.z == 4);
    PrDMatrix4 matrix = usdScalar<PrDMatrix4>(defaultOf(primOf(
        "matrix4d x = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 6, 7, 1))")));
    CHECK(matrix.m[0][3] == 5);
    CHECK(matrix.m[1][3] == 6);
    CHECK(matrix.m[2][3] == 7);
    CHECK(matrix.m[3][0] == 0);
    PrDMatrix2 small = usdScalar<PrDMatrix2>(defaultOf(primOf("matrix2d x = ((1, 2), (3, 4))")));
    CHECK(small.m[0][1] == 3);
    CHECK(small.m[1][0] == 2);

    const PrStringView badCases[] = {
        "int x = 3000000000", "uchar x = 256",
        "uint x = -1", "int64 x = 9223372036854775808",
        "uint64 x = 18446744073709551616",
    };
    for (PrStringView bad : badCases) {
        CAPTURE(String(bad));
        CHECK(primOf(bad).status == PrStatus_InvalidData);
    }
    CHECK(primOf("float x = +1").status == PrStatus_InvalidData);
    CHECK(primOf("int x = 0x10").status == PrStatus_Syntax);
    CHECK(primOf("float x = 1e").status == PrStatus_Syntax);
}

TEST_CASE("a value that is not of its attribute's type is refused") {
    const PrStringView badCases[] = {
        "int[] x = 3", "int x = [3]",
        "float3 x = (1, 2)", "float3 x = (1, 2, 3, 4)",
        "float3 x = 1", "matrix2d x = [(1, 2), (3, 4)]",
        "matrix2d x = ((1, 2))", "string x = 1", "asset x = \"a\"",
        "string[] x = [\"a\", [\"b\"]]", "opaque x = 1",
        "bool x = \"perhaps\"",
    };
    for (PrStringView bad : badCases) {
        CAPTURE(String(bad));
        CHECK(primOf(bad).status == PrStatus_InvalidData);
    }
}

TEST_CASE("strings decode their escapes, and asset paths end at their at signs") {
    CHECK(stringOf(defaultOf(primOf("string x = \"a\\n\\t\\\\\\\"\\x41\\101\\q\""))) == "a\n\t\\\"AAq");
    CHECK(stringOf(defaultOf(primOf("string x = 'it is \"quoted\"'"))) == "it is \"quoted\"");
    CHECK(stringOf(defaultOf(primOf("string x = \"\"\"one\n\"two\"\nthree\"\"\""))) == "one\n\"two\"\nthree");
    CHECK(stringOf(defaultOf(primOf("string x = '''a ' b'''"))) == "a ' b");
    CHECK(stringOf(defaultOf(primOf("string x = \"h\xc3\xa9llo\""))) == "h\xc3\xa9llo");
    CHECK(stringOf(defaultOf(primOf("string x = \"\""))) == "");
    CHECK(stringOf(defaultOf(primOf("asset x = @./a b.png@"))) == "./a b.png");
    CHECK(stringOf(defaultOf(primOf("asset x = @@@a@b@@@"))) == "a@b");
    CHECK(stringOf(defaultOf(primOf("asset x = @@@a\\@@@b@@@"))) == "a@@@b");
    CHECK(stringOf(defaultOf(primOf("asset x = @@"))) == "");
    CHECK(primOf("string x = \"line\nbreak\"").status == PrStatus_Syntax);
    CHECK(primOf("string x = \"never closed").status == PrStatus_Syntax);
    CHECK(primOf("asset x = @never closed").status == PrStatus_Syntax);
}

TEST_CASE("an attribute's declaration reads into its required fields") {
    UsdRead read = primOf("    custom uniform token[] primvars:names = [\"a\"] (\n"
        "        doc = \"documented\"\n"
        "        interpolation = \"vertex\"\n"
        "        elementSize = 2\n"
        "        allowedTokens = [\"a\", \"b\"]\n"
        "        displayUnit = cm\n"
        "    )\n"
        "    config int legacy\n"
        "    float blocked = None\n"
        "    opaque handle\n"
        "    myType strange = (1, \"two\")\n"
        "    myType pointer = </B>\n"
        "    int again = 1\n"
        "    int again = 2\n");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(read.spec("/A.primvars:names").form == PrUsdSpecForm_Attribute);
    CHECK(read.spec("/A.primvars:names").name == "primvars:names");
    CHECK(usdScalar<bool>(read.value("/A.primvars:names", "custom")));
    CHECK(usdScalar<PrUsdVariability>(read.value("/A.primvars:names", "variability")) == PrUsdVariability_Uniform);
    CHECK(stringOf(read.value("/A.primvars:names", "typeName")) == "token[]");
    CHECK(stringOf(read.value("/A.primvars:names", "documentation")) == "documented");
    CHECK(stringOf(read.value("/A.primvars:names", "interpolation")) == "vertex");
    CHECK(usdScalar<int32_t>(read.value("/A.primvars:names", "elementSize")) == 2);
    CHECK(read.value("/A.primvars:names", "allowedTokens").count == 2);
    CHECK(stringOf(read.value("/A.primvars:names", "displayUnit")) == "cm");
    CHECK(usdScalar<PrUsdVariability>(read.value("/A.legacy", "variability")) == PrUsdVariability_Uniform);
    CHECK(!read.has("/A.legacy", "default"));
    // None is the value block, and an opaque attribute has no value.
    CHECK(read.value("/A.blocked", "default").type == PrUsdType_Block);
    CHECK(read.value("/A.blocked", "default").count == 0);
    CHECK(stringOf(read.value("/A.handle", "typeName")) == "opaque");
    // A value of a type that is not the specification's keeps its text, or its path.
    CHECK(read.value("/A.strange", "default").type == PrUsdType_Unregistered);
    CHECK(stringOf(read.value("/A.strange", "default")) == "(1, \"two\")");
    CHECK(read.value("/A.pointer", "default").type == PrUsdType_Path);
    CHECK(usdScalar<int32_t>(read.value("/A.again", "default")) == 2);

    PrUsdType type = PrUsdType_Block;
    bool isArray = false;
    CHECK(prUsdTypeFromName("color3f[]", &type, &isArray));
    CHECK(type == PrUsdType_Float3);
    CHECK(isArray);
    CHECK(!prUsdTypeFromName("opaque", &type, &isArray));
    CHECK(!prUsdTypeFromName("myType", &type, &isArray));
    CHECK(prUsdTypeName(PrUsdType_Matrix4d) == "matrix4d");

    CHECK(primOf("int x = 1\n float x = 2").status == PrStatus_InvalidData);
    CHECK(primOf("int x = 1\n rel x").status == PrStatus_InvalidData);
    CHECK(primOf("uniform custom int x = 1").status == PrStatus_Syntax);
    CHECK(primOf("varying int x = 1").status == PrStatus_Syntax);
    CHECK(primOf("int a:1b = 1").status == PrStatus_Syntax);
    CHECK(primOf("int x.default = 1").status == PrStatus_Syntax);
    CHECK(primOf("int = 1").status == PrStatus_Syntax);
}

TEST_CASE("an attribute's time samples are sorted by time, and the last of a time is kept") {
    UsdRead read = primOf("    double x.timeSamples = { 2: 3.5, -1.5: None, 2: 4.5, 1e2: 6, }\n"
        "    float3[] p.timeSamples = {\n        0: [(1, 2, 3)],\n        1: [],\n    }\n"
        "    int empty.timeSamples = {}\n"
        "    double x = 9\n");
    REQUIRE(read.status == PrStatus_Ok);
    const PrUsdTimeSamples* samples = static_cast<const PrUsdTimeSamples*>(read.value("/A.x", "timeSamples").data);
    REQUIRE(samples->count == 3);
    CHECK(samples->times[0] == -1.5);
    CHECK(samples->values[0].type == PrUsdType_Block);
    CHECK(samples->times[1] == 2);
    CHECK(usdScalar<double>(samples->values[1]) == 4.5);
    CHECK(samples->times[2] == 100);
    // An attribute has samples and a default at once.
    CHECK(usdScalar<double>(read.value("/A.x", "default")) == 9);
    const PrUsdTimeSamples* points = static_cast<const PrUsdTimeSamples*>(read.value("/A.p", "timeSamples").data);
    REQUIRE(points->count == 2);
    CHECK(points->values[0].isArray);
    CHECK(points->values[0].type == PrUsdType_Float3);
    CHECK(points->values[1].count == 0);
    CHECK(static_cast<const PrUsdTimeSamples*>(read.value("/A.empty", "timeSamples").data)->count == 0);

    CHECK(primOf("double x.timeSamples = { nan: 1 }").status == PrStatus_InvalidData);
    CHECK(primOf("double x.timeSamples = { 1: \"one\" }").status == PrStatus_InvalidData);
    CHECK(primOf("double x.timeSamples = { 1 2 }").status == PrStatus_Syntax);
    CHECK(primOf("double x.timeSamples = { 1: 2 3: 4 }").status == PrStatus_Syntax);
    CHECK(primOf("double x.spline = { 1: 2 }").status == PrStatus_NotSupported);
}

TEST_CASE("an attribute's connections and a relationship's targets read as list ops of paths") {
    UsdRead read = primOf("    float a.connect = <../Other.out>\n"
        "    prepend float b.connect = [<Child.x>, <.f>]\n"
        "    float c.connect = None\n"
        "    rel r = <../Target>\n"
        "    custom rel r2 = [</B>, <C/D.p>, </E.f[/G].h>]\n"
        "    prepend rel r3 = <.f>\n"
        "    add rel r3 = </H>\n"
        "    delete rel r3 = </I>\n"
        "    reorder rel r3 = [</H>]\n"
        "    rel none = None\n"
        "    rel unvalued\n"
        "    varying rel legacy (doc = \"documented\")\n"
        "    rel fallback.default = </J>\n");
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(itemsOf(usdListOp(read.value("/A.a", "connectionPaths")), PrUsdListOpList_Explicit, 1)[0] == "/Other.out");
    CHECK(itemsOf(usdListOp(read.value("/A.b", "connectionPaths")), PrUsdListOpList_Prepended, 2)[0] == "/A/Child.x");
    CHECK(itemsOf(usdListOp(read.value("/A.b", "connectionPaths")), PrUsdListOpList_Prepended, 2)[1] == "/A.f");
    CHECK(usdListOp(read.value("/A.c", "connectionPaths")).isExplicit);

    CHECK(read.spec("/A.r").form == PrUsdSpecForm_Relationship);
    CHECK(itemsOf(usdListOp(read.value("/A.r", "targetPaths")), PrUsdListOpList_Explicit, 1)[0] == "/Target");
    CHECK(usdScalar<PrUsdVariability>(read.value("/A.r", "variability")) == PrUsdVariability_Uniform);
    CHECK(!usdScalar<bool>(read.value("/A.r", "custom")));
    CHECK(usdScalar<bool>(read.value("/A.r2", "custom")));
    const PrStringView* targets = itemsOf(usdListOp(read.value("/A.r2", "targetPaths")), PrUsdListOpList_Explicit, 3);
    CHECK(targets[1] == "/A/C/D.p");
    CHECK(targets[2] == "/E.f[/G].h");
    const PrUsdListOp& edits = usdListOp(read.value("/A.r3", "targetPaths"));
    CHECK(!edits.isExplicit);
    CHECK(itemsOf(edits, PrUsdListOpList_Prepended, 1)[0] == "/A.f");
    CHECK(itemsOf(edits, PrUsdListOpList_Added, 1)[0] == "/H");
    CHECK(itemsOf(edits, PrUsdListOpList_Deleted, 1)[0] == "/I");
    CHECK(itemsOf(edits, PrUsdListOpList_Ordered, 1)[0] == "/H");
    CHECK(usdListOp(read.value("/A.none", "targetPaths")).isExplicit);
    CHECK(!read.has("/A.unvalued", "targetPaths"));
    CHECK(usdScalar<PrUsdVariability>(read.value("/A.legacy", "variability")) == PrUsdVariability_Varying);
    CHECK(stringOf(read.value("/A.legacy", "documentation")) == "documented");
    CHECK(stringOf(read.value("/A.fallback", "default")) == "/J");

    CHECK(primOf("rel r = </B{v=x}C>").status == PrStatus_InvalidData);
    CHECK(primOf("rel r = <../../B>").status == PrStatus_InvalidData);
    CHECK(primOf("rel r = [</B>, </C>, </B>]").status == PrStatus_InvalidData);
    CHECK(primOf("rel r = <.>").status == PrStatus_Syntax);
    CHECK(primOf("rel r = </B/>").status == PrStatus_Syntax);
    CHECK(primOf("rel r = </B//C>").status == PrStatus_Syntax);
    CHECK(primOf("rel r = </B.x:>").status == PrStatus_Syntax);
    CHECK(primOf("prepend rel r = None").status == PrStatus_InvalidData);
    CHECK(primOf("rel r.timeSamples = { 1: </B> }").status == PrStatus_Syntax);
    CHECK(primOf("prepend int x = 1").status == PrStatus_Syntax);
}

TEST_CASE("variant sets and variants are specs that hold what a prim holds") {
    UsdRead read = usdReadText("def \"A\" (\n"
        "    variants = { string shape = \"round\"; string size = \"\" }\n"
        "    variantSets = [\"declared\", \"shape\"]\n"
        ")\n"
        "{\n"
        "    variantSet \"shape\" = {\n"
        "        \"round\" ( doc = \"documented\" ) {\n"
        "            float radius = 1\n"
        "            float out.connect = <.radius>\n"
        "            def Sphere \"S\" {\n"
        "                rel back = <../..>\n"
        "            }\n"
        "            variantSet \"nested\" = {\n"
        "                \"n1\" {\n"
        "                    def \"N\" {}\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        \"1-b|c\" {\n"
        "        }\n"
        "        \".dot\" {}\n"
        "    }\n"
        "}\n");
    REQUIRE(read.status == PrStatus_Ok);
    const PrUsdVariantSelection* selections = usdElements<PrUsdVariantSelection>(read.value("/A", "variantSelection"), 2);
    CHECK(selections[0].variantSet == "shape");
    CHECK(selections[0].variant == "round");
    CHECK(selections[1].variant == "");
    CHECK(itemsOf(usdListOp(read.value("/A", "variantSetNames")), PrUsdListOpList_Explicit, 2)[0] == "declared");

    // An explicit list of variant sets declares each of them, in its order.
    const PrUsdSpec& prim = read.spec("/A");
    REQUIRE(prim.variantSets.count == 2);
    const PrUsdSpec* specs = prUsdLayerSpecs(read.layer);
    const PrUsdSpecIndex* sets = prUsdLayerList(read.layer, prim.variantSets);
    CHECK(specs[sets[0]].path == "/A{declared=}");
    CHECK(specs[sets[0]].variants.count == 0);
    const PrUsdSpec& shape = specs[sets[1]];
    CHECK(shape.form == PrUsdSpecForm_VariantSet);
    CHECK(shape.name == "shape");
    CHECK(shape.path == "/A{shape=}");
    CHECK(shape.fieldCount == 0);
    REQUIRE(shape.variants.count == 3);
    const PrUsdSpec& round = specs[prUsdLayerList(read.layer, shape.variants)[0]];
    CHECK(round.form == PrUsdSpecForm_Variant);
    CHECK(round.name == "round");
    CHECK(round.path == "/A{shape=round}");
    CHECK(usdScalar<PrUsdSpecifier>(read.value("/A{shape=round}", "specifier")) == PrUsdSpecifier_Over);
    CHECK(stringOf(read.value("/A{shape=round}", "documentation")) == "documented");
    CHECK(round.properties.count == 2);
    CHECK(round.prims.count == 1);
    CHECK(round.variantSets.count == 1);
    CHECK(read.spec("/A{shape=round}.radius").form == PrUsdSpecForm_Attribute);
    CHECK(read.spec("/A{shape=round}S").form == PrUsdSpecForm_Prim);
    CHECK(read.spec("/A{shape=round}S.back").form == PrUsdSpecForm_Relationship);
    CHECK(read.spec("/A{shape=round}{nested=n1}N").parent == prUsdLayerFindSpec(read.layer,
        "/A{shape=round}{nested=n1}"));
    CHECK(read.spec("/A{shape=1-b|c}").name == "1-b|c");
    // A connection drops the variant selections of the prim that authors it, and a target must lie outside them.
    CHECK(itemsOf(usdListOp(read.value("/A{shape=round}.out", "connectionPaths")), PrUsdListOpList_Explicit, 1)[0] ==
        "/A.radius");
    CHECK(itemsOf(usdListOp(read.value("/A{shape=round}S.back", "targetPaths")), PrUsdListOpList_Explicit, 1)[0] == "/A");

    CHECK(primOf("variantSet \"v\" = {\n \"x\" {\n rel r = <Kid>\n }\n }").status == PrStatus_InvalidData);
    CHECK(primOf("variantSet \"v\" = {\n \"x\" {\n def \"C\" (inherits = <../X>) {}\n }\n }").status ==
        PrStatus_InvalidData);
    CHECK(primOf("variantSet \"v\" = {\n }").status == PrStatus_Syntax);
    CHECK(primOf("variantSet \"v\" = {\n \"x\" {}\n \"x\" {}\n }").status == PrStatus_InvalidData);
    CHECK(primOf("variantSet \"v\" = {\n \"x\" {}\n }\n variantSet \"v\" = {\n \"y\" {}\n }").status ==
        PrStatus_InvalidData);
    CHECK(primOf("variantSet \"1v\" = {\n \"x\" {}\n }").status == PrStatus_InvalidData);
    CHECK(primOf("variantSet \"v\" = {\n \"a b\" {}\n }").status == PrStatus_InvalidData);
    CHECK(primWith("    variants = { int shape = 1 }").status == PrStatus_InvalidData);
    CHECK(primWith("    variantSets = \"1v\"").status == PrStatus_InvalidData);
    // A list that edits declares no variant set.
    UsdRead edits = primWith("    prepend variantSets = [\"a\"]");
    REQUIRE(edits.status == PrStatus_Ok);
    CHECK(edits.spec("/A").variantSets.count == 0);
}

TEST_CASE("each spec of a text layer has the span of its text, which finds the spec at an offset") {
    String text("#usda 1.0\n");
    PrStringView body = "def Xform \"World\" (\n    kind = \"group\"\n)\n{\n    float size = 2\n"
        "    variantSet \"look\" = {\n        \"red\" {\n            def \"Paint\" {}\n        }\n    }\n"
        "    rel target = </World>\n}\n";
    text.append(body);
    UsdRead read = usdReadText(body);
    REQUIRE(read.status == PrStatus_Ok);
    auto spanText = [&](PrStringView path) {
        PrUsdTextSpan span = prUsdLayerSpecSpan(read.layer, prUsdLayerFindSpec(read.layer, path));
        return prStringSlice(text, span.begin, span.end - span.begin);
    };
    auto at = [&](PrStringView needle) {
        PrUsdSpecIndex spec = prUsdLayerSpecFromOffset(read.layer, uint32_t(prStringFind(text, needle)));
        return spec != kPrUsdNone ? prUsdLayerSpecs(read.layer)[spec].path : PrStringView{};
    };
    CHECK(prUsdLayerSpecSpan(read.layer, kPrUsdLayerSpec).end == text.count());
    CHECK(prStringStartsWith(spanText("/World"), "def Xform \"World\""));
    CHECK(prStringEndsWith(spanText("/World"), "}"));
    CHECK(spanText("/World.size") == "float size = 2");
    CHECK(spanText("/World.target") == "rel target = </World>");
    CHECK(prStringStartsWith(spanText("/World{look=}"), "variantSet \"look\""));
    CHECK(prStringStartsWith(spanText("/World{look=red}"), "\"red\""));
    CHECK(spanText("/World{look=red}Paint") == "def \"Paint\" {}");

    // An offset finds the innermost spec whose text holds it: a property, a prim in a variant, or the prim around them.
    CHECK(at("size") == "/World.size");
    CHECK(at("Paint") == "/World{look=red}Paint");
    CHECK(at("kind") == "/World");
    CHECK(at("#usda") == "/");
}
