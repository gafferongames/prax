// Gantt charts: the model of a Mermaid gantt chart, the parser that builds it from the source of `gantt`, and the
// calendar that its dates are in.
//
// A time is a count of milliseconds since 1970-01-01 00:00 on a calendar without time zones, so that a date in the
// source means the same time everywhere.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The length of a day, in milliseconds.
constexpr double kMermaidDay = 86400000;

// A time split into its calendar fields.
struct MermaidCivilTime {
    int32_t year;
    int32_t month;                      // 1 to 12
    int32_t day;                        // 1 to 31
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t millisecond;
    int32_t weekday;                    // 0 for Sunday to 6 for Saturday
    int32_t yearDay;                    // 1 to 366
};

// Converts a time to its calendar fields, and calendar fields to a time.
MermaidCivilTime mermaidCivilOf(double time);
double mermaidTimeOf(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second,
    int32_t millisecond);

// Returns `time` moved by whole months, keeping its day where the new month has it and its last day otherwise.
double mermaidAddMonths(double time, int32_t months);

// Appends `time` written in a d3 time format, such as %Y-%m-%d.
void mermaidFormatTime(double time, PrStringView format, RawArray<char>& out);

// The intervals that an axis's ticks may fall on.
enum {
    MermaidTimeUnit_Millisecond,
    MermaidTimeUnit_Second,
    MermaidTimeUnit_Minute,
    MermaidTimeUnit_Hour,
    MermaidTimeUnit_Day,
    MermaidTimeUnit_Week,
    MermaidTimeUnit_Month,
    MermaidTimeUnit_Year,
};
typedef uint8_t MermaidTimeUnit;

// A task's state, which colors its bar, and its kind of mark.
enum {
    MermaidTaskFlag_Done = 1u << 0,
    MermaidTaskFlag_Active = 1u << 1,
    MermaidTaskFlag_Crit = 1u << 2,
    MermaidTaskFlag_Milestone = 1u << 3,
    MermaidTaskFlag_Vert = 1u << 4,     // a vertical line across the chart at its start, which takes no row
};
typedef uint8_t MermaidTaskFlags;

// A task, in source order, with its times resolved.
struct MermaidTask {
    MermaidSpan id;                     // the ID that the source gives it, or empty
    MermaidSpan label;
    uint32_t section;                   // kMermaidNone for a task before any section
    double start;
    double end;
    MermaidTaskFlags flags;
};

// A gantt chart, as its source describes it.
struct MermaidGantt {
    MermaidSpan title;
    MermaidSpan axisFormat;             // a d3 time format, or empty for %Y-%m-%d
    MermaidTimeUnit tickUnit;           // the tickInterval statement's unit, if tickCount is not zero
    uint32_t tickCount;
    int32_t weekStart;                  // the weekday that week ticks fall on: 0 for Sunday
    bool topAxis;                       // the axis is drawn above the tasks as well as below them

    // The days that excludes skips, which the chart shades: weekends, other weekdays, and dates.
    bool excludeWeekends;
    int32_t weekendStart;               // 5 for a Friday and Saturday weekend, 6 for Saturday and Sunday
    uint8_t excludedWeekdays;           // a bit for each weekday, Sunday first
    RawArray<double> excludedDays;      // the start of each excluded date, in ascending order

    RawArray<char> text;                // the characters of every span
    RawArray<MermaidSpan> sections;
    RawArray<MermaidTask> tasks;
};

// Returns true if the day that holds `time` is excluded.
bool mermaidGanttExcluded(const MermaidGantt& gantt, double time);

// Parses the source of a gantt chart into `gantt`, emptying it first. If the source does not parse, returns Syntax and
// fills a non-null `error`.
PrStatus mermaidGanttParse(MermaidGantt& gantt, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the chart.
PrStringView mermaidGanttText(const MermaidGantt& gantt, MermaidSpan span);

// Frees the chart's storage.
void mermaidGanttReset(MermaidGantt& gantt);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
