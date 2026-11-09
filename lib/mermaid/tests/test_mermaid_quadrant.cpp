#include "mermaid/src/mermaid_quadrant.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed quadrant chart, freed when it goes out of scope.
struct Parsed {
    MermaidQuadrantChart chart{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidQuadrantParse(chart, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidQuadrantReset(chart);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidQuadrantText(chart, span);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid quadrant: a chart has a title, axes and the labels of its quadrants") {
    Parsed parsed("quadrantChart\ntitle Reach and engagement\nx-axis Low Reach --> High Reach\n"
        "Y-AXIS \"Low\" ---> \"High\"\nquadrant-1 Expand\nquadrant-4 \"`Improve`\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.chart.title) == "Reach and engagement");
    CHECK(parsed.text(parsed.chart.xAxis[0]) == "Low Reach");
    CHECK(parsed.text(parsed.chart.xAxis[1]) == "High Reach");
    CHECK(parsed.text(parsed.chart.yAxis[0]) == "Low");
    CHECK(parsed.text(parsed.chart.yAxis[1]) == "High");
    CHECK(parsed.text(parsed.chart.quadrants[0]) == "Expand");
    CHECK(parsed.text(parsed.chart.quadrants[1]).empty());
    CHECK(parsed.text(parsed.chart.quadrants[3]) == "Improve");

    Parsed one("quadrantChart\nx-axis Only\n");
    CHECK(one.text(one.chart.xAxis[0]) == "Only");
    CHECK(one.text(one.chart.xAxis[1]).empty());
}

TEST_CASE("mermaid quadrant: points have coordinates, styles and classes, under their own styles") {
    Parsed parsed("quadrantChart\nCampaign A: [0.3, 0.6]\n"
        "Campaign F: [0.35, 0.78] color: #ff3300, radius: 10\n"
        "Campaign G:::class1: [1, 0] stroke-width: 2px\n"
        "classDef class1 color: #109060, radius: 9, stroke-color: #310085, stroke-width: 4px\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.chart.points.count == 3);
    const MermaidQuadrantPoint& a = parsed.chart.points.data[0];
    CHECK(parsed.text(a.label) == "Campaign A");
    CHECK(a.x == doctest::Approx(0.3));
    CHECK(a.y == doctest::Approx(0.6));
    CHECK(a.style.flags == 0);
    const MermaidQuadrantPoint& f = parsed.chart.points.data[1];
    CHECK(f.style.flags == (MermaidQuadrantStyleFlag_Color | MermaidQuadrantStyleFlag_Radius));
    CHECK(f.style.radius == 10);
    CHECK(f.style.color.r == 0xff);
    CHECK(f.style.color.g == 0x33);
    const MermaidQuadrantPoint& g = parsed.chart.points.data[2];
    CHECK(parsed.text(g.label) == "Campaign G");
    CHECK(g.x == 1);
    CHECK(g.style.radius == 9);
    CHECK(g.style.color.g == 0x90);
    CHECK(g.style.strokeColor.b == 0x85);
    CHECK(g.style.strokeWidth == 2);
}

TEST_CASE("mermaid quadrant: a source that does not parse names where and why") {
    const char* cases[] = {
        "quadrantChart\nA: [1.5, 0.5]\n",
        "quadrantChart\nA: [0.5]\n",
        "quadrantChart\nA [0.5, 0.5]\n",
        "quadrantChart\nA: [0.5, 0.5] radius: big\n",
        "quadrantChart\nA: [0.5, 0.5] color: red\n",
        "quadrantChart\nA: [0.5, 0.5] stroke-width: 3\n",
        "quadrantChart\nA: [0.5, 0.5] opacity: 1\n",
        "quadrantChart\njust words\n",
    };
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
    }
}

TEST_CASE("mermaid quadrant: a class's last definition counts, and a point names a class by its whole name") {
    Parsed parsed("quadrantChart\naccDescr {\n  P: [2, 2]\n}\nA:::big: [0.5, 0.5]\nB:::bi: [0.5, 0.5]\nC: [0.5, 0.5]\n"
        "classDef big radius: 3\nclassDef big radius: 20, color: #00ff00\nclassDef bi\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.chart.points.count == 3);
    const MermaidQuadrantPoint* p = parsed.chart.points.data;
    CHECK(p[0].style.flags == (MermaidQuadrantStyleFlag_Radius | MermaidQuadrantStyleFlag_Color));
    CHECK(p[0].style.radius == 20);
    CHECK(p[1].style.flags == 0);
    CHECK(p[2].style.flags == 0);
}
