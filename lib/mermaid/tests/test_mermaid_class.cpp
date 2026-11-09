#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/mermaid.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_class.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed class diagram, freed when it goes out of scope.
struct Parsed {
    MermaidClassDiagram diagram{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidClassParse(diagram, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidClassReset(diagram);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidClassText(diagram, span);
        return std::string(view.data, view.size);
    }

    // The members of a node, as drawn, in source order.
    std::vector<std::string> members(uint32_t node) const {
        std::vector<std::string> out;
        for (const MermaidClassMember& member : diagram.members.span()) {
            if (member.node == node)
                out.push_back(text(member.text));
        }
        return out;
    }

    // The node whose ID is `id`, or kMermaidNone.
    uint32_t find(const char* id) const {
        for (uint32_t n = 0; n < diagram.nodes.count; ++n) {
            if (text(diagram.nodes.data[n].id) == id)
                return n;
        }
        return kMermaidNone;
    }
};

// A source of `depth` namespaces, each inside the one before, around a class.
std::string nestedNamespaces(int depth) {
    std::string source = "classDiagram\n";
    for (int i = 0; i < depth; ++i)
        source += "namespace N" + std::to_string(i) + " {\n";
    source += "class A\n";
    for (int i = 0; i < depth; ++i)
        source += "}\n";
    return source;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid class: members read as attributes or methods, with their visibility, types and classifiers") {
    Parsed parsed("classDiagram\n"
        "class Animal~T~ {\n"
        "    <<interface>>\n"
        "    +String name\n"
        "    -List~int~ ids\n"
        "    +count$\n"
        "    +makeSound(int times) void\n"
        "    +area()$ double\n"
        "    +get(Map~K,V~ map)* List~T~\n"
        "}\n"
        "Animal : #age int\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.nodes.count == 1);
    const MermaidClassNode& animal = parsed.diagram.nodes.data[0];
    CHECK(parsed.text(animal.id) == "Animal");
    CHECK(parsed.text(animal.label) == "Animal<T>");
    CHECK(parsed.text(animal.annotation) == "interface");
    std::vector<std::string> expected = {
        "+String name", "-List<int> ids", "+count", "+makeSound(int times) : void", "+area() : double",
        "+get(Map<K,V> map) : List<T>", "#age int",
    };
    CHECK(parsed.members(0) == expected);
    const MermaidClassMember* members = parsed.diagram.members.data;
    CHECK_FALSE(members[0].method);
    CHECK(members[2].classifier == MermaidClassifier_Static);
    CHECK(members[3].method);
    CHECK(members[4].classifier == MermaidClassifier_Static);
    CHECK(members[5].classifier == MermaidClassifier_Abstract);
}

TEST_CASE("mermaid class: relations give their marks, stroke, cardinalities and label") {
    Parsed parsed("classDiagram\n"
        "A <|-- B\nC \"1\" *-- \"many\" D : has\nE o.. F\nG --> H\nI ..|> J\nK -- L\nM ()-- N\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.relations.count == 7);
    const MermaidRelation* r = parsed.diagram.relations.data;
    CHECK(r[0].start == MermaidRelationEnd_Extension);
    CHECK(r[0].end == MermaidRelationEnd_None);
    CHECK(parsed.text(parsed.diagram.nodes.data[r[0].source].id) == "A");
    CHECK(r[1].start == MermaidRelationEnd_Composition);
    CHECK(parsed.text(r[1].sourceCardinality) == "1");
    CHECK(parsed.text(r[1].targetCardinality) == "many");
    CHECK(parsed.text(r[1].label) == "has");
    CHECK(r[2].start == MermaidRelationEnd_Aggregation);
    CHECK(r[2].dashed);
    CHECK(r[3].end == MermaidRelationEnd_Dependency);
    CHECK(r[4].end == MermaidRelationEnd_Extension);
    CHECK(r[4].dashed);
    CHECK(r[5].start == MermaidRelationEnd_None);
    CHECK(r[5].end == MermaidRelationEnd_None);
    // The far end of a lollipop is an interface, which is not a class.
    CHECK(r[6].start == MermaidRelationEnd_Lollipop);
    const MermaidClassNode& interface = parsed.diagram.nodes.data[r[6].source];
    CHECK(interface.kind == MermaidClassNodeKind_Interface);
    CHECK(parsed.text(interface.label) == "M");
}

TEST_CASE("mermaid class: namespaces hold their classes, and notes attach to theirs") {
    Parsed parsed("---\ntitle: Zoo\n---\nclassDiagram\ndirection LR\n"
        "namespace Animals {\n    class Dog\n    class Cat[\"House cat\"]\n}\n"
        "class Bird\n"
        "note for Dog \"Barks\"\nnote \"Alone\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.diagram.title) == "Zoo");
    CHECK(parsed.diagram.direction == PrGraphDirection_Right);
    REQUIRE(parsed.diagram.namespaces.count == 1);
    CHECK(parsed.text(parsed.diagram.namespaces.data[0].name) == "Animals");
    const MermaidClassNode* nodes = parsed.diagram.nodes.data;
    REQUIRE(parsed.diagram.nodes.count == 5);
    CHECK(nodes[0].space == 0);
    CHECK(parsed.text(nodes[1].label) == "House cat");
    CHECK(nodes[2].space == kMermaidNone);
    CHECK(nodes[3].kind == MermaidClassNodeKind_Note);
    CHECK(parsed.text(nodes[3].label) == "Barks");
    REQUIRE(parsed.diagram.relations.count == 1);
    CHECK(parsed.diagram.relations.data[0].note);
    CHECK(parsed.diagram.relations.data[0].source == 3);
    CHECK(parsed.diagram.relations.data[0].target == 0);
}

TEST_CASE("mermaid class: styles come from classDef, cssClass, ::: and style") {
    Parsed parsed("classDiagram\n"
        "class A:::warm\nclass B\nclassDef warm fill:#f00\nclassDef cool fill:#00f,stroke:#0f0\n"
        "cssClass \"B\" cool\nstyle B fill:#fff\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidClassNode* nodes = parsed.diagram.nodes.data;
    CHECK((nodes[0].style.flags & MermaidPaintFlag_Fill) != 0);
    CHECK(nodes[0].style.fill.color.r == 255);
    CHECK(nodes[1].style.fill.color.b == 255);
    CHECK(nodes[1].style.fill.color.r == 255);
    CHECK((nodes[1].style.flags & MermaidPaintFlag_Stroke) != 0);
}

TEST_CASE("mermaid class: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"classDiagram\nclass A {\n+x\n", 2},
        {"classDiagram\nnamespace N {\nclass A\n", 2},
        {"classDiagram\nA <|-- B\nA ?? B\n", 3},
        {"classDiagram\n}\n", 2},
        {"graph TD\n", 1},
        // Keywords are case-sensitive, as in mermaid.js.
        {"classDiagram\nClass A\n", 2},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid class: click and link statements give a class its URL, if the class is named and the URL is safe") {
    Parsed parsed("classDiagram\nclass A\nclass B\nclass C\nclass D\nclass F\n"
        "click A href \"https://a.example\" \"A tooltip\"\nlink B \"https://b.example\"\nclick C call open()\n"
        "click D href \"javascript:alert(1)\"\nclick E href \"https://e.example\"\ncallback F \"open\"\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    // As in mermaid.js, a click statement does not name a new class.
    CHECK(parsed.diagram.nodes.count == 5);
    CHECK(parsed.find("E") == kMermaidNone);
    const MermaidClassNode* nodes = parsed.diagram.nodes.data;
    CHECK(parsed.text(nodes[parsed.find("A")].link) == "https://a.example");
    CHECK(parsed.text(nodes[parsed.find("B")].link) == "https://b.example");
    CHECK(nodes[parsed.find("C")].link.size == 0);
    CHECK(nodes[parsed.find("D")].link.size == 0);
    CHECK(nodes[parsed.find("F")].link.size == 0);
    // A link before its class is named gives none.
    Parsed early("classDiagram\nlink A \"https://a.example\"\nclass A\n");
    REQUIRE(early.status == PrStatus_Ok);
    CHECK(early.diagram.nodes.data[0].link.size == 0);
}

TEST_CASE("mermaid class: style and cssClass statements style only classes that the source has named") {
    Parsed parsed("classDiagram\nstyle X fill:#f00\ncssClass \"Y\" warm\nclass A\nstyle A fill:#0f0\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.nodes.count == 1);
    CHECK(parsed.diagram.nodes.data[0].style.fill.color.g == 0xff);
}

TEST_CASE("mermaid class: generic types turn their tildes into angle brackets from the outside in") {
    Parsed parsed("classDiagram\nclass A {\n+List~List~int~~ nested\n}\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.members(0) == std::vector<std::string>{"+List<List<int>> nested"});
    // A member with many tildes reads in time linear in its length.
    std::string many = "classDiagram\nclass A {\n+x";
    for (int i = 0; i < 20000; ++i)
        many += "~";
    many += "\n}\n";
    Parsed tildes(many.c_str());
    REQUIRE(tildes.status == PrStatus_Ok);
    std::string member = tildes.members(0)[0];
    CHECK(member.size() == 20002);
    CHECK(member.substr(0, 4) == "+x<<");
    CHECK(member.substr(member.size() - 2) == ">>");
}

TEST_CASE("mermaid class: namespaces nest at most 64 deep") {
    CHECK(Parsed(nestedNamespaces(64).c_str()).status == PrStatus_Ok);
    Parsed tooDeep(nestedNamespaces(65).c_str());
    CHECK(tooDeep.status == PrStatus_Syntax);
    CHECK(tooDeep.error.line == 66);
    CHECK(tooDeep.error.message == "a namespace nests too deeply");
}

TEST_CASE("mermaid class: a class's item carries its link, and a relation's item has no ID") {
    MermaidDrawn drawn("classDiagram\nA <|-- B\nclick A href \"https://a.example\"\n");
    REQUIRE(drawn.status == PrStatus_Ok);
    std::vector<PrMermaidItem> nodes = drawn.itemsOf(PrMermaidItemKind_Node);
    REQUIRE(nodes.size() == 2);
    CHECK(nodes[0].id == "A");
    CHECK(nodes[0].link == "https://a.example");
    CHECK(nodes[1].link.size == 0);
    std::vector<PrMermaidItem> edges = drawn.itemsOf(PrMermaidItemKind_Edge);
    REQUIRE(edges.size() == 1);
    CHECK(edges[0].id.size == 0);
}

TEST_CASE("mermaid class: many classes, members and namespaces lay out in bounded time") {
    std::string source = "classDiagram\n";
    for (int i = 0; i < 1000; ++i) {
        std::string name = "C" + std::to_string(i);
        source += "namespace S" + std::to_string(i) + " {\nclass " + name + " {\n+int x\n+f()\n}\n}\n";
        if (i > 0)
            source += "C" + std::to_string(i - 1) + " --> " + name + "\n";
    }
    MermaidDrawn drawn(source.c_str());
    REQUIRE(drawn.status == PrStatus_Ok);
    CHECK(drawn.itemsOf(PrMermaidItemKind_Node).size() == 1000);
    CHECK(drawn.itemsOf(PrMermaidItemKind_Group).size() == 1000);
}

TEST_CASE("mermaid class: deeply nested namespaces draw") {
    MermaidDrawn drawn(nestedNamespaces(64).c_str());
    REQUIRE(drawn.status == PrStatus_Ok);
    CHECK(drawn.itemsOf(PrMermaidItemKind_Group).size() == 64);
}
