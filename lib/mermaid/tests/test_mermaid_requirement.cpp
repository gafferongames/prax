#include "mermaid/src/mermaid_requirement.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed requirement diagram, freed when it goes out of scope.
struct Parsed {
    MermaidRequirementDiagram diagram{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidRequirementParse(diagram, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidRequirementReset(diagram);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidRequirementText(diagram, span);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid requirement: requirements have an ID, a text, a risk and a verification method") {
    Parsed parsed("requirementDiagram\n"
        "requirement test_req {\nid: 1\ntext: the test text.\nrisk: High\nverifymethod: test\n}\n"
        "designConstraint \"quoted name\" {\n  id: \"1.2\"\n  text: \"a - b\"\n  verifyMethod: Analysis\n}\n"
        "functionalRequirement f {}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.blocks.count == 3);
    const MermaidRequirementBlock& first = parsed.diagram.blocks.data[0];
    CHECK(first.kind == MermaidRequirementKind_Requirement);
    CHECK(parsed.text(first.name) == "test_req");
    CHECK(parsed.text(first.id) == "1");
    CHECK(parsed.text(first.text) == "the test text.");
    CHECK(first.risk == MermaidRisk_High);
    CHECK(first.verifyMethod == MermaidVerifyMethod_Test);
    const MermaidRequirementBlock& second = parsed.diagram.blocks.data[1];
    CHECK(second.kind == MermaidRequirementKind_DesignConstraint);
    CHECK(parsed.text(second.name) == "quoted name");
    CHECK(parsed.text(second.id) == "1.2");
    CHECK(parsed.text(second.text) == "a - b");
    CHECK(second.risk == MermaidRisk_None);
    CHECK(second.verifyMethod == MermaidVerifyMethod_Analysis);
    CHECK(parsed.diagram.blocks.data[2].kind == MermaidRequirementKind_Functional);
}

TEST_CASE("mermaid requirement: elements have a type and a document reference") {
    Parsed parsed("requirementDiagram\nelement e {\ntype: word doc\ndocRef: reqs/test\n}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.blocks.count == 1);
    const MermaidRequirementBlock& block = parsed.diagram.blocks.data[0];
    CHECK(block.kind == MermaidRequirementKind_Element);
    CHECK(parsed.text(block.type) == "word doc");
    CHECK(parsed.text(block.docRef) == "reqs/test");
}

TEST_CASE("mermaid requirement: relations run from the tail of their arrow to its head") {
    Parsed parsed("requirementDiagram\ndirection LR\nrequirement r {\n}\nelement e {\n}\n"
        "e - satisfies -> r\nr <- copies - e\nr - contains -> e\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.diagram.direction == PrGraphDirection_Right);
    REQUIRE(parsed.diagram.links.count == 3);
    CHECK(parsed.diagram.links.data[0].source == 1);
    CHECK(parsed.diagram.links.data[0].target == 0);
    CHECK(parsed.diagram.links.data[0].relation == MermaidRequirementRelation_Satisfies);
    CHECK(parsed.diagram.links.data[1].source == 1);
    CHECK(parsed.diagram.links.data[1].target == 0);
    CHECK(parsed.diagram.links.data[1].relation == MermaidRequirementRelation_Copies);
    CHECK(parsed.diagram.links.data[2].source == 0);
    CHECK(parsed.diagram.links.data[2].relation == MermaidRequirementRelation_Contains);
    CHECK(mermaidRequirementRelationName(MermaidRequirementRelation_Traces) == "traces");
}

TEST_CASE("mermaid requirement: classDef, class, ::: and style set a block's style, in that order") {
    Parsed parsed("requirementDiagram\nrequirement a:::hot {\n}\nelement b {\n}\nelement c {\n}\n"
        "classDef hot fill:#f96,stroke-width:3px\nclassDef cold fill:#00f\nclass b cold\nc:::hot\n"
        "style a fill:#0f0\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidPaint& a = parsed.diagram.blocks.data[0].style;
    CHECK((a.flags & MermaidPaintFlag_StrokeWidth) != 0);
    CHECK(a.strokeWidth == 3);
    CHECK(a.fill.color.g == 0xff);
    CHECK(parsed.diagram.blocks.data[1].style.fill.color.b == 0xff);
    CHECK(parsed.diagram.blocks.data[2].style.fill.color.r == 0xff);
}

TEST_CASE("mermaid requirement: the first block of a name is the one that counts") {
    Parsed parsed("requirementDiagram\nrequirement a {\nid: 1\n}\nrequirement a {\nid: 2\n}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.blocks.count == 1);
    CHECK(parsed.text(parsed.diagram.blocks.data[0].id) == "1");
}

TEST_CASE("mermaid requirement: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"requirementDiagram\nrequirement a {\nrisk: extreme\n}\n", 3},
        {"requirementDiagram\nrequirement a {\ntype: x\n}\n", 3},
        {"requirementDiagram\nelement a {\nid: 1\n}\n", 3},
        {"requirementDiagram\nrequirement a {\nid: 1\n", 2},
        {"requirementDiagram\nrequirement a {\n}\na - likes -> a\n", 4},
        {"requirementDiagram\nrequirement a {\n}\na - traces - a\n", 4},
        {"requirementDiagram\nrequirement a {\n}\na - traces -> b\n", 4},
        {"requirementDiagram\nrequirement a\n", 2},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid requirement: class and style statements may name a block before the source defines it") {
    Parsed parsed("requirementDiagram\nclass a hot\nstyle a fill:#0f0\nclass missing hot\nclassDef hot stroke:#f00\n"
        "accDescr {\nrequirement hidden\n}\nrequirement a {\n}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.blocks.count == 1);
    const MermaidPaint& a = parsed.diagram.blocks.data[0].style;
    CHECK(a.fill.color.g == 0xff);
    CHECK(a.stroke.color.r == 0xff);
}

TEST_CASE("mermaid requirement: blocks and relations find each other by name among many") {
    std::string source = "requirementDiagram\n";
    for (int i = 0; i < 5000; ++i)
        source += "requirement r" + std::to_string(i) + " {\n}\n";
    for (int i = 0; i + 1 < 5000; ++i)
        source += "r" + std::to_string(i) + " - traces -> r" + std::to_string(i + 1) + "\n";
    for (int i = 0; i < 5000; ++i)
        source += "class r" + std::to_string(i) + " hot\n";
    Parsed parsed(source.c_str());
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.diagram.blocks.count == 5000);
    REQUIRE(parsed.diagram.links.count == 4999);
    CHECK(parsed.diagram.links.data[4998].target == 4999);
}
