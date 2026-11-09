#include "mermaid/src/mermaid_pie.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed pie chart, freed when it goes out of scope.
struct Parsed {
    MermaidPie pie{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidPieParse(pie, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidPieReset(pie);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidPieText(pie, span);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid pie: slices have labels and values, and the header may show data and a title") {
    Parsed parsed("pie showData title Pets\n    \"Dogs\" : 386\n    \"Cats\" : 85.5\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.pie.showData);
    CHECK(parsed.text(parsed.pie.title) == "Pets");
    REQUIRE(parsed.pie.slices.count == 2);
    CHECK(parsed.text(parsed.pie.slices.data[1].label) == "Cats");
    CHECK(parsed.text(parsed.pie.slices.data[1].valueText) == "85.5");
    CHECK(parsed.pie.slices.data[1].value == 85.5);

    Parsed statement("pie\ntitle Stated\n\"A\" : 1\n");
    CHECK(statement.text(statement.pie.title) == "Stated");
    CHECK_FALSE(statement.pie.showData);
}

TEST_CASE("mermaid pie: a source that does not parse names where and why") {
    const char* cases[] = {"pie\n\"A\" : -1\n", "pie\n\"A\" 1\n", "pie\nA : 1\n", "pie\n\"A : 1\n"};
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
    }
}

TEST_CASE("mermaid pie: the title may come from the front matter, and accessibility lines are ignored") {
    Parsed parsed("---\ntitle: 'Front'\n---\npie\naccTitle: A title\naccDescr {\n  \"A\" : 1\n}\n\"B\" : 2\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.pie.title) == "Front");
    REQUIRE(parsed.pie.slices.count == 1);
    CHECK(parsed.text(parsed.pie.slices.data[0].label) == "B");
}
