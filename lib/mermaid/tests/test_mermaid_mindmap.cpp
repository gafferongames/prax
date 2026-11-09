#include "mermaid/src/mermaid_mindmap.h"

#include "tools/harness/test.h"

#include <chrono>
#include <string>

namespace pr_impl {

namespace {

// A parsed mindmap, freed when it goes out of scope.
struct Parsed {
    MermaidMindmap mindmap{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidMindmapParse(mindmap, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidMindmapReset(mindmap);
    }

    std::string text(uint32_t node) const {
        PrStringView view = mermaidMindmapText(mindmap, mindmap.nodes.data[node].label);
        return std::string(view.data, view.size);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid mindmap: indentation makes the tree, and brackets give the shapes") {
    Parsed parsed("mindmap\n"
        "  root((Center))\n"
        "    A\n"
        "      a1[Square]\n"
        "      ::icon(fa fa-book)\n"
        "      a2(Rounded)\n"
        "    B\n"
        "      b1{{Hexagon}}\n"
        "        deep))Bang((\n"
        "      b2)Cloud(\n"
        "      \"`**Bold** text`\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.mindmap.nodes.count == 9);
    const MermaidMindmapNode* n = parsed.mindmap.nodes.data;
    CHECK(n[0].shape == MermaidMindmapShape_Circle);
    CHECK(parsed.text(0) == "Center");
    CHECK(n[1].parent == 0);
    CHECK(n[1].section == 0);
    CHECK(n[2].shape == MermaidMindmapShape_Square);
    CHECK(n[2].parent == 1);
    CHECK(n[3].shape == MermaidMindmapShape_Rounded);
    CHECK(n[4].section == 1);
    CHECK(n[5].shape == MermaidMindmapShape_Hexagon);
    CHECK(n[5].section == 1);
    CHECK(n[6].shape == MermaidMindmapShape_Bang);
    CHECK(n[6].depth == 3);
    CHECK(n[7].shape == MermaidMindmapShape_Cloud);
    CHECK(n[7].parent == 4);
    CHECK(n[8].format == MermaidTextFormat_Markdown);
    CHECK(parsed.text(8) == "**Bold** text");
}

TEST_CASE("mermaid mindmap: a second root fails") {
    Parsed parsed("mindmap\n  root\n    child\n  other\n");
    CHECK(parsed.status == PrStatus_Syntax);
    CHECK(parsed.error.line == 4);
}

TEST_CASE("mermaid mindmap: each of the root's children starts the next section, however many there are") {
    constexpr uint32_t kChildren = 20000;
    std::string source = "mindmap\nroot\n";
    for (uint32_t i = 0; i < kChildren; ++i)
        source += "  child\n    grandchild\n";
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    Parsed parsed(source.c_str());
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(seconds < 5);
    REQUIRE(parsed.mindmap.nodes.count == 2 * kChildren + 1);
    const MermaidMindmapNode& last = parsed.mindmap.nodes.data[2 * kChildren - 1];
    CHECK(last.parent == 0);
    CHECK(last.section == kChildren - 1);
    CHECK(parsed.mindmap.nodes.data[2 * kChildren].section == kChildren - 1);
}

TEST_CASE("mermaid mindmap: comments, directives and accessibility lines are not nodes") {
    Parsed parsed("---\ntitle: \"Front\"\n---\nmindmap\n  root\n  %% a comment\n  accDescr {\n    a\n  }\n"
        "    child\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    PrStringView title = mermaidMindmapText(parsed.mindmap, parsed.mindmap.title);
    CHECK(std::string(title.data, title.size) == "Front");
    REQUIRE(parsed.mindmap.nodes.count == 2);
    CHECK(parsed.mindmap.nodes.data[1].parent == 0);
}
