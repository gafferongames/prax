#include "mermaid/src/mermaid_flowchart.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A parsed flowchart, freed when it goes out of scope.
struct Parsed {
    MermaidFlowchart chart{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidFlowchartParse(chart, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidFlowchartReset(chart);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidFlowchartText(chart, span);
        return std::string(view.data, view.size);
    }

    // A label as it is drawn: its source decoded in its format.
    std::string decoded(const MermaidNode& node) const {
        RawArray<char> out{};
        mermaidTextDecode(mermaidFlowchartText(chart, node.label), node.format, out);
        std::string result(out.data, out.count);
        out.reset();
        return result;
    }

    uint32_t node(const char* id) const {
        for (uint32_t i = 0; i < chart.nodes.count; ++i) {
            if (text(chart.nodes.data[i].id) == id)
                return i;
        }
        return kMermaidNone;
    }

    const MermaidNode& nodeNamed(const char* id) const {
        uint32_t index = node(id);
        REQUIRE(index != kMermaidNone);
        return chart.nodes.data[index];
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid flowchart: the header gives the direction, and nodes take their shapes and labels") {
    Parsed parsed("flowchart LR\n"
        "    a[Rect] --> b(Round) --> c([Stadium]) --> d[[Subroutine]] --> e[(Database)]\n"
        "    f((Circle)) --> g(((Double))) --> h>Flag] --> i{Decision} --> j{{Hexagon}}\n"
        "    k[/Lean right/] --> l[\\Lean left\\] --> m[/Trapezoid\\] --> n[\\Inverted/]\n"
        "    o\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.chart.direction == PrGraphDirection_Right);
    struct Expected {
        const char* id;
        MermaidShape shape;
        const char* label;
    };
    const Expected expected[] = {
        {"a", MermaidShape_Rect, "Rect"},
        {"b", MermaidShape_Round, "Round"},
        {"c", MermaidShape_Stadium, "Stadium"},
        {"d", MermaidShape_Subroutine, "Subroutine"},
        {"e", MermaidShape_Cylinder, "Database"},
        {"f", MermaidShape_Circle, "Circle"},
        {"g", MermaidShape_DoubleCircle, "Double"},
        {"h", MermaidShape_Asymmetric, "Flag"},
        {"i", MermaidShape_Diamond, "Decision"},
        {"j", MermaidShape_Hexagon, "Hexagon"},
        {"k", MermaidShape_LeanRight, "Lean right"},
        {"l", MermaidShape_LeanLeft, "Lean left"},
        {"m", MermaidShape_Trapezoid, "Trapezoid"},
        {"n", MermaidShape_InvTrapezoid, "Inverted"},
        {"o", MermaidShape_Rect, "o"},
    };
    for (const Expected& e : expected) {
        INFO(e.id);
        const MermaidNode& node = parsed.nodeNamed(e.id);
        CHECK(node.shape == e.shape);
        CHECK(parsed.text(node.label) == e.label);
    }
    CHECK(parsed.chart.edges.count == 11);
}

TEST_CASE("mermaid flowchart: links take their strokes, marks, lengths and labels") {
    Parsed parsed("graph TD\n"
        "A --> B\n"
        "A --- C\n"
        "A -.-> D\n"
        "A ==> E\n"
        "A ~~~ F\n"
        "A --o G\n"
        "A --x H\n"
        "A <--> I\n"
        "A ----> J\n"
        "A -- text --> K\n"
        "A -->|pipe| L\n"
        "A -. dotted .-> M\n"
        "A == thick ==> N\n"
        "A -..-> O\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidEdge* edges = parsed.chart.edges.data;
    REQUIRE(parsed.chart.edges.count == 14);
    CHECK((edges[0].line == MermaidLinkStroke_Normal && edges[0].end == MermaidHead_Arrow && edges[0].length == 1));
    CHECK((edges[1].line == MermaidLinkStroke_Normal && edges[1].end == MermaidHead_None && edges[1].length == 1));
    CHECK((edges[2].line == MermaidLinkStroke_Dotted && edges[2].end == MermaidHead_Arrow));
    CHECK((edges[3].line == MermaidLinkStroke_Thick && edges[3].end == MermaidHead_Arrow));
    CHECK(edges[4].line == MermaidLinkStroke_Invisible);
    CHECK(edges[5].end == MermaidHead_Circle);
    CHECK(edges[6].end == MermaidHead_Cross);
    CHECK((edges[7].start == MermaidHead_Arrow && edges[7].end == MermaidHead_Arrow));
    CHECK(edges[8].length == 3);
    CHECK(parsed.text(edges[9].label) == "text");
    CHECK(edges[9].end == MermaidHead_Arrow);
    CHECK(parsed.text(edges[10].label) == "pipe");
    CHECK((parsed.text(edges[11].label) == "dotted" && edges[11].line == MermaidLinkStroke_Dotted));
    CHECK((parsed.text(edges[12].label) == "thick" && edges[12].line == MermaidLinkStroke_Thick));
    CHECK((edges[13].line == MermaidLinkStroke_Dotted && edges[13].length == 2));
}

TEST_CASE("mermaid flowchart: chains and ampersands make a link for each pair") {
    Parsed parsed("graph LR; a & b --> c & d --> e");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.chart.edges.count == 6);
    CHECK(parsed.chart.nodes.count == 5);
}

TEST_CASE("mermaid flowchart: subgraphs hold the nodes that they name, and may set a direction") {
    Parsed parsed("flowchart TB\n"
        "    c1-->a2\n"
        "    subgraph one [First]\n"
        "        direction LR\n"
        "        a1-->a2\n"
        "        subgraph inner\n"
        "            x\n"
        "        end\n"
        "    end\n"
        "    subgraph \"Second\"\n"
        "        b1\n"
        "    end\n"
        "    one --> b1\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    uint32_t one = parsed.node("one");
    REQUIRE(one != kMermaidNone);
    CHECK(parsed.chart.nodes.data[one].isSubgraph);
    CHECK(parsed.text(parsed.chart.nodes.data[one].label) == "First");
    CHECK(parsed.chart.nodes.data[one].direction == PrGraphDirection_Right);
    CHECK(parsed.nodeNamed("a1").parent == one);
    CHECK(parsed.nodeNamed("a2").parent == one);
    CHECK(parsed.nodeNamed("c1").parent == kMermaidNone);
    CHECK(parsed.nodeNamed("inner").parent == one);
    CHECK(parsed.nodeNamed("x").parent == parsed.node("inner"));
    CHECK(parsed.nodeNamed("b1").parent == parsed.node("Second"));
}

TEST_CASE("mermaid flowchart: classes and styles resolve in order, with the default class first") {
    Parsed parsed("graph TD\n"
        "classDef default fill:#eee\n"
        "classDef warn fill:#f96,stroke:#333,stroke-width:4px\n"
        "a:::warn --> b --> c\n"
        "class c warn\n"
        "style c fill:rgb(0, 128, 255),stroke-dasharray: 5 5\n"
        "linkStyle 1 stroke:red\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidNode& a = parsed.nodeNamed("a");
    const MermaidNode& b = parsed.nodeNamed("b");
    const MermaidNode& c = parsed.nodeNamed("c");
    CHECK(a.style.fill.color.r == 0xff);
    CHECK(a.style.strokeWidth == 4);
    CHECK(b.style.fill.color.r == 0xee);
    CHECK((c.style.fill.color.r == 0 && c.style.fill.color.b == 255));
    CHECK(c.style.dash[0] == 5);
    CHECK((parsed.chart.edges.data[1].style.flags & MermaidPaintFlag_Stroke) != 0);
    CHECK((parsed.chart.edges.data[0].style.flags & MermaidPaintFlag_Stroke) == 0);
}

TEST_CASE("mermaid flowchart: labels keep their source without quotes, and decode line breaks, entities and Markdown") {
    Parsed parsed("graph TD\n"
        "a[\"A [quoted] label\"]\n"
        "b[one<br>two<br/>three]\n"
        "c[say #quot;hi#quot; #9829;]\n"
        "d[\"`**Bold** text`\"]\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.nodeNamed("a").label) == "A [quoted] label");
    CHECK(parsed.nodeNamed("a").format == MermaidTextFormat_Html);
    CHECK(parsed.text(parsed.nodeNamed("b").label) == "one<br>two<br/>three");
    CHECK(parsed.decoded(parsed.nodeNamed("b")) == "one\ntwo\nthree");
    CHECK(parsed.decoded(parsed.nodeNamed("c")) == "say \"hi\" ♥");
    CHECK(parsed.text(parsed.nodeNamed("d").label) == "**Bold** text");
    CHECK(parsed.nodeNamed("d").format == MermaidTextFormat_Markdown);
    CHECK(parsed.decoded(parsed.nodeNamed("d")) == "Bold text");
}

TEST_CASE("mermaid flowchart: front matter, comments and semicolons") {
    Parsed parsed("---\n"
        "title: Example\n"
        "---\n"
        "%%{init: {\"theme\": \"forest\"}}%%\n"
        "flowchart TD;\n"
        "%% a comment\n"
        "A-->B; B-->C[x;y]\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.chart.edges.count == 2);
    CHECK(parsed.text(parsed.nodeNamed("C").label) == "x;y");
}

TEST_CASE("mermaid flowchart: errors say where") {
    {
        Parsed parsed("graph TD\nA --> B\nA -- B\n");
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 3);
    }
    {
        Parsed parsed("graph TD\nA[unclosed --> B\n");
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
        CHECK(parsed.error.column == 2);
    }
    {
        Parsed parsed("graph TD\nsubgraph s\nA\n");
        CHECK(parsed.status == PrStatus_Syntax);
    }
    {
        // Another kind of diagram is not a flowchart. prMermaidCreate dispatches on the kind before any parser reads
        // it.
        Parsed parsed("sequenceDiagram\nAlice->>Bob: Hi\n");
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 1);
    }
}

TEST_CASE("mermaid flowchart: a node's metadata names its shape and sets its label") {
    Parsed parsed("flowchart TD\n"
        "    a@{ shape: cyl, label: \"Disk\" } --> b@{ shape: db }\n"
        "    c[Text]@{ shape: notch-rect } --> d@{shape: decision, label: '**Bold**'}:::warm\n"
        "    e@{ shape: lin-doc, icon: \"fa:user\", form: square, pos: t, w: 60, h: 40, constraint: on }\n"
        "    f:::warm@{ shape: fork }\n"
        "    g@{ label: \"`Markdown` text\" }\n"
        "    h@{\n"
        "      shape: brace-r\n"
        "      label: \"Over, two lines\"\n"
        "    } --> a\n"
        "    subgraph s\n"
        "    end\n"
        "    s@{ shape: circle }\n"
        "    classDef warm stroke:#f00\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.nodeNamed("a").shape == MermaidShape_Cylinder);
    CHECK(parsed.text(parsed.nodeNamed("a").label) == "Disk");
    CHECK(parsed.nodeNamed("a").format == MermaidTextFormat_Markdown);
    CHECK(parsed.nodeNamed("b").shape == MermaidShape_Cylinder);
    CHECK(parsed.text(parsed.nodeNamed("b").label) == "b");
    CHECK(parsed.nodeNamed("c").shape == MermaidShape_NotchedRect);
    CHECK(parsed.text(parsed.nodeNamed("c").label) == "Text");
    CHECK(parsed.nodeNamed("d").shape == MermaidShape_Diamond);
    CHECK(parsed.decoded(parsed.nodeNamed("d")) == "Bold");
    CHECK(parsed.nodeNamed("e").shape == MermaidShape_LinedDocument);
    CHECK(parsed.nodeNamed("f").shape == MermaidShape_Fork);
    CHECK(parsed.nodeNamed("g").shape == MermaidShape_Rect);
    CHECK(parsed.text(parsed.nodeNamed("g").label) == "`Markdown` text");
    CHECK(parsed.nodeNamed("h").shape == MermaidShape_BraceRight);
    CHECK(parsed.text(parsed.nodeNamed("h").label) == "Over, two lines");
    CHECK(parsed.nodeNamed("s").isSubgraph);
    CHECK(parsed.chart.edges.count == 3);
    CHECK((parsed.nodeNamed("d").style.flags & MermaidPaintFlag_Stroke) != 0);
    CHECK((parsed.nodeNamed("f").style.flags & MermaidPaintFlag_Stroke) != 0);
    CHECK(parsed.chart.nodes.count == 9);
}

TEST_CASE("mermaid flowchart: shape names and their aliases select mermaid.js's shapes") {
    struct Expected {
        const char* name;
        MermaidShape shape;
    };
    const Expected expected[] = {
        {"rect", MermaidShape_Rect}, {"process", MermaidShape_Rect}, {"rounded", MermaidShape_Round},
        {"event", MermaidShape_Round}, {"pill", MermaidShape_Stadium}, {"fr-rect", MermaidShape_Subroutine},
        {"database", MermaidShape_Cylinder}, {"circle", MermaidShape_Circle}, {"dbl-circ", MermaidShape_DoubleCircle},
        {"odd", MermaidShape_Asymmetric}, {"diam", MermaidShape_Diamond}, {"prepare", MermaidShape_Hexagon},
        {"lean-r", MermaidShape_LeanRight}, {"lean-l", MermaidShape_LeanLeft}, {"trap-b", MermaidShape_Trapezoid},
        {"trap-t", MermaidShape_InvTrapezoid}, {"text", MermaidShape_Text}, {"card", MermaidShape_NotchedRect},
        {"lin-rect", MermaidShape_LinedRect}, {"div-rect", MermaidShape_DividedRect},
        {"sl-rect", MermaidShape_SlopedRect}, {"st-rect", MermaidShape_StackedRect},
        {"tag-rect", MermaidShape_TaggedRect}, {"bow-rect", MermaidShape_BowTieRect},
        {"win-pane", MermaidShape_WindowPane}, {"notch-pent", MermaidShape_NotchedPentagon},
        {"curv-trap", MermaidShape_CurvedTrapezoid}, {"delay", MermaidShape_Delay},
        {"h-cyl", MermaidShape_HorizontalCylinder}, {"lin-cyl", MermaidShape_LinedCylinder},
        {"doc", MermaidShape_Document}, {"lin-doc", MermaidShape_LinedDocument},
        {"docs", MermaidShape_StackedDocument}, {"tag-doc", MermaidShape_TaggedDocument},
        {"flag", MermaidShape_PaperTape}, {"tri", MermaidShape_Triangle}, {"flip-tri", MermaidShape_FlippedTriangle},
        {"brace", MermaidShape_BraceLeft}, {"brace-r", MermaidShape_BraceRight}, {"braces", MermaidShape_Braces},
        {"sm-circ", MermaidShape_SmallCircle}, {"fr-circ", MermaidShape_FramedCircle},
        {"f-circ", MermaidShape_FilledCircle}, {"cross-circ", MermaidShape_CrossedCircle},
        {"join", MermaidShape_Fork}, {"hourglass", MermaidShape_Hourglass}, {"bolt", MermaidShape_Bolt},
        {"datastore", MermaidShape_DataStore}, {"folder", MermaidShape_Folder}, {"bucket", MermaidShape_Bucket},
        {"console", MermaidShape_Console}, {"browser", MermaidShape_Browser}, {"person", MermaidShape_Person},
        {"bang", MermaidShape_Bang}, {"cloud", MermaidShape_Cloud},
    };
    for (const Expected& e : expected) {
        CAPTURE(e.name);
        std::string source = std::string("flowchart TD\n    a@{ shape: ") + e.name + " }\n";
        Parsed parsed(source.c_str());
        REQUIRE(parsed.status == PrStatus_Ok);
        CHECK(parsed.nodeNamed("a").shape == e.shape);
    }
}

TEST_CASE("mermaid flowchart: an unknown shape or an unclosed metadata block is an error") {
    {
        Parsed parsed("flowchart TD\n    a --> b@{ shape: blob }\n");
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
        CHECK(parsed.error.column == 22);
    }
    {
        // Shape names are lowercase, and mermaid.js's internal names with underscores are not shapes.
        Parsed upper("flowchart TD\n    a@{ shape: Rect }\n");
        CHECK(upper.status == PrStatus_Syntax);
        Parsed underscore("flowchart TD\n    a@{ shape: lean_right }\n");
        CHECK(underscore.status == PrStatus_Syntax);
    }
    {
        Parsed parsed("flowchart TD\n    a@{ shape: rect\n");
        CHECK(parsed.status == PrStatus_Syntax);
    }
}

TEST_CASE("mermaid flowchart: keywords are lowercase, so that End and Class name nodes") {
    Parsed parsed("graph TD\n"
        "subgraph s\n"
        "A --> End\n"
        "end\n"
        "Class --> Object\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.nodeNamed("End").parent == parsed.node("s"));
    CHECK(parsed.nodeNamed("Class").parent == kMermaidNone);
    CHECK(parsed.node("Object") != kMermaidNone);
    CHECK(parsed.chart.edges.count == 2);
}

TEST_CASE("mermaid flowchart: a directive may span lines, and front matter may hold a quote") {
    Parsed parsed("---\n"
        "title: A 5\" disk\n"
        "---\n"
        "%%{\n"
        "  init: {'theme': 'dark'}\n"
        "}%%\n"
        "flowchart LR\n"
        "A --> B\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.chart.direction == PrGraphDirection_Right);
    CHECK(parsed.text(parsed.chart.title) == "A 5\" disk");
    CHECK(parsed.chart.edges.count == 1);
}

TEST_CASE("mermaid flowchart: a link may be named, and its name takes metadata and classes") {
    Parsed parsed("flowchart LR\n"
        "A e1@--> B & C\n"
        "B e1@--> D\n"
        "e1@{ animate: true }\n"
        "classDef hot stroke:#f00\n"
        "class e1 hot\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.chart.edges.count == 3);
    // A name goes to the first link that it makes, and only once.
    CHECK(parsed.text(parsed.chart.edges.data[0].id) == "e1");
    CHECK(parsed.chart.edges.data[1].id.size == 0);
    CHECK(parsed.chart.edges.data[2].id.size == 0);
    CHECK(parsed.node("e1") == kMermaidNone);
    CHECK((parsed.chart.edges.data[0].style.flags & MermaidPaintFlag_Stroke) != 0);
    CHECK((parsed.chart.edges.data[1].style.flags & MermaidPaintFlag_Stroke) == 0);
}

TEST_CASE("mermaid flowchart: a click statement gives a node made before it a link, unless the link runs script") {
    Parsed parsed("flowchart LR\n"
        "A --> B --> C\n"
        "click A \"https://example.com/a\" \"A tooltip\" _blank\n"
        "click B href \"docs/b.html\"\n"
        "click C \"java\tscript:alert(1)\"\n"
        "click D \"https://example.com/d\"\n"
        "click A callback \"ignored\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.nodeNamed("A").link) == "https://example.com/a");
    CHECK(parsed.text(parsed.nodeNamed("B").link) == "docs/b.html");
    CHECK(parsed.nodeNamed("C").link.size == 0);
    CHECK(parsed.node("D") == kMermaidNone);
}

TEST_CASE("mermaid flowchart: linkStyle replaces a link's style, as the default does for the links that none names") {
    Parsed parsed("graph TD\n"
        "a --> b --> c --> d\n"
        "linkStyle default stroke:#00f\n"
        "linkStyle 1 stroke-width:4px\n"
        "linkStyle 1 stroke:#f00\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidPaint& first = parsed.chart.edges.data[0].style;
    const MermaidPaint& second = parsed.chart.edges.data[1].style;
    CHECK(first.stroke.color.b == 0xff);
    CHECK(second.stroke.color.r == 0xff);
    CHECK((second.flags & MermaidPaintFlag_StrokeWidth) == 0);

    Parsed past("graph TD\na --> b\nlinkStyle 1 stroke:#f00\n");
    CHECK(past.status == PrStatus_Syntax);
    CHECK(past.error.line == 3);
}

TEST_CASE("mermaid flowchart: a class statement styles nodes that exist, and makes none") {
    Parsed parsed("graph TD\n"
        "classDef hot fill:#f00\n"
        "class a,ghost hot\n"
        "a --> b\n"
        "class b hot\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.node("ghost") == kMermaidNone);
    CHECK((parsed.nodeNamed("a").style.flags & MermaidPaintFlag_Fill) == 0);
    CHECK((parsed.nodeNamed("b").style.flags & MermaidPaintFlag_Fill) != 0);
}

TEST_CASE("mermaid flowchart: subgraphs nest 64 deep, and a link spans at most 16 layers") {
    std::string nested = "graph TD\n";
    for (int i = 0; i < 64; ++i)
        nested += "subgraph s" + std::to_string(i) + "\n";
    nested += "A\n";
    for (int i = 0; i < 64; ++i)
        nested += "end\n";
    CHECK(Parsed(nested.c_str()).status == PrStatus_Ok);
    std::string deeper = "graph TD\nsubgraph x\n" + nested.substr(9) + "end\n";
    Parsed tooDeep(deeper.c_str());
    CHECK(tooDeep.status == PrStatus_Syntax);
    CHECK(tooDeep.error.line == 66);

    std::string longest = "graph TD\nA " + std::string(20000, '-') + "> B\n";
    Parsed linked(longest.c_str());
    REQUIRE(linked.status == PrStatus_Ok);
    CHECK(linked.chart.edges.data[0].length == 16);
}

TEST_CASE("mermaid flowchart: a quote left open runs on to the line that closes it, or to the end") {
    std::string source = "graph TD\nA[\"x\n";
    for (int i = 0; i < 20000; ++i)
        source += "B-->C\n";
    Parsed open(source.c_str());
    CHECK(open.status == PrStatus_Syntax);
    CHECK(open.error.line == 2);
    Parsed closed("graph TD\nA[\"one\ntwo\"] --> B\nB --> C\n");
    REQUIRE(closed.status == PrStatus_Ok);
    CHECK(closed.text(closed.nodeNamed("A").label) == "one\ntwo");
    CHECK(closed.chart.edges.count == 2);
}
