#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/mermaid.h"
#include "pr/text.h"

#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestMermaidCApi(void);

using namespace pr_impl;

TEST_CASE("mermaid: the public header compiles as C") {
    CHECK(prTestMermaidCApi() == 0);
}

TEST_CASE("mermaid: a flowchart lays out around its labels, and draws") {
    MermaidTestFonts fonts;
    PrMermaid* small = nullptr;
    PrMermaid* large = nullptr;
    REQUIRE(fonts.create("graph TD\nA[Hi] --> B[Ok]", &small) == PrStatus_Ok);
    REQUIRE(fonts.create("graph TD\nA[A much longer label than before] --> B[Ok]", &large) == PrStatus_Ok);
    PrVec2 smallExtent = prMermaidExtent(small);
    PrVec2 largeExtent = prMermaidExtent(large);
    // A node's label is at least Mermaid's least node width, however short its text.
    CHECK(smallExtent.x >= 120);
    // The longer label wraps at the wrapping width, which is no wider than the least width, so its lines make its node
    // taller and no wider.
    CHECK(largeExtent.x == doctest::Approx(smallExtent.x));
    CHECK(largeExtent.y > smallExtent.y + 10);
    PrDrawList* list = prDrawListCreate();
    prMermaidDraw(large, list, PrVec2{10, 10}, PrColorScheme_Light);
    CHECK(prDrawListCommandCount(list) > 0);
    prDrawListRelease(list);
    prMermaidDestroy(small);
    prMermaidDestroy(large);
}

TEST_CASE("mermaid: the direction of a flowchart turns its extent") {
    MermaidTestFonts fonts;
    PrMermaid* down = nullptr;
    PrMermaid* right = nullptr;
    REQUIRE(fonts.create("graph TD\nA --> B --> C --> D", &down) == PrStatus_Ok);
    REQUIRE(fonts.create("graph LR\nA --> B --> C --> D", &right) == PrStatus_Ok);
    CHECK(prMermaidExtent(down).y > prMermaidExtent(down).x);
    CHECK(prMermaidExtent(right).x > prMermaidExtent(right).y);
    prMermaidDestroy(down);
    prMermaidDestroy(right);
}

TEST_CASE("mermaid: a subgraph keeps its direction until its nodes link outside it") {
    MermaidTestFonts fonts;
    PrMermaid* alone = nullptr;
    PrMermaid* linked = nullptr;
    const char* subgraph = "graph TD\nsubgraph s\ndirection LR\na --> b --> c\nend\n";
    REQUIRE(fonts.create(subgraph, &alone) == PrStatus_Ok);
    REQUIRE(fonts.create((std::string(subgraph) + "x --> a\n").c_str(), &linked) == PrStatus_Ok);
    // Alone, the subgraph runs left to right. Linked from above, it runs down with its parent.
    CHECK(prMermaidExtent(alone).x > prMermaidExtent(alone).y);
    CHECK(prMermaidExtent(linked).y > prMermaidExtent(linked).x);
    prMermaidDestroy(alone);
    prMermaidDestroy(linked);
}

TEST_CASE("mermaid: every shape, link and subgraph lays out and draws") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    const char* source = "flowchart TB\n"
        "    a[Rect] --> b(Round) -.-> c([Stadium]) ==> d[[Subroutine]] --o e[(Database)]\n"
        "    f((Circle)) --x g(((Double))) <--> h>Flag] -- label --> i{Decision} --> j{{Hexagon}}\n"
        "    subgraph s [Group]\n"
        "        direction LR\n"
        "        k[/Lean right/] --> l[\\Lean left\\] --> m[/Trapezoid\\] --> n[\\Inverted/]\n"
        "    end\n"
        "    a --> s\n"
        "    s --> j\n"
        "    i --> i\n"
        "    style a fill:#f9f,stroke:#333,stroke-width:4px\n";
    REQUIRE(fonts.create(source, &diagram) == PrStatus_Ok);
    uint32_t counts[4] = {};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i)
        ++counts[prMermaidItem(diagram, i).kind];
    CHECK(counts[PrMermaidItemKind_Node] == 14);
    CHECK(counts[PrMermaidItemKind_Group] == 1);
    CHECK(counts[PrMermaidItemKind_Edge] == 14);
    CHECK(counts[PrMermaidItemKind_Label] == 1);
    PrDrawList* list = prDrawListCreate();
    prMermaidDraw(diagram, list, PrVec2{0, 0}, PrColorScheme_Light);
    CHECK(prDrawListCommandCount(list) > 0);
    prDrawListRelease(list);
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a label's styled text takes room, and a long label wraps") {
    MermaidTestFonts fonts;
    // The width of the one node of `label`, a circle, which unlike other shapes is as wide as its text.
    auto widthOf = [&](const char* label) {
        std::string source = std::string("graph TD\nA((\"") + label + "\"))";
        PrMermaid* diagram = nullptr;
        REQUIRE(fonts.create(source.c_str(), &diagram) == PrStatus_Ok);
        REQUIRE(prMermaidItemCount(diagram) == 1);
        PrRect rect = prMermaidItem(diagram, 0).rect;
        prMermaidDestroy(diagram);
        return rect.right - rect.left;
    };
    // Tags and Markdown's marks style the text and take no room of their own. Code's text is drawn.
    float plain = widthOf("Order");
    CHECK(widthOf("<b>Order</b>") == doctest::Approx(plain).epsilon(0.1));
    CHECK(widthOf("<i>Order</i>") == doctest::Approx(plain).epsilon(0.1));
    CHECK(widthOf("`**Order**`") == doctest::Approx(plain).epsilon(0.1));
    CHECK(widthOf("`*Order*`") == doctest::Approx(plain).epsilon(0.1));
    CHECK(widthOf("Order <code>id</code>") > plain + 10);
    // A line longer than the wrapping width breaks, in HTML and in a Markdown string, so its node is narrower than the
    // line would make it.
    CHECK(widthOf("One two three four five six seven eight nine ten") < 170);
    CHECK(widthOf("`One two three four five six seven eight nine ten`") < 170);
}

TEST_CASE("mermaid: a diagram's items say where its nodes and edges are, by their names") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("graph TD\nA --> B", &diagram) == PrStatus_Ok);
    REQUIRE(prMermaidItemCount(diagram) == 3);
    PrMermaidItem edge = prMermaidItem(diagram, 0);
    PrMermaidItem a = prMermaidItem(diagram, 1);
    PrMermaidItem b = prMermaidItem(diagram, 2);
    CHECK(edge.kind == PrMermaidItemKind_Edge);
    CHECK(a.id == "A");
    CHECK(b.id == "B");
    // The edge runs from the bottom of A to the top of B.
    REQUIRE(edge.pointCount >= 2);
    CHECK(edge.points[0].y == doctest::Approx(a.rect.bottom).epsilon(0.01));
    CHECK(edge.points[edge.pointCount - 1].y <= b.rect.top + 0.5f);
    CHECK(b.rect.bottom <= prMermaidExtent(diagram).y);
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a source names its kind and theme, and its title and description for assistive technology") {
    PrMermaidKind kind = PrMermaidKind_Count;
    CHECK(prMermaidKindFromSource("---\ntitle: x\n---\n%% comment\nsequenceDiagram\n", &kind));
    CHECK(kind == PrMermaidKind_Sequence);
    CHECK(prMermaidKindName(kind) == "sequenceDiagram");
    CHECK_FALSE(prMermaidKindFromSource("not a diagram", &kind));

    MermaidTestFonts fonts;
    PrMermaid* light = nullptr;
    PrMermaid* dark = nullptr;
    PrMermaid* chosen = nullptr;
    REQUIRE(fonts.create("graph TD\naccTitle: Steps\naccDescr: A to B\nA --> B", &light) == PrStatus_Ok);
    REQUIRE(fonts.create("%%{init: {'theme': 'dark'}}%%\ngraph TD\nA --> B", &dark) == PrStatus_Ok);
    CHECK(prMermaidAccessibleTitle(light) == "Steps");
    CHECK(prMermaidAccessibleDescription(light) == "A to B");
    // A diagram draws in the scheme that it is asked for, unless its source names a theme.
    PrColorScheme named = PrColorScheme_Light;
    CHECK_FALSE(prMermaidScheme(light, &named));
    CHECK(prMermaidBackground(light, PrColorScheme_Light).r == 1.0f);
    CHECK(prMermaidBackground(light, PrColorScheme_Dark).r < 0.5f);
    REQUIRE(prMermaidScheme(dark, &named));
    CHECK(named == PrColorScheme_Dark);
    CHECK(prMermaidBackground(dark, PrColorScheme_Light).r < 0.5f);
    // A source's other themes are drawn in the light scheme.
    PrMermaidDesc desc{
        .source = "---\nconfig:\n  theme: forest\ntitle: Flow\n---\ngraph TD\nA --> B",
        .fonts = fonts.set,
        .shaper = fonts.shaper,
    };
    REQUIRE(prMermaidCreate(&desc, nullptr, &chosen, nullptr) == PrStatus_Ok);
    CHECK(prMermaidBackground(chosen, PrColorScheme_Dark).r == 1.0f);
    CHECK(prMermaidAccessibleTitle(chosen) == "Flow");
    prMermaidDestroy(light);
    prMermaidDestroy(dark);
    prMermaidDestroy(chosen);
}

TEST_CASE("mermaid: a source that does not parse makes no diagram, and says where") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    PrMermaidError error{};
    CHECK(fonts.create("graph TD\nA --> B\nA --\n", &diagram, &error) == PrStatus_Syntax);
    CHECK(diagram == nullptr);
    CHECK(error.line == 3);
    CHECK(error.message.size > 0);
    CHECK(fonts.create("kanban\n  Todo\n", &diagram, &error) == PrStatus_NotSupported);
    CHECK(diagram == nullptr);

    // The error diagram shows what failed, as mermaid.js shows a source that it cannot draw.
    REQUIRE(fonts.create("graph TD\nA --\n", &diagram, &error) == PrStatus_Syntax);
    PrMermaidDesc desc{.source = "graph TD\nA --\n", .fonts = fonts.set, .shaper = fonts.shaper};
    PrMermaid* shown = prMermaidCreateError(&desc, nullptr, &error);
    CHECK(prMermaidKind(shown) == PrMermaidKind_Error);
    CHECK(prMermaidExtent(shown).x > 500);
    PrDrawList* list = prDrawListCreate();
    prMermaidDraw(shown, list, PrVec2{0, 0}, PrColorScheme_Dark);
    CHECK(prDrawListCommandCount(list) > 0);
    prDrawListRelease(list);
    prMermaidDestroy(shown);
}

TEST_CASE("mermaid: a source at the edge of what parses draws a finite diagram, or fails, in bounded time") {
    MermaidTestFonts fonts;
    // Each draws: an activation that never ends, a style of a node named nowhere else, an empty region, an empty alias,
    // an empty block, a transition between a composite state and its own state, a week of excluded days, and axes whose
    // ranges doubles cannot count in ones.
    const char* drawn[] = {
        "sequenceDiagram\nA->>+B: hi\n",
        "classDiagram\nstyle A fill:#f00\n",
        "erDiagram\nstyle A fill:#f00\n",
        "stateDiagram-v2\nstyle A fill:#f00\n",
        "stateDiagram-v2\nstate A {\n--\n}\n",
        "erDiagram\nA[\"\"] {\n string x\n}\n",
        "sequenceDiagram\nA->>B: x\nloop\nend\nalt x\nelse y\nend\n",
        "stateDiagram-v2\nstate A {\n A --> B\n}\n",
        "stateDiagram-v2\nstate A {\n B\n}\nB --> A\n",
        "gantt\nexcludes monday,tuesday,wednesday,thursday,friday,saturday,sunday\na :2024-01-01, 1d\n",
        "xychart-beta\ny-axis 1152921504606846976 --> 1152921504606847232\nbar [1, 2]\n",
        "xychart-beta\ny-axis 1e-320 --> 2e-320\nbar [1, 2]\n",
        "xychart-beta\ny-axis -1e308 --> 1e308\nbar [1, 2]\n",
    };
    for (const char* source : drawn) {
        CAPTURE(source);
        PrMermaid* diagram = nullptr;
        REQUIRE(fonts.create(source, &diagram) == PrStatus_Ok);
        PrVec2 extent = prMermaidExtent(diagram);
        CHECK(prF32IsFinite(extent.x));
        CHECK(prF32IsFinite(extent.y));
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, PrColorScheme_Light);
        prDrawListRelease(list);
        prMermaidDestroy(diagram);
    }
    // Each fails: numbers past what a diagram computes with, and times past what a Date holds.
    const char* failed[] = {
        "pie\n\"a\" : 1e400\n\"b\" : 1\n",
        "journey\nsection s\nt: 1e300: Me\n",
        "graph TD\nA-->B\nlinkStyle 1e20 stroke:red\n",
        "gantt\nexcludes weekends\na :2024-01-01, 1000000000000000d\n",
        "gantt\ndateFormat X\na : 0, 1e29\n",
        "gantt\na :2024-01-01, 99999999999y\n",
    };
    for (const char* source : failed) {
        CAPTURE(source);
        PrMermaid* diagram = nullptr;
        CHECK(fonts.create(source, &diagram) == PrStatus_Syntax);
        CHECK(diagram == nullptr);
    }
}

TEST_CASE("mermaid: a sequence diagram puts its participants side by side, and its messages down the page") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("sequenceDiagram\nparticipant A\nactor B\nA->>B: one\nB-->>A: two\nA->>A: three\n",
        &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Sequence);
    std::vector<PrMermaidItem> nodes;
    std::vector<PrMermaidItem> edges;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node)
            nodes.push_back(item);
        else if (item.kind == PrMermaidItemKind_Edge)
            edges.push_back(item);
    }
    // Participants are mermaid.js's 150 by 65, 50 apart, inside a margin of 50 at the sides.
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].id == "A");
    CHECK(nodes[1].id == "B");
    CHECK(nodes[0].rect.left == doctest::Approx(50));
    CHECK(nodes[0].rect.right - nodes[0].rect.left == doctest::Approx(150));
    CHECK(nodes[0].rect.bottom - nodes[0].rect.top == doctest::Approx(65));
    CHECK(nodes[1].rect.left - nodes[0].rect.right == doctest::Approx(50));
    CHECK(prMermaidExtent(diagram).x == doctest::Approx(nodes[1].rect.right + 50));
    // Each message is below the one before, and below the participants.
    REQUIRE(edges.size() == 3);
    CHECK(edges[0].rect.top > nodes[0].rect.bottom);
    CHECK(edges[1].rect.top > edges[0].rect.top);
    CHECK(edges[2].rect.top > edges[1].rect.top);
    CHECK(prMermaidExtent(diagram).y > edges[2].rect.bottom + 65);
    // It draws in either scheme.
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a participant's type gives its shape, and a glyph's label below it deepens the bottom row") {
    MermaidTestFonts fonts;
    auto height = [&](const char* type) {
        std::string source = std::string("sequenceDiagram\nparticipant A@{ \"type\": \"") + type + "\" }\nA->>A: hi\n";
        PrMermaid* diagram = nullptr;
        REQUIRE(fonts.create(source.c_str(), &diagram) == PrStatus_Ok);
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, PrColorScheme_Light);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
        float y = prMermaidExtent(diagram).y;
        prMermaidDestroy(diagram);
        return y;
    };
    float participant = height("participant");
    // A box is 65 tall at the bottom. A glyph's circle is 44 across, and its label takes 20 below it, or 40 for a
    // control.
    CHECK(height("control") - participant == doctest::Approx(19));
    CHECK(height("boundary") - participant == doctest::Approx(-1));
    CHECK(height("entity") - participant == doctest::Approx(-1));
    CHECK(height("database") - participant == doctest::Approx(5));
    CHECK(height("collections") == doctest::Approx(participant));
    CHECK(height("queue") == doctest::Approx(participant));
}

TEST_CASE("mermaid: a created participant starts at its message, beside the participants before it") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("sequenceDiagram\nA->>B: x\ncreate participant C\nA->>C: make\ncreate actor D\nC->>D: y\n"
        "destroy C\nA-xC: gone\n", &diagram) == PrStatus_Ok);
    std::vector<PrMermaidItem> nodes;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node)
            nodes.push_back(item);
    }
    REQUIRE(nodes.size() == 4);
    // A created participant is half its width further from its neighbor, and its box is below the first row.
    CHECK(nodes[2].rect.left - nodes[1].rect.right == doctest::Approx(50 + 75));
    CHECK(nodes[3].rect.left - nodes[2].rect.right == doctest::Approx(50 + 75));
    CHECK(nodes[2].rect.top > nodes[0].rect.bottom);
    CHECK(nodes[3].rect.top > nodes[2].rect.bottom);
    CHECK(prMermaidExtent(diagram).x == doctest::Approx(nodes[3].rect.right + 50));
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a class's box has compartments for its name, attributes and methods, sized as Mermaid's") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("classDiagram\n"
        "class Animal {\n+String name\n+int age\n+makeSound() void\n}\n"
        "class Empty\nclass Duck {\n+swim()\n}\nclass Fields {\n-int count\n}\n"
        "Animal <|-- Duck\nnote for Duck \"Quacks\"\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Class);
    std::vector<PrMermaidItem> nodes;
    uint32_t edges = 0;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node)
            nodes.push_back(item);
        edges += item.kind == PrMermaidItemKind_Edge ? 1 : 0;
    }
    REQUIRE(nodes.size() == 4);
    // Lines are 24 high and the padding is 12, so the heights do not depend on the font.
    auto height = [](const PrMermaidItem& item) {
        return item.rect.bottom - item.rect.top;
    };
    CHECK(nodes[0].id == "Animal");
    CHECK(height(nodes[0]) == doctest::Approx(168));
    CHECK(height(nodes[1]) == doctest::Approx(84));
    CHECK(height(nodes[2]) == doctest::Approx(126));
    CHECK(height(nodes[3]) == doctest::Approx(120));
    CHECK(edges == 2);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a state diagram sizes its states as Mermaid's, and frames a composite state around its states") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("stateDiagram-v2\n[*] --> Still\nstate Active {\n[*] --> Idle\n}\nStill --> Active\n"
        "Still --> [*]\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_State);
    PrRect still{};
    PrRect idle{};
    PrRect active{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "Still")
            still = item.rect;
        else if (item.id == "Idle")
            idle = item.rect;
        else if (item.id == "Active" && item.kind == PrMermaidItemKind_Group)
            active = item.rect;
    }
    // A state is its name, at least 120 wide, with 8 around it. Lines are 24 high.
    CHECK(still.bottom - still.top == doctest::Approx(40));
    CHECK(still.right - still.left >= 136);
    CHECK(active.left < idle.left);
    CHECK(active.top < idle.top);
    CHECK(active.right > idle.right);
    CHECK(active.bottom > idle.bottom);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: an ER diagram's entity is a table of its attributes, or a box around its name") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("erDiagram\nCUSTOMER ||--o{ ORDER : places\n"
        "CUSTOMER {\nstring name\nstring id PK \"unique\"\nstring sector\n}\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Er);
    PrRect customer{};
    PrRect order{};
    uint32_t edges = 0;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "CUSTOMER" && item.kind == PrMermaidItemKind_Node)
            customer = item.rect;
        else if (item.id == "ORDER" && item.kind == PrMermaidItemKind_Node)
            order = item.rect;
        edges += item.kind == PrMermaidItemKind_Edge ? 1 : 0;
    }
    // A header and three rows, each a 24 high line with 9.375 above and below it.
    CHECK(customer.bottom - customer.top == doctest::Approx(4 * 42.75));
    // A box is its name with 30 above and below it.
    CHECK(order.bottom - order.top == doctest::Approx(84));
    CHECK(order.top > customer.bottom);
    CHECK(edges == 1);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a gantt chart gives each task a row, and a bar as long as its time") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("gantt\ndateFormat YYYY-MM-DD\nsection S\nA :a, 2024-01-01, 10d\nB :b, after a, 20d\n"
        "M :milestone, m, 2024-01-31, 0d\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Gantt);
    std::vector<PrMermaidItem> bars;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node)
            bars.push_back(item);
    }
    REQUIRE(bars.size() == 3);
    // Three rows of 24, with 50 above and below them.
    CHECK(prMermaidExtent(diagram).y == doctest::Approx(3 * 24 + 100));
    CHECK(bars[1].rect.top - bars[0].rect.top == doctest::Approx(24));
    CHECK(bars[1].rect.left == doctest::Approx(bars[0].rect.right));
    float a = bars[0].rect.right - bars[0].rect.left;
    float b = bars[1].rect.right - bars[1].rect.left;
    CHECK(b == doctest::Approx(2 * a));
    // The chart spans 75 to its width less 75, from the first start to the last end.
    CHECK(bars[0].rect.left == doctest::Approx(75));
    CHECK(bars[1].rect.right == doctest::Approx(prMermaidExtent(diagram).x - 75));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a pie chart is 450 high, with its legend to the right") {
    MermaidTestFonts fonts;
    PrMermaid* small = nullptr;
    PrMermaid* wide = nullptr;
    REQUIRE(fonts.create("pie\n\"A\" : 1\n\"B\" : 3\n", &small) == PrStatus_Ok);
    REQUIRE(fonts.create("pie showData\n\"A label much longer than the others\" : 1\n\"B\" : 3\n", &wide) ==
        PrStatus_Ok);
    CHECK(prMermaidKind(small) == PrMermaidKind_Pie);
    CHECK(prMermaidExtent(small).y == doctest::Approx(450));
    CHECK(prMermaidExtent(small).x > 450);
    CHECK(prMermaidExtent(wide).x > prMermaidExtent(small).x + 100);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(small, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(small);
    prMermaidDestroy(wide);
}

TEST_CASE("mermaid: a git graph gives each branch a lane, and each commit a column") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("gitGraph\ncommit id: \"A\"\nbranch dev\ncommit id: \"B\"\ncheckout main\nmerge dev id: \"C\"\n",
        &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_GitGraph);
    PrRect a{};
    PrRect b{};
    PrRect c{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "A")
            a = item.rect;
        else if (item.id == "B")
            b = item.rect;
        else if (item.id == "C")
            c = item.rect;
    }
    // Commits are 50 apart along the lanes, and lanes are 90 apart.
    CHECK(b.left - a.left == doctest::Approx(50));
    CHECK(c.left - b.left == doctest::Approx(50));
    CHECK(b.top - a.top == doctest::Approx(90));
    CHECK(c.top == doctest::Approx(a.top));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a mindmap lays its tree out down the page, with each node below its parent") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("mindmap\n  root((Root))\n    A\n      A1\n    B[Square]\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Mindmap);
    PrRect root{};
    PrRect a{};
    PrRect a1{};
    PrRect b{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "Root")
            root = item.rect;
        else if (item.id == "A")
            a = item.rect;
        else if (item.id == "A1")
            a1 = item.rect;
        else if (item.id == "Square")
            b = item.rect;
    }
    CHECK(a.top > root.bottom);
    CHECK(a1.top > a.bottom);
    CHECK(b.top == doctest::Approx(a.top - (b.bottom - b.top - (a.bottom - a.top)) * 0.5f));
    // A circle is as tall as it is wide, and a plain node is its text with 5 above and below.
    CHECK(root.bottom - root.top == doctest::Approx(root.right - root.left));
    CHECK(a.bottom - a.top == doctest::Approx(34));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a long message between neighbors moves them apart, and a block frames its messages") {
    MermaidTestFonts fonts;
    auto gap = [&](const char* source) {
        PrMermaid* diagram = nullptr;
        REQUIRE(fonts.create(source, &diagram) == PrStatus_Ok);
        float left = prMermaidItem(diagram, 0).rect.right;
        float right = prMermaidItem(diagram, 1).rect.left;
        prMermaidDestroy(diagram);
        return right - left;
    };
    float shortGap = gap("sequenceDiagram\nparticipant A\nparticipant B\nA->>B: hi\n");
    float longGap = gap("sequenceDiagram\nparticipant A\nparticipant B\n"
        "A->>B: a message much longer than the space between the two participants\n");
    CHECK(shortGap == doctest::Approx(50));
    CHECK(longGap > shortGap + 100);
    // A wrapped message wraps in the space it has, and does not move its participants apart.
    float wrappedGap = gap("sequenceDiagram\nparticipant A\nparticipant B\n"
        "A->>B: wrap:a message much longer than the space between the two participants\n");
    CHECK(wrappedGap == doctest::Approx(shortGap));

    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("sequenceDiagram\nA->>B: before\nloop forever\nA->>B: inside\nend\n", &diagram) ==
        PrStatus_Ok);
    PrRect frame{};
    PrRect inside{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Group)
            frame = item.rect;
        if (item.kind == PrMermaidItemKind_Edge && item.id == "inside")
            inside = item.rect;
    }
    CHECK(frame.top < inside.top);
    CHECK(frame.bottom > inside.bottom);
    CHECK(frame.left < inside.left);
    CHECK(frame.right > inside.right);
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: an XY chart is 700 by 500, and its bars stand on the x-axis, as tall as their values") {
    MermaidTestFonts fonts;
    auto bars = [&](const char* source, PrMermaid** diagram) {
        REQUIRE(fonts.create(source, diagram) == PrStatus_Ok);
        std::vector<PrRect> rects;
        for (uint32_t i = 0; i < prMermaidItemCount(*diagram); ++i)
            rects.push_back(prMermaidItem(*diagram, i).rect);
        return rects;
    };
    PrMermaid* vertical = nullptr;
    std::vector<PrRect> up = bars("xychart\nx-axis [a, b, c]\ny-axis 0 --> 100\nbar [0, 50, 100]\nline [1, 2, 3]\n",
        &vertical);
    CHECK(prMermaidKind(vertical) == PrMermaidKind_XyChart);
    CHECK(prMermaidExtent(vertical).x == doctest::Approx(700));
    CHECK(prMermaidExtent(vertical).y == doctest::Approx(500));
    REQUIRE(up.size() == 3);
    CHECK(up[0].bottom == doctest::Approx(up[2].bottom));
    CHECK(up[0].right - up[0].left == doctest::Approx(up[2].right - up[2].left));
    CHECK(up[1].left - up[0].left == doctest::Approx(up[2].left - up[1].left));
    // The bars' tops step evenly with their values.
    CHECK(up[0].top - up[1].top == doctest::Approx(up[1].top - up[2].top));
    CHECK(up[0].top > up[1].top);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(vertical, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }

    // A horizontal chart's bars start at the left of the plot and grow to the right.
    PrMermaid* horizontal = nullptr;
    std::vector<PrRect> across = bars("xychart horizontal\nx-axis [a, b]\nbar [10, 20]\n", &horizontal);
    REQUIRE(across.size() == 2);
    CHECK(across[0].left == doctest::Approx(across[1].left));
    CHECK(across[1].top > across[0].bottom);
    CHECK(across[1].right > across[0].right);
    CHECK(prMermaidExtent(horizontal).x == doctest::Approx(700));
    prMermaidDestroy(vertical);
    prMermaidDestroy(horizontal);
}

TEST_CASE("mermaid: a user journey puts its tasks in a row under their sections, 200 apart") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("journey\ntitle Day\nsection Morning\nTea: 5: Me\nWork: 1: Me, Cat\nsection Evening\n"
        "Rest: 3: Cat\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Journey);
    PrRect tea{};
    PrRect work{};
    PrRect rest{};
    PrRect morning{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "Tea")
            tea = item.rect;
        else if (item.id == "Work")
            work = item.rect;
        else if (item.id == "Rest")
            rest = item.rect;
        else if (item.id == "Morning")
            morning = item.rect;
    }
    // Tasks are 150 by 50, and a section's header spans its tasks, 10 above them.
    CHECK(tea.right - tea.left == doctest::Approx(150));
    CHECK(tea.bottom - tea.top == doctest::Approx(50));
    CHECK(work.left - tea.left == doctest::Approx(200));
    CHECK(rest.left - work.left == doctest::Approx(200));
    CHECK(morning.left == doctest::Approx(tea.left));
    CHECK(morning.right == doctest::Approx(work.right));
    CHECK(tea.top - morning.bottom == doctest::Approx(10));
    // The diagram is 470 high, and a title adds 70.
    CHECK(prMermaidExtent(diagram).y == doctest::Approx(540));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a timeline puts its periods in a row under their sections, with their events below them") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("timeline\ntitle History\nsection Early\n2002 : One : Two\n2004 : Three\nsection Late\n"
        "2010 : Four\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Timeline);
    auto find = [&](const char* id) {
        for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
            PrMermaidItem item = prMermaidItem(diagram, i);
            if (item.id == id)
                return item.rect;
        }
        return PrRect{};
    };
    PrRect early = find("Early");
    PrRect late = find("Late");
    PrRect p2002 = find("2002");
    PrRect p2004 = find("2004");
    PrRect p2010 = find("2010");
    PrRect one = find("One");
    PrRect two = find("Two");
    // Periods are 190 wide and 200 apart, and a section's header spans its periods.
    CHECK(p2002.right - p2002.left == doctest::Approx(190));
    CHECK(p2004.left - p2002.left == doctest::Approx(200));
    CHECK(p2010.left - p2004.left == doctest::Approx(200));
    CHECK(early.left == doctest::Approx(p2002.left));
    CHECK(early.right == doctest::Approx(p2004.right));
    CHECK(late.left == doctest::Approx(p2010.left));
    CHECK(p2002.top - early.bottom == doctest::Approx(50));
    // A line of text makes a box 47.8 high, and the sections and periods add 20. An event is at least 50 high.
    CHECK(early.bottom - early.top == doctest::Approx(67.8));
    CHECK(p2002.bottom - p2002.top == doctest::Approx(67.8));
    CHECK(one.top - p2002.top == doctest::Approx(200));
    CHECK(one.bottom - one.top == doctest::Approx(50));
    CHECK(two.top - one.bottom == doctest::Approx(10));
}

TEST_CASE("mermaid: a requirement diagram's block stacks its kind, name and fields, sized as Mermaid's") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("requirementDiagram\nrequirement r {\nid: 1\ntext: the text\nrisk: high\n"
        "verifymethod: test\n}\nelement e {\ntype: simulation\n}\nelement bare {\n}\n"
        "e - satisfies -> r\nr - contains -> bare\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Requirement);
    PrRect r{};
    PrRect e{};
    PrRect bare{};
    uint32_t edges = 0;
    uint32_t labels = 0;
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node && item.id == "r")
            r = item.rect;
        else if (item.kind == PrMermaidItemKind_Node && item.id == "e")
            e = item.rect;
        else if (item.kind == PrMermaidItemKind_Node && item.id == "bare")
            bare = item.rect;
        edges += item.kind == PrMermaidItemKind_Edge ? 1 : 0;
        labels += item.kind == PrMermaidItemKind_Label ? 1 : 0;
    }
    // Lines 24 high: the kind and the name, then 20 above four fields, with 8 above and 12 below them all.
    CHECK(r.bottom - r.top == doctest::Approx(48 + 20 + 4 * 24 + 20));
    CHECK(e.bottom - e.top == doctest::Approx(48 + 20 + 24 + 20));
    CHECK(bare.bottom - bare.top == doctest::Approx(48 + 20));
    // The layers are 50 apart, and the labels between them, 24 high, move them apart.
    CHECK(r.top - e.bottom == doctest::Approx(74));
    CHECK(bare.top - r.bottom == doctest::Approx(74));
    CHECK(edges == 2);
    CHECK(labels == 2);
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a quadrant chart is 500 square, with its points placed in the space its labels leave") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    REQUIRE(fonts.create("quadrantChart\ntitle Campaigns\nx-axis Low --> High\ny-axis Low --> High\n"
        "quadrant-1 Expand\nA: [0.3, 0.6]\nB: [1, 0] radius: 10\n", &diagram) == PrStatus_Ok);
    CHECK(prMermaidKind(diagram) == PrMermaidKind_Quadrant);
    CHECK(prMermaidExtent(diagram).x == doctest::Approx(500));
    CHECK(prMermaidExtent(diagram).y == doctest::Approx(500));
    PrRect a{};
    PrRect b{};
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.id == "A")
            a = item.rect;
        else if (item.id == "B")
            b = item.rect;
    }
    // The quadrants span 31 to 495 across, after the y axis's 26 and padding of 5, and 45 to 469 down, after the
    // title's 40 and before the x axis's 26.
    CHECK((a.left + a.right) * 0.5f == doctest::Approx(31 + 0.3f * 464));
    CHECK((a.top + a.bottom) * 0.5f == doctest::Approx(469 - 0.6f * 424));
    CHECK(a.right - a.left == doctest::Approx(10));
    CHECK(b.right - b.left == doctest::Approx(20));
    CHECK((b.left + b.right) * 0.5f == doctest::Approx(495));
    CHECK((b.top + b.bottom) * 0.5f == doctest::Approx(469));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: a flowchart's expanded shapes are sized as mermaid.js sizes them") {
    MermaidTestFonts fonts;
    PrMermaid* diagram = nullptr;
    // Every label is shorter than the least node width, so the sizes do not depend on the font.
    const char* source = "flowchart TB\n"
        "    a@{ shape: sm-circ } --> b@{ shape: fork } --> c@{ shape: cross-circ } --> d@{ shape: bolt }\n"
        "    e@{ shape: hourglass } --> f@{ shape: text, label: x } --> g@{ shape: doc, label: x }\n"
        "    h@{ shape: lin-rect, label: x } --> i@{ shape: docs, label: x } --> j@{ shape: notch-rect, label: x }\n";
    REQUIRE(fonts.create(source, &diagram) == PrStatus_Ok);
    struct Expected {
        const char* id;
        float width;
        float height;
    };
    // The labels are 120 px wide and one 24 px line high, and the padding is 15 px.
    const Expected expected[] = {
        {"a", 14, 14}, {"b", 70, 10}, {"c", 60, 60}, {"d", 35, 70}, {"e", 30, 30}, {"f", 135, 39},
        {"g", 150, 67.5f}, {"h", 166, 54}, {"i", 170, 101.9375f}, {"j", 147, 39},
    };
    for (const Expected& e : expected) {
        CAPTURE(e.id);
        bool found = false;
        for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
            PrMermaidItem item = prMermaidItem(diagram, i);
            if (item.kind != PrMermaidItemKind_Node || !(item.id == e.id))
                continue;
            found = true;
            CHECK(item.rect.right - item.rect.left == doctest::Approx(e.width));
            CHECK(item.rect.bottom - item.rect.top == doctest::Approx(e.height));
        }
        CHECK(found);
    }
    prMermaidDestroy(diagram);

    // A fork lies across the flow.
    REQUIRE(fonts.create("flowchart LR\n    a --> b@{ shape: fork } --> c\n", &diagram) == PrStatus_Ok);
    for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
        PrMermaidItem item = prMermaidItem(diagram, i);
        if (item.kind == PrMermaidItemKind_Node && item.id == "b") {
            CHECK(item.rect.right - item.rect.left == doctest::Approx(10));
            CHECK(item.rect.bottom - item.rect.top == doctest::Approx(70));
        }
    }
    prMermaidDestroy(diagram);
}

TEST_CASE("mermaid: every expanded shape lays out and draws, with its links clipped to it") {
    MermaidTestFonts fonts;
    const char* names[] = {
        "text", "notch-rect", "lin-rect", "div-rect", "sl-rect", "st-rect", "tag-rect", "bow-rect", "win-pane",
        "notch-pent", "curv-trap", "delay", "h-cyl", "lin-cyl", "doc", "lin-doc", "docs", "tag-doc", "flag", "tri",
        "flip-tri", "brace", "brace-r", "braces", "sm-circ", "fr-circ", "f-circ", "cross-circ", "fork", "hourglass",
        "bolt", "datastore", "folder", "bucket", "console", "browser", "person", "bang", "cloud",
    };
    for (const char* name : names) {
        CAPTURE(name);
        PrMermaid* diagram = nullptr;
        std::string source = std::string("flowchart TB\n    a in@--> b@{ shape: ") + name + ", label: Shape } out@--> c\n";
        REQUIRE(fonts.create(source.c_str(), &diagram) == PrStatus_Ok);
        PrRect b{};
        for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
            PrMermaidItem item = prMermaidItem(diagram, i);
            if (item.kind == PrMermaidItemKind_Node && item.id == "b")
                b = item.rect;
        }
        CHECK(b.right > b.left);
        CHECK(b.bottom > b.top);
        // Each link ends on or inside the node's box.
        for (uint32_t i = 0; i < prMermaidItemCount(diagram); ++i) {
            PrMermaidItem item = prMermaidItem(diagram, i);
            if (item.kind != PrMermaidItemKind_Edge)
                continue;
            REQUIRE(item.pointCount >= 2);
            PrVec2 end = item.id == "in" ? item.points[item.pointCount - 1] : item.points[0];
            CHECK(end.y >= b.top - 0.5f);
            CHECK(end.y <= b.bottom + 0.5f);
        }
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(diagram, list, PrVec2{0, 0}, PrColorScheme_Light);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
        prMermaidDestroy(diagram);
    }
}
