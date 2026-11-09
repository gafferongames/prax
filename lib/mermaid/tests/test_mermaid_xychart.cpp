#include "mermaid/src/mermaid_xychart.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed XY chart, freed when it goes out of scope.
struct Parsed {
    MermaidXyChart chart{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidXyChartParse(chart, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidXyChartReset(chart);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidXyChartText(chart, span);
        return std::string(view.data, view.size);
    }

    double value(uint32_t series, uint32_t index) const {
        return chart.values.data[chart.series.data[series].valueStart + index];
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid xychart: a band x-axis has categories, and series have values in source order") {
    Parsed parsed("xychart-beta horizontal\n  title \"Sales\"\n  x-axis Months [jan, \"feb x\", mar]\n"
        "  y-axis \"Revenue\" 0 --> 100\n  bar [1, 2.5, -3]\n  line Trend [4, 5, 6]\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.chart.horizontal);
    CHECK(parsed.text(parsed.chart.title) == "Sales");
    CHECK(parsed.chart.x.band);
    CHECK(parsed.text(parsed.chart.x.title) == "Months");
    REQUIRE(parsed.chart.x.categoryCount == 3);
    CHECK(parsed.text(parsed.chart.categories.data[1]) == "feb x");
    CHECK(parsed.text(parsed.chart.y.title) == "Revenue");
    CHECK(parsed.chart.y.min == 0);
    CHECK(parsed.chart.y.max == 100);
    REQUIRE(parsed.chart.series.count == 2);
    CHECK_FALSE(parsed.chart.series.data[0].line);
    CHECK(parsed.value(0, 1) == 2.5);
    CHECK(parsed.value(0, 2) == -3);
    CHECK(parsed.chart.series.data[1].line);
    CHECK(parsed.text(parsed.chart.series.data[1].title) == "Trend");
}

TEST_CASE("mermaid xychart: an axis without a range of its own spans the data") {
    Parsed linear("xychart\nx-axis \"Year\" 2000 --> 2010\ny-axis Count\nbar [3, 9, 1]\nline [7, 2]\n");
    REQUIRE(linear.status == PrStatus_Ok);
    CHECK_FALSE(linear.chart.horizontal);
    CHECK_FALSE(linear.chart.x.band);
    CHECK(linear.chart.x.min == 2000);
    CHECK(linear.chart.x.max == 2010);
    CHECK(linear.text(linear.chart.y.title) == "Count");
    CHECK(linear.chart.y.min == 1);
    CHECK(linear.chart.y.max == 9);

    // Without an x-axis, the values are numbered from 1. A band axis places only as many values as it has categories.
    Parsed bare("xychart vertical\nline [5, 6, 7, 8]\n");
    REQUIRE(bare.status == PrStatus_Ok);
    CHECK(bare.chart.x.min == 1);
    CHECK(bare.chart.x.max == 4);
    Parsed band("xychart\nx-axis [a, b]\nbar [1, 2, 30]\n");
    CHECK(band.chart.y.max == 2);
}

TEST_CASE("mermaid xychart: a source that does not parse names where and why") {
    const char* cases[] = {
        "xychart\nbar [1, x]\n",
        "xychart\nbar 1, 2\n",
        "xychart\ny-axis [a, b]\n",
        "xychart\nx-axis \"T 1 --> 2\n",
        "xychart\ny-axis T 1 --> x\n",
        "xychart\npie [1]\n",
    };
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
    }
    CHECK(Parsed("xychart sideways\nbar [1]\n").status == PrStatus_Syntax);
    CHECK(Parsed("xychart\ntitle Empty\n").status == PrStatus_Syntax);
}

TEST_CASE("mermaid xychart: the title may come from the front matter, and accessibility lines are ignored") {
    Parsed parsed("---\ntitle: \"Front\"\n---\nxychart-beta\naccTitle: A title\naccDescr {\n  bar [x]\n}\n"
        "%% bar [y]\nbar [1, 2]\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.chart.title) == "Front");
    CHECK(parsed.chart.series.count == 1);
    Parsed empty("xychart-beta\n\n%% nothing\n");
    CHECK(empty.status == PrStatus_Syntax);
    CHECK(empty.error.line == 4);
}
