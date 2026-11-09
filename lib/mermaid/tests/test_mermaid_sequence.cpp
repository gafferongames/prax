#include "pr/float.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/mermaid.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_sequence.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed sequence diagram, freed when it goes out of scope.
struct Parsed {
    MermaidSequence diagram{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidSequenceParse(diagram, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidSequenceReset(diagram);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidSequenceText(diagram, span);
        return std::string(view.data, view.size);
    }

    // The steps of one kind, in order.
    std::vector<MermaidStep> steps(MermaidStepKind kind) const {
        std::vector<MermaidStep> out;
        for (const MermaidStep& step : diagram.steps.span()) {
            if (step.kind == kind)
                out.push_back(step);
        }
        return out;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid sequence: participants come in the order the source names them, declared or not") {
    Parsed parsed("sequenceDiagram\n"
        "    participant A as Alice Smith\n"
        "    actor B\n"
        "    A->>C: Hi\n"
        "    participant Q@{ \"type\": \"queue\" }\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.actors.count == 4);
    CHECK(parsed.text(parsed.diagram.actors.data[0].id) == "A");
    CHECK(parsed.text(parsed.diagram.actors.data[0].label) == "Alice Smith");
    CHECK(parsed.diagram.actors.data[1].kind == MermaidActorKind_Actor);
    CHECK(parsed.text(parsed.diagram.actors.data[2].id) == "C");
    CHECK(parsed.text(parsed.diagram.actors.data[2].label) == "C");
    CHECK(parsed.text(parsed.diagram.actors.data[3].id) == "Q");
    CHECK(parsed.diagram.actors.data[3].kind == MermaidActorKind_Queue);
}

TEST_CASE("mermaid sequence: each arrow gives its line and heads, and + and - activate and deactivate") {
    Parsed parsed("sequenceDiagram\n"
        "A->>B: solid\nA-->>B: dotted\nA->B: open\nA-->B: dotted open\nA-xB: cross\nA--xB: dotted cross\n"
        "A-)B: async\nA--)B: dotted async\nA<<->>B: both\nA<<-->>B: both dotted\n"
        "A->>+B: start\nB-->>-A: end\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    std::vector<MermaidStep> messages = parsed.steps(MermaidStepKind_Message);
    REQUIRE(messages.size() == 12);
    const MermaidArrow expected[] = {
        MermaidArrow_Solid, MermaidArrow_Dotted, MermaidArrow_SolidOpen, MermaidArrow_DottedOpen,
        MermaidArrow_SolidCross, MermaidArrow_DottedCross, MermaidArrow_SolidPoint, MermaidArrow_DottedPoint,
        MermaidArrow_BothSolid, MermaidArrow_BothDotted, MermaidArrow_Solid, MermaidArrow_Dotted,
    };
    for (size_t i = 0; i < messages.size(); ++i) {
        INFO("message " << i);
        CHECK(messages[i].arrow == expected[i]);
    }
    CHECK(parsed.text(messages[0].text) == "solid");
    CHECK(messages[10].activates);
    // Each activation follows its message: B's starts after the first, and ends after B's reply.
    std::vector<MermaidStep> starts = parsed.steps(MermaidStepKind_Activate);
    std::vector<MermaidStep> ends = parsed.steps(MermaidStepKind_Deactivate);
    REQUIRE(starts.size() == 1);
    REQUIRE(ends.size() == 1);
    CHECK(starts[0].from == 1);
    CHECK(ends[0].from == 1);
}

TEST_CASE("mermaid sequence: notes sit left of, right of, or over one or two participants") {
    Parsed parsed("sequenceDiagram\n"
        "Note left of A: one\nnote right of B: two\nNote over A,B: three\nNote over C: four\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    std::vector<MermaidStep> notes = parsed.steps(MermaidStepKind_Note);
    REQUIRE(notes.size() == 4);
    CHECK(notes[0].place == MermaidNotePlace_LeftOf);
    CHECK(notes[1].place == MermaidNotePlace_RightOf);
    CHECK(notes[2].place == MermaidNotePlace_Over);
    CHECK(notes[2].from == 0);
    CHECK(notes[2].to == 1);
    CHECK(notes[3].from == notes[3].to);
    CHECK(parsed.text(notes[3].text) == "four");
}

TEST_CASE("mermaid sequence: blocks nest, sections belong to their block, and boxes hold their participants") {
    Parsed parsed("sequenceDiagram\n"
        "box Aqua Front\nparticipant A\nend\nparticipant B\n"
        "alt ok\nloop retry\nA->>B: x\nend\nelse failed\nA->>B: y\nend\n"
        "par one\nA->>B: z\nand two\nB->>A: w\nend\n"
        "rect rgb(1, 2, 3)\nA->>B: v\nend\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.boxes.count == 1);
    CHECK(parsed.diagram.boxes.data[0].hasColor);
    CHECK(parsed.text(parsed.diagram.boxes.data[0].label) == "Front");
    CHECK(parsed.diagram.actors.data[0].box == 0);
    CHECK(parsed.diagram.actors.data[1].box == kMermaidNone);
    std::vector<MermaidStep> starts = parsed.steps(MermaidStepKind_BlockStart);
    std::vector<MermaidStep> sections = parsed.steps(MermaidStepKind_BlockSection);
    std::vector<MermaidStep> ends = parsed.steps(MermaidStepKind_BlockEnd);
    REQUIRE(starts.size() == 4);
    CHECK(starts[0].block == MermaidBlock_Alt);
    CHECK(starts[1].block == MermaidBlock_Loop);
    CHECK(starts[3].block == MermaidBlock_Rect);
    CHECK(parsed.text(starts[3].text) == "rgb(1, 2, 3)");
    REQUIRE(sections.size() == 2);
    CHECK(parsed.text(sections[0].text) == "failed");
    REQUIRE(ends.size() == 4);
    CHECK(ends[0].block == MermaidBlock_Loop);
    CHECK(ends[1].block == MermaidBlock_Alt);
}

TEST_CASE("mermaid sequence: autonumber starts, steps and stops the numbers") {
    Parsed parsed("sequenceDiagram\nautonumber 10 5\nA->>B: x\nautonumber off\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    std::vector<MermaidStep> numbers = parsed.steps(MermaidStepKind_Autonumber);
    REQUIRE(numbers.size() == 2);
    CHECK(numbers[0].start == 10);
    CHECK(numbers[0].step == 5);
    CHECK(numbers[0].visible);
    CHECK_FALSE(numbers[1].visible);
}

TEST_CASE("mermaid sequence: a title comes from the front matter or a statement") {
    Parsed matter("---\ntitle: From matter\n---\nsequenceDiagram\nA->>B: x\n");
    CHECK(matter.text(matter.diagram.title) == "From matter");
    Parsed statement("sequenceDiagram\ntitle: From statement\nA->>B: x\n");
    CHECK(statement.text(statement.diagram.title) == "From statement");
}

TEST_CASE("mermaid sequence: text wraps as its prefix says, or else as the configuration says") {
    Parsed plain("sequenceDiagram\nparticipant A as wrap:Alice\nA->>B: wrap: x\nA->>B: y\nNote over A: nowrap: z\n");
    REQUIRE(plain.status == PrStatus_Ok);
    CHECK(plain.diagram.actors.data[0].wrap);
    CHECK(plain.text(plain.diagram.actors.data[0].label) == "Alice");
    CHECK_FALSE(plain.diagram.actors.data[1].wrap);
    std::vector<MermaidStep> messages = plain.steps(MermaidStepKind_Message);
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].wrap);
    CHECK(plain.text(messages[0].text) == "x");
    CHECK_FALSE(messages[1].wrap);
    CHECK_FALSE(plain.steps(MermaidStepKind_Note)[0].wrap);
    CHECK(plain.text(plain.steps(MermaidStepKind_Note)[0].text) == "z");

    const char* configured[] = {
        "---\nconfig:\n  sequence:\n    wrap: true\n---\nsequenceDiagram\nA->>B: x\n",
        "%%{init: {\"wrap\": true}}%%\nsequenceDiagram\nA->>B: x\n",
        "%%{wrap}%%\nsequenceDiagram\nA->>B: x\n",
    };
    for (const char* source : configured) {
        INFO(source);
        Parsed parsed(source);
        REQUIRE(parsed.status == PrStatus_Ok);
        CHECK(parsed.diagram.actors.data[0].wrap);
        CHECK(parsed.steps(MermaidStepKind_Message)[0].wrap);
    }
    // A key that only starts with wrap is another setting.
    Parsed other("---\nconfig:\n  flowchart:\n    wrappingWidth: 200\n---\nsequenceDiagram\nA->>B: x\n");
    CHECK_FALSE(other.steps(MermaidStepKind_Message)[0].wrap);
}

TEST_CASE("mermaid sequence: the message after create or destroy creates or destroys its participant") {
    Parsed parsed("sequenceDiagram\nA->>B: x\ncreate participant C as Carl\nA->>C: make\ndestroy C\nC->>A: gone\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.actors.count == 3);
    const MermaidActor& carl = parsed.diagram.actors.data[2];
    CHECK(parsed.text(carl.label) == "Carl");
    CHECK(carl.created == 1);
    CHECK(carl.destroyed == 2);
    CHECK(parsed.diagram.actors.data[0].created == kMermaidNone);
    CHECK(parsed.diagram.actors.data[0].destroyed == kMermaidNone);

    const char* failing[] = {
        "sequenceDiagram\nA->>B: x\ncreate participant B\nA->>B: again\n",
        "sequenceDiagram\ncreate participant C\nA->>B: not C\n",
        "sequenceDiagram\nA->>B: x\ndestroy B\nA->>C: not B\n",
    };
    for (const char* source : failing) {
        INFO(source);
        Parsed failed(source);
        CHECK(failed.status == PrStatus_Syntax);
    }
}

TEST_CASE("mermaid sequence: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"sequenceDiagram\nA->>B: ok\nthis is not a statement\n", 3},
        {"sequenceDiagram\nend\n", 2},
        {"sequenceDiagram\nelse nothing\n", 2},
        {"sequenceDiagram\nloop forever\nA->>B: x\n", 2},
        {"sequenceDiagram\nNote above A: x\n", 2},
    };
    for (const Case& c : cases) {
        Parsed parsed(c.source);
        INFO(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid sequence: keywords ignore case, as in mermaid.js") {
    Parsed parsed("sequenceDiagram\nPARTICIPANT A\nActor B\nNote Over A,B: x\nLOOP again\nA->>B: m\nEND\n"
        "AutoNumber\nACTIVATE B\nDeactivate B\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.diagram.actors.count == 2);
    CHECK(parsed.diagram.actors.data[1].kind == MermaidActorKind_Actor);
    CHECK(parsed.steps(MermaidStepKind_Note).size() == 1);
    CHECK(parsed.steps(MermaidStepKind_BlockStart).size() == 1);
    CHECK(parsed.steps(MermaidStepKind_BlockEnd).size() == 1);
    CHECK(parsed.steps(MermaidStepKind_Autonumber).size() == 1);
    CHECK(parsed.steps(MermaidStepKind_Activate).size() == 1);
}

TEST_CASE("mermaid sequence: the body's comments, directives and accessibility statements are not statements") {
    Parsed parsed("sequenceDiagram\n%% A->>B: comment\n%%{init: {\n\"wrap\": true\n}}%%\naccTitle: T\n"
        "accDescr {\nA->>B: described\n}\nA->>B: shown\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    std::vector<MermaidStep> messages = parsed.steps(MermaidStepKind_Message);
    REQUIRE(messages.size() == 1);
    CHECK(parsed.text(messages[0].text) == "shown");
}

TEST_CASE("mermaid sequence: blocks nest at most 64 deep, and at most 64 activations are open at once") {
    std::string nested = "sequenceDiagram\n";
    for (int i = 0; i < 64; ++i)
        nested += "loop\n";
    nested += "A->>B: x\n";
    for (int i = 0; i < 64; ++i)
        nested += "end\n";
    CHECK(Parsed(nested.c_str()).status == PrStatus_Ok);
    std::string deeper = "sequenceDiagram\n";
    for (int i = 0; i < 65; ++i)
        deeper += "opt\n";
    Parsed tooDeep(deeper.c_str());
    CHECK(tooDeep.status == PrStatus_Syntax);
    CHECK(tooDeep.error.line == 66);
    CHECK(tooDeep.error.message == "a block nests too deeply");

    // Activations that end make room for others.
    std::string balanced = "sequenceDiagram\n";
    for (int i = 0; i < 1000; ++i)
        balanced += "A->>+B: x\nB-->>-A: y\n";
    CHECK(Parsed(balanced.c_str()).status == PrStatus_Ok);
    std::string open = "sequenceDiagram\n";
    for (int i = 0; i < 65; ++i)
        open += "activate A\n";
    Parsed tooMany(open.c_str());
    CHECK(tooMany.status == PrStatus_Syntax);
    CHECK(tooMany.error.line == 66);
    CHECK(tooMany.error.message == "activations nest too deeply");
    // The deepest blocks and the most activations that parse draw.
    nested = "sequenceDiagram\n";
    for (int i = 0; i < 64; ++i)
        nested += "alt\nactivate A\n";
    nested += "A->>B: x\n";
    for (int i = 0; i < 64; ++i)
        nested += "else\nA->>B: y\nend\n";
    MermaidDrawn drawn(nested);
    CHECK(drawn.status == PrStatus_Ok);
    CHECK(prF32IsFinite(prMermaidExtent(drawn.diagram).y));
}

TEST_CASE("mermaid sequence: many boxes and sections draw in time linear in their number") {
    std::string source = "sequenceDiagram\n";
    for (int i = 0; i < 1000; ++i) {
        std::string a = "P" + std::to_string(i);
        source += "box Group\nparticipant " + a + "\nend\nalt one\n" + a + "->>" + a + ": x\nelse two\nend\n";
    }
    MermaidDrawn drawn(source);
    REQUIRE(drawn.status == PrStatus_Ok);
    CHECK(prMermaidItemCount(drawn.diagram) > 1000);
}

TEST_CASE("mermaid sequence: each message finds its participants by name among many") {
    std::string source = "sequenceDiagram\n";
    for (int i = 0; i < 20000; ++i)
        source += "P" + std::to_string(i) + "->>P" + std::to_string(i + 1) + ": x\n";
    Parsed parsed(source.c_str());
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.diagram.actors.count == 20001);
    CHECK(parsed.diagram.steps.data[19999].from == 19999);
    CHECK(parsed.diagram.steps.data[19999].to == 20000);
}
