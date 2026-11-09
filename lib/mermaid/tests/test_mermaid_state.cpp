#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/mermaid.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_state.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed state diagram, freed when it goes out of scope.
struct Parsed {
    MermaidStateDiagram diagram{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidStateParse(diagram, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidStateReset(diagram);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidStateText(diagram, span);
        return std::string(view.data, view.size);
    }

    uint32_t find(const char* id) const {
        for (uint32_t n = 0; n < diagram.nodes.count; ++n) {
            if (text(diagram.nodes.data[n].id) == id)
                return n;
        }
        return kMermaidNone;
    }
};

// A source of `depth` composite states, each inside the one before, with a transition at the bottom.
std::string nestedStates(int depth) {
    std::string source = "stateDiagram-v2\n";
    for (int i = 0; i < depth; ++i)
        source += "state S" + std::to_string(i) + " {\n";
    source += "[*] --> X\n";
    for (int i = 0; i < depth; ++i)
        source += "}\n";
    return source;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid state: transitions join states, and [*] is the start or the end of its scope") {
    Parsed parsed("stateDiagram-v2\n"
        "[*] --> Still\nStill --> Moving : push\nMoving --> [*]\n"
        "state \"A long name\" as Long\nLong : first\nLong : second\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidStateNode* nodes = parsed.diagram.nodes.data;
    REQUIRE(parsed.diagram.transitions.count == 3);
    const MermaidTransition* t = parsed.diagram.transitions.data;
    CHECK(nodes[t[0].source].kind == MermaidStateKind_Start);
    CHECK(nodes[t[2].target].kind == MermaidStateKind_End);
    CHECK(parsed.text(t[1].label) == "push");
    uint32_t longState = parsed.find("Long");
    REQUIRE(longState != kMermaidNone);
    CHECK(parsed.text(nodes[longState].label) == "A long name");
    CHECK(parsed.text(nodes[longState].description) == "first<br>second");
}

TEST_CASE("mermaid state: composite states hold their states, and -- divides them into regions") {
    Parsed parsed("stateDiagram-v2\n"
        "state Active {\n    direction LR\n    [*] --> Idle\n}\n"
        "state Parallel {\n    [*] --> A\n    --\n    [*] --> B\n}\n"
        "state fork_state <<fork>>\nstate join_state <<join>>\nstate check <<choice>>\n"
        "note right of Idle : Waiting\n"
        "note left of A\n    two\n    lines\nend note\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidStateNode* nodes = parsed.diagram.nodes.data;
    uint32_t active = parsed.find("Active");
    uint32_t idle = parsed.find("Idle");
    CHECK(nodes[idle].parent == active);
    CHECK(nodes[active].direction == PrGraphDirection_Right);
    // Each region has its own start, and the regions belong to the composite state.
    uint32_t parallel = parsed.find("Parallel");
    uint32_t a = parsed.find("A");
    uint32_t b = parsed.find("B");
    CHECK(nodes[nodes[a].parent].kind == MermaidStateKind_Region);
    CHECK(nodes[nodes[b].parent].kind == MermaidStateKind_Region);
    CHECK(nodes[a].parent != nodes[b].parent);
    CHECK(nodes[nodes[a].parent].parent == parallel);
    CHECK(nodes[parsed.find("fork_state")].kind == MermaidStateKind_Fork);
    CHECK(nodes[parsed.find("join_state")].kind == MermaidStateKind_Join);
    CHECK(nodes[parsed.find("check")].kind == MermaidStateKind_Choice);
    std::vector<std::string> notes;
    for (const MermaidStateNode& node : parsed.diagram.nodes.span()) {
        if (node.kind == MermaidStateKind_Note)
            notes.push_back(parsed.text(node.label));
    }
    CHECK(notes == std::vector<std::string>{"Waiting", "two<br>lines"});
}

TEST_CASE("mermaid state: styles come from classDef, class, ::: and style") {
    Parsed parsed("stateDiagram-v2\nclassDef hot fill:#f00\nA:::hot --> B\nclass B hot\nstyle B stroke:#00f\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidStateNode* nodes = parsed.diagram.nodes.data;
    CHECK(nodes[parsed.find("A")].style.fill.color.r == 255);
    CHECK(nodes[parsed.find("B")].style.fill.color.r == 255);
    CHECK(nodes[parsed.find("B")].style.stroke.color.b == 255);
}

TEST_CASE("mermaid state: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"stateDiagram-v2\nstate A {\n[*] --> B\n", 2},
        {"stateDiagram-v2\nnote right of A\ntext\n", 2},
        {"stateDiagram-v2\n}\n", 2},
        {"stateDiagram-v2\n--\n", 2},
        {"stateDiagram-v2\nthis is not a statement\n", 2},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid state: keywords ignore case, as in mermaid.js") {
    Parsed parsed("stateDiagram-v2\nSTATE A {\n[*] --> B\n}\nState F <<FORK>>\nNOTE Right Of B : n\n"
        "CLASSDEF hot fill:#f00\nCLASS B hot\nStyle B stroke:#00f\nnote left of A\nx\nEND NOTE\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidStateNode* nodes = parsed.diagram.nodes.data;
    uint32_t b = parsed.find("B");
    CHECK(nodes[b].parent == parsed.find("A"));
    CHECK(nodes[parsed.find("F")].kind == MermaidStateKind_Fork);
    CHECK(nodes[b].style.fill.color.r == 0xff);
    CHECK(nodes[b].style.stroke.color.b == 0xff);
}

TEST_CASE("mermaid state: a composite state that opens again continues in its last region") {
    Parsed parsed("stateDiagram-v2\nstate A {\nB\n--\nC\n}\nstate A {\nD\n[*] --> E\n}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidStateNode* nodes = parsed.diagram.nodes.data;
    uint32_t a = parsed.find("A");
    uint32_t regions = 0;
    for (const MermaidStateNode& node : parsed.diagram.nodes.span())
        regions += node.kind == MermaidStateKind_Region ? 1 : 0;
    CHECK(regions == 2);
    uint32_t first = nodes[parsed.find("B")].parent;
    uint32_t last = nodes[parsed.find("C")].parent;
    CHECK(nodes[first].kind == MermaidStateKind_Region);
    CHECK(nodes[first].parent == a);
    CHECK(first != last);
    CHECK(nodes[parsed.find("D")].parent == last);
    CHECK(nodes[parsed.find("E")].parent == last);
}

TEST_CASE("mermaid state: composite states nest at most 64 deep") {
    CHECK(Parsed(nestedStates(64).c_str()).status == PrStatus_Ok);
    Parsed tooDeep(nestedStates(65).c_str());
    CHECK(tooDeep.status == PrStatus_Syntax);
    CHECK(tooDeep.error.line == 66);
    CHECK(tooDeep.error.message == "a composite state nests too deeply");
    // The deepest that parses draws.
    MermaidDrawn drawn(nestedStates(64));
    REQUIRE(drawn.status == PrStatus_Ok);
    CHECK(drawn.count(PrMermaidItemKind_Group) == 64);
}

TEST_CASE("mermaid state: a transition's item has no ID") {
    MermaidDrawn drawn("stateDiagram-v2\nA --> B\n");
    REQUIRE(drawn.status == PrStatus_Ok);
    for (uint32_t i = 0; i < prMermaidItemCount(drawn.diagram); ++i) {
        PrMermaidItem item = prMermaidItem(drawn.diagram, i);
        if (item.kind == PrMermaidItemKind_Edge)
            CHECK(item.id.size == 0);
    }
    CHECK(drawn.count(PrMermaidItemKind_Edge) == 1);
}

TEST_CASE("mermaid state: many states, descriptions and pseudostates read in linear time") {
    std::string source = "stateDiagram-v2\n";
    for (int i = 0; i < 10000; ++i) {
        std::string name = "S" + std::to_string(i);
        source += "[*] --> " + name + "\n" + name + " --> [*]\nMany : line\n";
    }
    Parsed parsed(source.c_str());
    REQUIRE(parsed.status == PrStatus_Ok);
    // One start and one end serve the diagram's scope.
    CHECK(parsed.diagram.nodes.count == 10003);
    std::string description = parsed.text(parsed.diagram.nodes.data[parsed.find("Many")].description);
    CHECK(description.size() == 10000 * 4 + 9999 * 4);
}

TEST_CASE("mermaid state: a click statement gives a state made before it a link, unless the link runs script") {
    Parsed parsed("stateDiagram-v2\n"
        "A --> B\n"
        "B --> C\n"
        "click A \"https://example.com/a\" \"A tooltip\"\n"
        "click B href \"docs/b.html\"\n"
        "click C \"javascript:alert(1)\"\n"
        "click D \"https://example.com/d\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.diagram.nodes.data[parsed.find("A")].link) == "https://example.com/a");
    CHECK(parsed.text(parsed.diagram.nodes.data[parsed.find("B")].link) == "docs/b.html");
    CHECK(parsed.diagram.nodes.data[parsed.find("C")].link.size == 0);
    CHECK(parsed.find("D") == kMermaidNone);
    Parsed unclosed("stateDiagram-v2\nA --> B\nclick A \"https://example.com\n");
    CHECK(unclosed.status == PrStatus_Syntax);
}
