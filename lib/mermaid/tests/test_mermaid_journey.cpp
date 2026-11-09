#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_journey.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <chrono>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed user journey, freed when it goes out of scope.
struct Parsed {
    MermaidJourney journey{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidJourneyParse(journey, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidJourneyReset(journey);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidJourneyText(journey, span);
        return std::string(view.data, view.size);
    }

    std::string actor(const MermaidJourneyTask& task, uint32_t i) const {
        return text(journey.actors.data[task.firstActor + i]);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid journey: tasks have a score and actors, in the section before them") {
    Parsed parsed("journey\n  title My day\n  Wake up: 2\n  section Go to work\n    Make tea: 5: Me\n"
        "    Do work: 1: Me, Cat\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.journey.title) == "My day");
    REQUIRE(parsed.journey.tasks.count == 3);
    const MermaidJourneyTask& wake = parsed.journey.tasks.data[0];
    CHECK(parsed.text(wake.name) == "Wake up");
    CHECK(parsed.text(wake.section).empty());
    CHECK(wake.score == 2);
    CHECK(wake.actorCount == 0);
    const MermaidJourneyTask& work = parsed.journey.tasks.data[2];
    CHECK(parsed.text(work.name) == "Do work");
    CHECK(parsed.text(work.section) == "Go to work");
    CHECK(work.score == 1);
    REQUIRE(work.actorCount == 2);
    CHECK(parsed.actor(work, 0) == "Me");
    CHECK(parsed.actor(work, 1) == "Cat");
}

TEST_CASE("mermaid journey: the title may come from the front matter, and accessibility lines are ignored") {
    Parsed parsed("---\ntitle: \"Front\"\n---\njourney\naccTitle: A title\naccDescr {\n  a description\n}\n"
        "accDescr: one line\nTask: 3: A\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.journey.title) == "Front");
    REQUIRE(parsed.journey.tasks.count == 1);
    CHECK(parsed.journey.tasks.data[0].score == 3);
}

TEST_CASE("mermaid journey: a source that does not parse names where and why") {
    const char* cases[] = {"journey\nTask without a score\n", "journey\n: 3: Me\n", "journey\nTask: high: Me\n"};
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
    }
    Parsed wrong("journeys\n");
    CHECK(wrong.status == PrStatus_Syntax);
}

TEST_CASE("mermaid journey: a comment ends a statement, and a task's actors are listed once each") {
    Parsed parsed("journey\nsection Go # a comment\nTea: 5: Me, , Cat ; a comment\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.journey.tasks.count == 1);
    const MermaidJourneyTask& tea = parsed.journey.tasks.data[0];
    CHECK(parsed.text(tea.section) == "Go");
    REQUIRE(tea.actorCount == 2);
    CHECK(parsed.actor(tea, 1) == "Cat");
}

TEST_CASE("mermaid journey: many actors lay out in time linear in the tasks") {
    constexpr int32_t kTasks = 2000;
    std::string source = "journey\nsection s\n";
    for (int32_t i = 0; i < kTasks; ++i)
        source += "t: 3: a" + std::to_string(i) + ", b" + std::to_string(i % 7) + "\n";
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    MermaidDrawn drawn(source);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    REQUIRE(drawn.status == PrStatus_Ok);
    CHECK(seconds < 10);
    // The legend of every distinct actor, 50 apart, sets the height.
    CHECK(prMermaidExtent(drawn.diagram).y > float(kTasks + 7) * 50);
}
