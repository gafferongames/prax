#include "mermaid/src/mermaid_er.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed ER diagram, freed when it goes out of scope.
struct Parsed {
    MermaidErDiagram diagram{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidErParse(diagram, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidErReset(diagram);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidErText(diagram, span);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid er: relationships give each end's cardinality, their line, and their label") {
    Parsed parsed("erDiagram\n"
        "CUSTOMER ||--o{ ORDER : places\nORDER |o..|{ LINE-ITEM : \"is in\"\nA }|--o| B : x\nC }o--|| D : y\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.entities.count == 7);
    CHECK(parsed.text(parsed.diagram.entities.data[2].id) == "LINE-ITEM");
    REQUIRE(parsed.diagram.relations.count == 4);
    const MermaidErRelation* r = parsed.diagram.relations.data;
    CHECK(r[0].sourceCardinality == MermaidCardinality_ExactlyOne);
    CHECK(r[0].targetCardinality == MermaidCardinality_ZeroOrMore);
    CHECK(r[0].identifying);
    CHECK(parsed.text(r[0].label) == "places");
    CHECK(r[1].sourceCardinality == MermaidCardinality_ZeroOrOne);
    CHECK(r[1].targetCardinality == MermaidCardinality_OneOrMore);
    CHECK_FALSE(r[1].identifying);
    CHECK(parsed.text(r[1].label) == "is in");
    CHECK(r[2].sourceCardinality == MermaidCardinality_OneOrMore);
    CHECK(r[2].targetCardinality == MermaidCardinality_ZeroOrOne);
    CHECK(r[3].sourceCardinality == MermaidCardinality_ZeroOrMore);
    CHECK(r[3].targetCardinality == MermaidCardinality_ExactlyOne);
}

TEST_CASE("mermaid er: an entity's block lists its attributes, with their keys and comments") {
    Parsed parsed("erDiagram\n"
        "CUSTOMER[\"Customer\"] {\n    string name\n    string id PK, FK \"unique id\"\n}\nEMPTY { }\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.diagram.entities.data[0].label) == "Customer");
    REQUIRE(parsed.diagram.attributes.count == 2);
    const MermaidAttribute& id = parsed.diagram.attributes.data[1];
    CHECK(parsed.text(id.type) == "string");
    CHECK(parsed.text(id.name) == "id");
    CHECK(parsed.text(id.keys) == "PK, FK");
    CHECK(parsed.text(id.comment) == "unique id");
    CHECK(parsed.diagram.entities.count == 2);
}

TEST_CASE("mermaid er: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"erDiagram\nA {\nstring name\n", 2},
        {"erDiagram\nA ||==o{ B : x\n", 2},
        {"erDiagram\nA ||--o{ B x\n", 2},
        {"erDiagram\nA {\nonlytype\n}\n", 3},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid er: keywords ignore case, and class and style statements apply to named entities") {
    Parsed parsed("erDiagram\nCLASSDEF hot fill:#f00\nA |O--O{ B : x\nCLASS A hot\nSTYLE B fill:#0f0\n"
        "Direction LR\nstyle X fill:#00f\nclass Y hot\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.diagram.direction == PrGraphDirection_Right);
    // As in mermaid.js, a class or style statement names no new entity.
    REQUIRE(parsed.diagram.entities.count == 2);
    CHECK(parsed.diagram.entities.data[0].style.fill.color.r == 0xff);
    CHECK(parsed.diagram.entities.data[1].style.fill.color.g == 0xff);
    CHECK(parsed.diagram.relations.data[0].sourceCardinality == MermaidCardinality_ZeroOrOne);
    CHECK(parsed.diagram.relations.data[0].targetCardinality == MermaidCardinality_ZeroOrMore);
}

TEST_CASE("mermaid er: each relationship finds its entities by name among many") {
    std::string source = "erDiagram\n";
    for (int i = 0; i < 20000; ++i)
        source += "E" + std::to_string(i) + " ||--o{ E" + std::to_string(i + 1) + " : x\n";
    Parsed parsed(source.c_str());
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.diagram.entities.count == 20001);
    CHECK(parsed.diagram.relations.data[19999].target == 20000);
}
