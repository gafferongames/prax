#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/text.h"

#include "mermaid/src/mermaid_gantt.h"
#include "mermaid/tests/mermaid_tests.h"

#include "tools/harness/test.h"

#include <chrono>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A parsed gantt chart, freed when it goes out of scope.
struct Parsed {
    MermaidGantt gantt{};
    PrMermaidError error{};
    PrStatus status = PrStatus_Ok;

    explicit Parsed(const char* source) {
        status = mermaidGanttParse(gantt, source, &error);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    ~Parsed() {
        mermaidGanttReset(gantt);
    }

    std::string text(MermaidSpan span) const {
        PrStringView view = mermaidGanttText(gantt, span);
        return std::string(view.data, view.size);
    }
};

double day(int32_t year, int32_t month, int32_t day) {
    return mermaidTimeOf(year, month, day, 0, 0, 0, 0);
}

std::string format(double time, const char* pattern) {
    RawArray<char> out{};
    mermaidFormatTime(time, pattern, out);
    std::string text(out.data, out.count);
    out.reset();
    return text;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("mermaid gantt: the calendar converts dates, adds months and formats times") {
    double leap = day(2024, 2, 29);
    MermaidCivilTime civil = mermaidCivilOf(leap + 3600000 * 13.5);
    CHECK(civil.year == 2024);
    CHECK(civil.month == 2);
    CHECK(civil.day == 29);
    CHECK(civil.hour == 13);
    CHECK(civil.minute == 30);
    CHECK(civil.weekday == 4);
    CHECK(civil.yearDay == 60);
    CHECK(mermaidCivilOf(day(1969, 12, 31)).day == 31);
    CHECK(mermaidAddMonths(day(2024, 1, 31), 1) == day(2024, 2, 29));
    CHECK(mermaidAddMonths(leap, 12) == day(2025, 2, 28));
    CHECK(format(leap, "%Y-%m-%d") == "2024-02-29");
    CHECK(format(leap, "%e %b %y, %a") == "29 Feb 24, Thu");
    CHECK(format(leap + 3600000 * 15, "%-I %p") == "3 PM");
}

TEST_CASE("mermaid gantt: tasks start at a date, after other tasks, or after the task before them") {
    Parsed parsed("gantt\ntitle Plan\ndateFormat YYYY-MM-DD\n"
        "section One\nA :a1, 2024-01-01, 30d\nB :after a1, 2w\nC :12d\n"
        "section Two\nD :done, crit, d1, 2024-01-05, 2024-01-08\nE :milestone, m1, after c1, 0d\nF :c1, 2024-02-01, 1M\n"
        "G :2023-12-20, until a1\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(parsed.text(parsed.gantt.title) == "Plan");
    REQUIRE(parsed.gantt.tasks.count == 7);
    REQUIRE(parsed.gantt.sections.count == 2);
    const MermaidTask* t = parsed.gantt.tasks.data;
    CHECK(t[0].start == day(2024, 1, 1));
    CHECK(t[0].end == day(2024, 1, 31));
    CHECK(t[1].start == day(2024, 1, 31));
    CHECK(t[1].end == day(2024, 2, 14));
    CHECK(t[2].start == day(2024, 2, 14));
    CHECK(t[2].end == day(2024, 2, 26));
    CHECK(t[3].flags == (MermaidTaskFlag_Done | MermaidTaskFlag_Crit));
    CHECK(t[3].end == day(2024, 1, 8));
    CHECK(t[3].section == 1);
    // E starts after F, which comes later in the source.
    CHECK(t[5].end == day(2024, 3, 1));
    CHECK(t[4].start == day(2024, 3, 1));
    CHECK((t[4].flags & MermaidTaskFlag_Milestone) != 0);
    CHECK(t[6].end == day(2024, 1, 1));
}

TEST_CASE("mermaid gantt: dates follow the chart's format, and excluded days lengthen a task") {
    Parsed parsed("gantt\ndateFormat DD/MM/YYYY HH:mm\nexcludes weekends, 2024-01-10\ninclusiveEndDates\n"
        "A :05/01/2024 09:30, 3d\nB :08/01/2024 00:00, 09/01/2024 00:00\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    const MermaidTask* t = parsed.gantt.tasks.data;
    CHECK(t[0].start == mermaidTimeOf(2024, 1, 5, 9, 30, 0, 0));
    // Friday the 5th, for three days that skip the weekend and the excluded 10th.
    CHECK(t[0].end == mermaidTimeOf(2024, 1, 11, 9, 30, 0, 0));
    CHECK(t[1].end == day(2024, 1, 10));
    CHECK(mermaidGanttExcluded(parsed.gantt, day(2024, 1, 6)));
    CHECK(mermaidGanttExcluded(parsed.gantt, day(2024, 1, 10) + 5000));
    CHECK_FALSE(mermaidGanttExcluded(parsed.gantt, day(2024, 1, 9)));
}

TEST_CASE("mermaid gantt: a source that does not parse names where and why") {
    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"gantt\nA :3d\n", 2},
        {"gantt\nA :2024-13-01, 3d\n", 2},
        {"gantt\nA :2024-01-01, soon\n", 2},
        {"gantt\nA :a, after b, 1d\nB :b, after a, 1d\n", 2},
        {"gantt\nthis is not a task\n", 2},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid gantt: a long chain of tasks that name later tasks resolves in time linear in the chain") {
    // Each task starts after the next one, so that the last task in the source resolves first.
    constexpr int32_t kTasks = 4000;
    std::string source = "gantt\n";
    for (int32_t i = 0; i < kTasks; ++i) {
        std::string start = i + 1 < kTasks ? "after t" + std::to_string(i + 1) : std::string("2024-01-01");
        source += "T :t" + std::to_string(i) + ", " + start + ", 1d\n";
    }
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    Parsed parsed(source.c_str());
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    REQUIRE(parsed.status == PrStatus_Ok);
    CHECK(seconds < 5);
    const MermaidTask* t = parsed.gantt.tasks.data;
    CHECK(t[kTasks - 1].start == day(2024, 1, 1));
    CHECK(t[0].end == day(2024, 1, 1) + kTasks * kMermaidDay);
    CHECK(t[0].start == t[1].end);
}

TEST_CASE("mermaid gantt: a task's ID names the last task that has it, and tasks that name each other fail") {
    Parsed later("gantt\nA :x, 2024-01-01, 1d\nB :x, 2024-02-01, 1d\nC :after x, 1d\n");
    REQUIRE(later.status == PrStatus_Ok);
    CHECK(later.gantt.tasks.data[2].start == day(2024, 2, 2));

    struct Case {
        const char* source;
        uint32_t line;
    };
    const Case cases[] = {
        {"gantt\nA :a, after a, 1d\n", 2},
        {"gantt\nA :a, 2024-01-01, 1d\nB :b, after c, 1d\nC :c, after d, 1d\nD :d, after b, 1d\n", 3},
        {"gantt\nA :a, 2024-01-01, until b\nB :b, after a, 1d\n", 2},
        {"gantt\nA :a, 2024-01-01, 1d\nB :b, after nothing, 1d\n", 3},
        {"gantt\nA :a, 2024-01-01, until nothing\n", 2},
    };
    for (const Case& c : cases) {
        INFO(c.source);
        Parsed parsed(c.source);
        CHECK(parsed.status == PrStatus_Syntax);
        CHECK(parsed.error.line == c.line);
    }
}

TEST_CASE("mermaid gantt: excluded days lengthen a task as a walk a day at a time from its start does") {
    // mermaid.js's walk: each excluded day that it steps on before it passes the end moves the end a day later.
    auto walked = [](const MermaidGantt& gantt, double start, double end) {
        for (double at = start; at <= end; at += kMermaidDay) {
            if (mermaidGanttExcluded(gantt, at))
                end += kMermaidDay;
        }
        return end;
    };
    const char* excludes[] = {
        "excludes weekends",
        "excludes weekends\nweekend friday",
        "excludes monday, wednesday, 2024-01-04, 2024-01-04, 2024-02-29, 2024-01-06",
        "excludes 2024-01-02, 2024-01-03, 2024-01-04, 2024-01-05",
        "excludes sunday, monday, tuesday, wednesday, thursday, friday",
    };
    const char* durations[] = {"0d", "1d", "3d", "36h", "12d", "100d", "6w", "1M"};
    for (const char* exclude : excludes) {
        for (const char* duration : durations) {
            std::string source = std::string("gantt\n") + exclude + "\nA :2024-01-01 09:30, " + duration + "\n" +
                "B :2023-12-31, " + duration + "\n";
            INFO(source);
            Parsed parsed(source.c_str());
            REQUIRE(parsed.status == PrStatus_Ok);
            Parsed plain((std::string("gantt\nA :2024-01-01 09:30, ") + duration + "\nB :2023-12-31, " + duration +
                "\n").c_str());
            for (uint32_t t = 0; t < 2; ++t) {
                const MermaidTask& task = parsed.gantt.tasks.data[t];
                CHECK(task.end == walked(parsed.gantt, task.start, plain.gantt.tasks.data[t].end));
            }
        }
    }
}

TEST_CASE("mermaid gantt: a task whose excluded days push its end past a date fails, in bounded time") {
    // The duration fits in a date, and the weekends that it spans push its end past one.
    Parsed parsed("gantt\nexcludes weekends\nA :2024-01-01, 90000000d\n");
    CHECK(parsed.status == PrStatus_Syntax);
    CHECK(parsed.error.line == 3);
    Parsed fits("gantt\nexcludes weekends, 2024-01-03\nA :2024-01-01, 1000000d\n");
    REQUIRE(fits.status == PrStatus_Ok);
    // Five of each seven days are open, less the excluded Wednesday.
    double days = (fits.gantt.tasks.data[0].end - fits.gantt.tasks.data[0].start) / kMermaidDay;
    CHECK(days == doctest::Approx(1000000.0 * 7 / 5).epsilon(1e-5));
}

TEST_CASE("mermaid gantt: a vert marker is a line across the chart at its time, with its name below the rows") {
    Parsed parsed("gantt\nA :a, 2024-01-01, 10d\nRelease : vert, v1, 2024-01-04, 4m\nFreeze : vert, 2024-01-08, 1d\n"
        "B :after v1, 1d\n");
    REQUIRE(parsed.status == PrStatus_Ok);
    REQUIRE(parsed.gantt.tasks.count == 4);
    const MermaidTask* t = parsed.gantt.tasks.data;
    CHECK(t[1].flags == MermaidTaskFlag_Vert);
    CHECK(parsed.text(t[1].id) == "v1");
    CHECK(t[1].start == day(2024, 1, 4));
    CHECK(t[1].end == day(2024, 1, 4) + 4 * 60000);
    CHECK(t[2].flags == MermaidTaskFlag_Vert);
    CHECK(t[2].id.size == 0);
    CHECK(t[3].start == t[1].end);

    MermaidDrawn drawn("gantt\nA :a, 2024-01-01, 10d\nRelease : vert, v1, 2024-01-04, 4m\nB :b, after a, 10d\n");
    REQUIRE(drawn.status == PrStatus_Ok);
    MermaidDrawn plain("gantt\nA :a, 2024-01-01, 10d\nB :b, after a, 10d\n");
    REQUIRE(plain.status == PrStatus_Ok);
    // The marker takes no row, so the bars and the chart's height are as they are without it.
    CHECK(prMermaidExtent(drawn.diagram).y == doctest::Approx(prMermaidExtent(plain.diagram).y));
    REQUIRE(drawn.items.size() == 3);
    CHECK(drawn.items[1].rect.top == doctest::Approx(plain.items[1].rect.top));
    // The line runs from the top of the grid to two bars' heights past the rows, at the marker's time: three tenths of
    // the way along the twenty days that the chart spans.
    const PrMermaidItem& line = drawn.items[2];
    CHECK(line.id == "v1");
    CHECK(line.rect.top == doctest::Approx(35));
    CHECK(line.rect.bottom == doctest::Approx(35 + 2 * 24 + 2 * 20));
    CHECK(line.rect.left == doctest::Approx(75 + 650 * 3.0f / 20));
    CHECK(line.rect.right - line.rect.left == doctest::Approx(1.6f));
    for (PrColorScheme scheme : {PrColorScheme_Light, PrColorScheme_Dark}) {
        PrDrawList* list = prDrawListCreate();
        prMermaidDraw(drawn.diagram, list, PrVec2{0, 0}, scheme);
        CHECK(prDrawListCommandCount(list) > 0);
        prDrawListRelease(list);
    }
    // A chart of markers alone has no rows.
    MermaidDrawn alone("gantt\nsection S\nRelease : vert, 2024-01-04, 1d\n");
    REQUIRE(alone.status == PrStatus_Ok);
    CHECK(prMermaidExtent(alone.diagram).y == doctest::Approx(100));
}

TEST_CASE("mermaid gantt: an axis's ticks cost no more than the ticks drawn, whatever the domain") {
    const char* sources[] = {
        "gantt\ntickInterval 999999999week\nA :0001-01-01, 2000000d\n",
        "gantt\ntickInterval 999999999month\nA :0001-01-01, 2000000d\n",
        "gantt\ntickInterval 999999999day\nA :0001-01-01, 2000000d\n",
        "gantt\ntickInterval 1day\nA :0001-01-01, 2000000d\n",
        "gantt\ndateFormat YYYY\nA :0001, 9000y\n",
    };
    for (const char* source : sources) {
        INFO(source);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        MermaidDrawn drawn(source);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        CHECK(drawn.status == PrStatus_Ok);
        CHECK(seconds < 5);
    }
}
