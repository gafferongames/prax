#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_timeline.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <chrono>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed timeline, freed when it goes out of scope.
struct Parsed {
    MermaidTimeline timeline{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidTimelineParse(timeline, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidTimelineReset(timeline);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidTimelineText(timeline, span);
        return std::string(view.data, view.size);
    }

    std::string event(const MermaidTimelinePeriod& period, uint32_t i) const {
        return text(timeline.events.data[period.firstEvent + i]);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid timeline: periods have events, on their line and on the lines after them") {
    Parsed parsed("timeline\n  title History\n  2002 : LinkedIn\n  section Growth\n  2004 : Facebook : Google\n"
        "       : Flickr\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.timeline.title) == "History");
    REQUIRE(parsed.timeline.sections.count == 1);
    CHECK(parsed.text(parsed.timeline.sections.data[0]) == "Growth");
    REQUIRE(parsed.timeline.periods.count == 2);
    const MermaidTimelinePeriod& first = parsed.timeline.periods.data[0];
    CHECK(parsed.text(first.name) == "2002");
    CHECK(first.section == kMermaidNone);
    REQUIRE(first.eventCount == 1);
    CHECK(parsed.event(first, 0) == "LinkedIn");
    const MermaidTimelinePeriod& second = parsed.timeline.periods.data[1];
    CHECK(second.section == 0);
    REQUIRE(second.eventCount == 3);
    CHECK(parsed.event(second, 0) == "Facebook");
    CHECK(parsed.event(second, 1) == "Google");
    CHECK(parsed.event(second, 2) == "Flickr");
}

TEST_CASE("mermaid timeline: a colon without a space in an event stays in the event") {
    Parsed parsed("timeline LR\n2020 : at 10:30 : later\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.timeline.periods.count == 1);
    const MermaidTimelinePeriod& period = parsed.timeline.periods.data[0];
    REQUIRE(period.eventCount == 2);
    CHECK(parsed.event(period, 0) == "at 10:30");
    CHECK(parsed.event(period, 1) == "later");
}

TEST_CASE("mermaid timeline: the title may come from the front matter, and accessibility lines are ignored") {
    Parsed parsed("---\ntitle: Front\n---\ntimeline\naccTitle: A title\naccDescr {\n  a description\n}\n"
        "accDescr: one line\n2020 : Event\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.timeline.title) == "Front");
    CHECK(parsed.timeline.periods.count == 1);
}

TEST_CASE("mermaid timeline: a source that does not parse names where and why") {
    const char* cases[] = {
        "timeline\n: An event before any period\n", "timeline\n2020 :\n", "timeline\n2020 :x\n",
        "timeline\n10:30 : A colon ends a period's name\n",
    };
    for (const char* source : cases) {
        INFO(source);
        Parsed parsed(source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == 2);
    }
    Parsed direction("timeline RL\n");
    CHECK(direction.status == PrStatus_Syntax);
}

TEST_CASE("mermaid timeline: each section spans its periods, and one without periods takes a period's place") {
    MermaidDrawn drawn("timeline\n2000 : Before any section\nsection One\n2001 : a\n2002 : b\nsection Empty\n"
        "section Three\n2003 : c\n");
    REQUIRE(drawn.status == PrStatus_Ok);
    std::vector<PrMermaidItem> groups;
    std::vector<PrMermaidItem> periods;
    for (const PrMermaidItem& item : drawn.items) {
        if (item.kind == PrMermaidItemKind_Group)
            groups.push_back(item);
        else if (item.id == "2000" || item.id == "2001" || item.id == "2002" || item.id == "2003")
            periods.push_back(item);
    }
    // The period before the first section is not drawn.
    REQUIRE(groups.size() == 3);
    REQUIRE(periods.size() == 3);
    CHECK(periods[0].id == "2001");
    CHECK(groups[0].rect.left == doctest::Approx(periods[0].rect.left));
    CHECK(groups[0].rect.right == doctest::Approx(periods[1].rect.right));
    CHECK(groups[1].rect.left - groups[0].rect.left == doctest::Approx(400));
    CHECK(groups[2].rect.left - groups[1].rect.left == doctest::Approx(200));
    CHECK(periods[2].rect.left == doctest::Approx(groups[2].rect.left));
}

TEST_CASE("mermaid timeline: many sections lay out in time linear in the periods") {
    constexpr int32_t kSections = 1000;
    std::string source = "timeline\n";
    for (int32_t i = 0; i < kSections; ++i)
        source += "section S" + std::to_string(i) + "\n" + std::to_string(i) + " : e\n";
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    MermaidDrawn drawn(source);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    CHECK(drawn.status == PrStatus_Ok);
    CHECK(seconds < 10);
}
