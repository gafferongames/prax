#include "pr/materialx.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A document as the specification writes one: a node graph with an image and a normal map, a shader that connects to
// it, and a material.
constexpr PrStringView kDocument = R"(<?xml version="1.0"?>
<materialx version="1.39" colorspace="lin_rec709">
  <!-- A comment is no element. -->
  <nodegraph name="NG_wood" fileprefix="textures/">
    <image name="color" type="color3">
      <input name="file" type="filename" value="wood_color.png" colorspace="srgb_texture" />
    </image>
    <image name="bumps" type="vector3">
      <input name="file" type="filename" value="wood_normal.png" />
    </image>
    <normalmap name="mapped" type="vector3">
      <input name="in" type="vector3" nodename="bumps" />
    </normalmap>
    <output name="color_out" type="color3" nodename="color" />
    <output name="normal_out" type="vector3" nodename="mapped" />
  </nodegraph>
  <standard_surface name="SR_wood" type="surfaceshader">
    <input name="base_color" type="color3" nodegraph="NG_wood" output="color_out" />
    <input name="specular_roughness" type="float" value="0.4" />
  </standard_surface>
  <surfacematerial name="M_wood" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_wood" />
  </surfacematerial>
</materialx>
)";

PrMaterialxDocument* read(PrStringView source, PrStatus expected = PrStatus_Ok, PrStringView diagnosis = {}) {
    PrTextBuffer diagnostics = {};
    PrMaterialxDocument* document = nullptr;
    CHECK(prMaterialxDocumentCreate(source, &diagnostics, &document) == expected);
    if (diagnosis.size != 0)
        CHECK(prStringContains(prTextBufferView(&diagnostics), diagnosis));
    prTextBufferReset(&diagnostics);
    return document;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a document is its elements as a tree, with their attributes in the order of the source") {
    PrMaterialxDocument* document = read(kDocument);
    REQUIRE(document != nullptr);
    uint32_t count = 0;
    const PrMaterialxElement* elements = prMaterialxDocumentElements(document, &count);
    CHECK(count == 15);
    CHECK(elements[0].category == "materialx");
    CHECK(elements[0].parent == kPrMaterialxNone);
    CHECK(prMaterialxAttribute(document, 0, "version") == "1.39");

    PrMaterialxElementIndex graph = prMaterialxChild(document, 0, "NG_wood");
    REQUIRE(graph != kPrMaterialxNone);
    CHECK(elements[graph].category == "nodegraph");
    PrMaterialxElementIndex color = elements[graph].firstChild;
    CHECK(elements[color].name == "color");
    CHECK(elements[color].type == "color3");
    CHECK(elements[elements[color].nextSibling].name == "bumps");
    PrMaterialxElementIndex file = prMaterialxChild(document, color, "file");
    REQUIRE(file != kPrMaterialxNone);
    CHECK(elements[file].parent == color);
    REQUIRE(elements[file].attributeCount == 4);
    CHECK(elements[file].attributes[0].name == "name");
    CHECK(elements[file].attributes[2].name == "value");
    CHECK(elements[file].attributes[2].value == "wood_color.png");
    CHECK(prMaterialxAttribute(document, file, "nodename").data == nullptr);
    CHECK(prMaterialxChild(document, graph, "missing") == kPrMaterialxNone);

    // A scoped attribute is the element's, or its nearest ancestor's.
    CHECK(prMaterialxScopedAttribute(document, file, "colorspace") == "srgb_texture");
    PrMaterialxElementIndex bumpsFile = prMaterialxChild(document, prMaterialxChild(document, graph, "bumps"), "file");
    CHECK(prMaterialxScopedAttribute(document, bumpsFile, "colorspace") == "lin_rec709");
    CHECK(prMaterialxScopedAttribute(document, bumpsFile, "fileprefix") == "textures/");
    CHECK(prMaterialxScopedAttribute(document, 0, "fileprefix").data == nullptr);
    prMaterialxDocumentDestroy(document);
}

TEST_CASE("a document is refused unless its root is materialx and its siblings' names are unique") {
    CHECK(read("<materialx><nodegraph name=\"a\"/><nodegraph name=\"a\"/></materialx>", PrStatus_InvalidData,
        "same name") == nullptr);
    CHECK(read("<materialx><image type=\"float\"/></materialx>", PrStatus_InvalidData, "no name") == nullptr);
    CHECK(read("<mtlx/>", PrStatus_InvalidData, "root") == nullptr);
    CHECK(read("<materialx><nodegraph name=\"a\"></materialx>", PrStatus_InvalidData) == nullptr);
    CHECK(read("", PrStatus_InvalidData) == nullptr);
    // Names may repeat under different parents.
    PrMaterialxDocument* document = read("<materialx><nodegraph name=\"a\"><image name=\"a\" type=\"float\"/></nodegraph>"
        "<nodegraph name=\"b\"><image name=\"a\" type=\"float\"/></nodegraph></materialx>");
    CHECK(document != nullptr);
    prMaterialxDocumentDestroy(document);
    prMaterialxDocumentDestroy(nullptr);
}

TEST_CASE("a value of floats is numbers between commas and whitespace") {
    float out[4] = {9, 9, 9, 9};
    CHECK(prMaterialxParseFloats("0.5", out, 1));
    CHECK(out[0] == 0.5f);
    CHECK(prMaterialxParseFloats("0.1, 0.2,0.3", out, 3));
    CHECK(out[2] == 0.3f);
    CHECK(prMaterialxParseFloats(" 1 2 ", out, 2));
    CHECK(out[1] == 2);
    CHECK(prMaterialxParseFloats("1e-2, -4", out, 2));
    CHECK(out[0] == 0.01f);
    CHECK(out[1] == -4);
    // Another number of numbers, or what is not a number, changes nothing.
    out[0] = 7;
    CHECK(!prMaterialxParseFloats("1, 2", out, 3));
    CHECK(!prMaterialxParseFloats("1, 2, 3, 4", out, 3));
    CHECK(!prMaterialxParseFloats("1, x", out, 2));
    CHECK(!prMaterialxParseFloats("1x", out, 1));
    CHECK(out[0] == 7);
}
