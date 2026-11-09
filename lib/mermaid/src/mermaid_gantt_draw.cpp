// Gantt charts as drawn (mermaid_gantt_draw.h). Each task has a row, banded by its section, and a bar from its start to
// its end on a time axis that spans every task. A vert marker has no row, and is a line across the rows at its start.
// The axis's ticks fall on the calendar interval that gives about ten of them, as d3's time scale chooses, unless the
// chart sets its own.

#include "mermaid/src/mermaid_gantt_draw.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_gantt.h"

namespace pr_impl {

namespace {

// mermaid.js's gantt configuration, in px.
constexpr float kWidth = 800;               // the chart's width, which mermaid.js takes from its container
constexpr float kLeftPadding = 75;          // the column of section names
constexpr float kRightPadding = 75;
constexpr float kTopPadding = 50;
constexpr float kBarHeight = 20;
constexpr float kBarGap = 4;
constexpr float kGridTop = 35;              // gridLineStartPadding
constexpr float kFontSize = 11;
constexpr float kAxisFontSize = 10;
constexpr float kTitleFontSize = 18;
constexpr float kTitleBaseline = 25;        // titleTopMargin
constexpr float kRadius = 3;
constexpr float kTextGap = 5;               // between a bar and a name outside it
constexpr float kMilestoneScale = 0.8f;
constexpr uint32_t kMaxTicks = 1000;

// A vert marker: a bar as wide as mermaid.js's 0.08 of a bar's height, and its name, whose baseline is below the rows.
constexpr float kVertWidth = 0.08f * kBarHeight;
constexpr float kVertFontSize = 15;
constexpr float kVertTextDrop = 60;         // from the top of the grid, past the rows

// A tick interval: a count of calendar units.
struct Interval {
    MermaidTimeUnit unit;
    uint32_t count;
    double length;                          // its typical length in milliseconds
};

// d3's time scale intervals, from which it takes the one nearest a tenth of the domain.
constexpr Interval kIntervals[] = {
    {MermaidTimeUnit_Second, 1, 1000},
    {MermaidTimeUnit_Second, 5, 5000},
    {MermaidTimeUnit_Second, 15, 15000},
    {MermaidTimeUnit_Second, 30, 30000},
    {MermaidTimeUnit_Minute, 1, 60000},
    {MermaidTimeUnit_Minute, 5, 300000},
    {MermaidTimeUnit_Minute, 15, 900000},
    {MermaidTimeUnit_Minute, 30, 1800000},
    {MermaidTimeUnit_Hour, 1, 3600000},
    {MermaidTimeUnit_Hour, 3, 10800000},
    {MermaidTimeUnit_Hour, 6, 21600000},
    {MermaidTimeUnit_Hour, 12, 43200000},
    {MermaidTimeUnit_Day, 1, kMermaidDay},
    {MermaidTimeUnit_Day, 2, 2 * kMermaidDay},
    {MermaidTimeUnit_Week, 1, 7 * kMermaidDay},
    {MermaidTimeUnit_Month, 1, 30 * kMermaidDay},
    {MermaidTimeUnit_Month, 3, 90 * kMermaidDay},
    {MermaidTimeUnit_Year, 1, 365 * kMermaidDay},
};

Interval chooseInterval(const MermaidGantt& gantt, double span) {
    if (gantt.tickCount > 0)
        return Interval{gantt.tickUnit, gantt.tickCount, 0};
    double target = span / 10;
    uint32_t count = uint32_t(countOf(kIntervals));
    uint32_t i = 0;
    while (i < count && kIntervals[i].length <= target)
        ++i;
    if (i == 0)
        return kIntervals[0];
    if (i == count)
        return Interval{MermaidTimeUnit_Year, uint32_t(prF64Max(1, prF64Round(target / kIntervals[count - 1].length))), 0};
    return target / kIntervals[i - 1].length < kIntervals[i].length / target ? kIntervals[i - 1] : kIntervals[i];
}

double unitLength(MermaidTimeUnit unit) {
    switch (unit) {
        case MermaidTimeUnit_Millisecond:
            return 1;
        case MermaidTimeUnit_Second:
            return 1000;
        case MermaidTimeUnit_Minute:
            return 60000;
        case MermaidTimeUnit_Hour:
            return 3600000;
        default:
            return kMermaidDay;
    }
}

// Appends the ticks of `interval` from `from` to `to`, as d3 places them: on whole units, and for a count of days,
// months or years, on the units whose number the count divides. Each step goes straight to the next tick, so that the
// cost is in the ticks, of which there are at most kMaxTicks, and not in the units that the domain spans.
void ticks(const MermaidGantt& gantt, Interval interval, double from, double to, RawArray<double>& out) {
    if (interval.unit <= MermaidTimeUnit_Hour) {
        double step = unitLength(interval.unit) * interval.count;
        for (double t = prF64Ceil(from / step) * step; t <= to && out.count < kMaxTicks; t += step)
            out.push(t);
        return;
    }
    double firstDay = prF64Ceil(from / kMermaidDay) * kMermaidDay;
    if (interval.unit == MermaidTimeUnit_Week) {
        // Every count weeks from the first day of a week.
        int32_t ahead = (gantt.weekStart - mermaidCivilOf(firstDay).weekday + 7) % 7;
        double step = 7 * kMermaidDay * double(interval.count);
        for (double t = firstDay + double(ahead) * kMermaidDay; t <= to && out.count < kMaxTicks; t += step)
            out.push(t);
        return;
    }
    if (interval.unit == MermaidTimeUnit_Day) {
        // In each month, the 1st and then every count days.
        for (double t = firstDay; t <= to && out.count < kMaxTicks;) {
            MermaidCivilTime civil = mermaidCivilOf(t);
            uint32_t past = uint32_t(civil.day - 1) % interval.count;
            if (past == 0)
                out.push(t);
            uint32_t ahead = interval.count - past;
            double nextMonth = mermaidAddMonths(mermaidTimeOf(civil.year, civil.month, 1, 0, 0, 0, 0), 1);
            double next = t + double(ahead) * kMermaidDay;
            t = ahead < 31 && next < nextMonth ? next : nextMonth;
        }
        return;
    }
    // In each year, the months whose index from zero the count divides, or the years whose number the count divides.
    bool years = interval.unit == MermaidTimeUnit_Year;
    int64_t count = int64_t(interval.count);
    MermaidCivilTime civil = mermaidCivilOf(from);
    int64_t year = civil.year;
    int64_t month = years ? 0 : civil.month - 1;
    if (mermaidTimeOf(int32_t(year), int32_t(month + 1), 1, 0, 0, 0, 0) < from) {
        if (years)
            ++year;
        else
            ++month;
    }
    // A year past this is later than any time that a chart holds.
    constexpr int64_t kMaxYear = 1000000;
    while (out.count < kMaxTicks) {
        if (years) {
            int64_t past = (year % count + count) % count;
            year += past == 0 ? 0 : count - past;
        } else if (month % count != 0) {
            month += count - month % count;
        }
        if (month >= 12) {
            year += 1;
            month = 0;
        }
        if (year > kMaxYear)
            return;
        double t = mermaidTimeOf(int32_t(year), int32_t(month + 1), 1, 0, 0, 0, 0);
        if (t > to)
            return;
        out.push(t);
        if (years)
            ++year;
        else
            ++month;
    }
}

MermaidLabelIndex label(MermaidRender& render, PrStringView text, float size, bool italic) {
    MermaidTextStyle style = mermaidTextStyle(size);
    style.italic = italic;
    return mermaidLabelCreate(render, text, MermaidTextFormat_Plain, style);
}

void fillRect(MermaidRender& render, PrRect r, MermaidInk ink) {
    mermaidFillRect(render, r, ink);
}

// The fill and stroke of a task's bar, and the color of its name inside the bar.
void taskInks(MermaidTaskFlags flags, MermaidInk* fill, MermaidInk* stroke, MermaidInk* text) {
    bool crit = (flags & MermaidTaskFlag_Crit) != 0;
    bool done = (flags & MermaidTaskFlag_Done) != 0;
    bool active = (flags & MermaidTaskFlag_Active) != 0;
    MermaidRole fillRole = MermaidRole_GanttTaskFill;
    if (active)
        fillRole = MermaidRole_GanttActiveFill;
    else if (done)
        fillRole = MermaidRole_GanttDoneFill;
    else if (crit)
        fillRole = MermaidRole_GanttCritFill;
    MermaidRole strokeRole = MermaidRole_GanttTaskStroke;
    if (crit)
        strokeRole = MermaidRole_GanttCritStroke;
    else if (done)
        strokeRole = MermaidRole_GanttDoneStroke;
    *fill = mermaidInk(fillRole);
    *stroke = mermaidInk(strokeRole);
    *text = mermaidInk(active || done ? MermaidRole_StrongText : MermaidRole_GanttTaskText);
}

} // namespace

PrStatus mermaidGanttDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidGantt gantt{};
    PrStatus status = mermaidGanttParse(gantt, source, error);
    if (status != PrStatus_Ok) {
        mermaidGanttReset(gantt);
        return status;
    }

    // Each task but a vert marker has a row, in source order, and each section's rows follow one another.
    RawArray<uint32_t> taskRows{};      // by task: its row, or kMermaidNone for a vert marker
    RawArray<uint32_t> sectionRows{};   // by section: its first row
    RawArray<uint32_t> sectionCounts{}; // by section: its number of rows
    sectionRows.resize(gantt.sections.count);
    sectionCounts.assignZeroed(gantt.sections.count);
    uint32_t rows = 0;
    for (const MermaidTask& task : gantt.tasks.span()) {
        if ((task.flags & MermaidTaskFlag_Vert) != 0) {
            taskRows.push(kMermaidNone);
            continue;
        }
        if (task.section != kMermaidNone && sectionCounts.data[task.section]++ == 0)
            sectionRows.data[task.section] = rows;
        taskRows.push(rows++);
    }
    float rowHeight = kBarHeight + kBarGap;
    float height = float(rows) * rowHeight + 2 * kTopPadding;
    float axisY = height - kTopPadding;
    render.extent = PrVec2{kWidth, height};

    // The domain runs from the earliest start to the latest end, of every task and marker.
    double first = 0;
    double last = kMermaidDay;
    for (uint32_t t = 0; t < gantt.tasks.count; ++t) {
        const MermaidTask& task = gantt.tasks.data[t];
        first = t == 0 ? task.start : prF64Min(first, task.start);
        last = t == 0 ? task.end : prF64Max(last, task.end);
    }
    if (last <= first)
        last = first + kMermaidDay;
    float chartWidth = kWidth - kLeftPadding - kRightPadding;
    auto xOf = [&](double time) {
        return kLeftPadding + float((time - first) / (last - first)) * chartWidth;
    };

    // The grid's lines and the axis's labels below them, and above them if the chart asks.
    PrStringView axisFormat = gantt.axisFormat.size > 0 ? mermaidGanttText(gantt, gantt.axisFormat) : "%Y-%m-%d";
    RawArray<double> times{};
    ticks(gantt, chooseInterval(gantt, last - first), first, last, times);
    RawArray<char> text{};
    MermaidInk grid = mermaidInk(MermaidRole_GanttGrid);
    MermaidInk ink = mermaidInk(MermaidRole_Text);
    for (double time : times.span()) {
        float x = xOf(time);
        mermaidDrawLine(render, PrVec2{x, kGridTop}, PrVec2{x, axisY}, grid, 1, nullptr);
        text.clear();
        mermaidFormatTime(time, axisFormat, text);
        MermaidLabelIndex tick = label(render, PrStringView{text.data, text.count}, kAxisFontSize, false);
        PrVec2 extent = mermaidLabelExtent(render, tick);
        mermaidLabelDraw(render, tick, PrVec2{x - extent.x * 0.5f, axisY + 3}, ink);
        if (gantt.topAxis)
            mermaidLabelDraw(render, tick, PrVec2{x - extent.x * 0.5f, kGridTop - 3 - extent.y}, ink);
    }

    // The excluded days in the domain, shaded across the rows. A domain of more days than an axis has ticks shades
    // none, since its days are narrower than a pixel.
    bool excludes = gantt.excludeWeekends || gantt.excludedWeekdays != 0 || gantt.excludedDays.count > 0;
    excludes = excludes && last - first <= kMaxTicks * kMermaidDay;
    for (double day = prF64Floor(first / kMermaidDay) * kMermaidDay; excludes && day < last; day += kMermaidDay) {
        if (!mermaidGanttExcluded(gantt, day))
            continue;
        float left = xOf(prF64Max(day, first));
        float right = xOf(prF64Min(day + kMermaidDay, last));
        fillRect(render, PrRect{left, kGridTop, right, axisY}, mermaidInk(MermaidRole_GanttExclude));
    }

    // The rows' bands, colored by their sections in turn.
    for (uint32_t t = 0; t < gantt.tasks.count; ++t) {
        uint32_t row = taskRows.data[t];
        uint32_t section = gantt.tasks.data[t].section;
        uint32_t band = section == kMermaidNone ? 0 : section % 4;
        if (row == kMermaidNone || band == 1 || band == 3)
            continue;
        float top = float(row) * rowHeight + kTopPadding - 2;
        PrRect r{0, top, kWidth - kRightPadding * 0.5f, top + rowHeight};
        fillRect(render, r, mermaidInk(band == 0 ? MermaidRole_GanttBandA : MermaidRole_GanttBandB));
    }

    // The bars, and the tasks' names inside them or beside them.
    for (uint32_t t = 0; t < gantt.tasks.count; ++t) {
        const MermaidTask& task = gantt.tasks.data[t];
        if (taskRows.data[t] == kMermaidNone)
            continue;
        bool milestone = (task.flags & MermaidTaskFlag_Milestone) != 0;
        float top = float(taskRows.data[t]) * rowHeight + kTopPadding;
        float middle = top + kBarHeight * 0.5f;
        float left = xOf(task.start);
        float right = xOf(task.end);
        MermaidInk fill{};
        MermaidInk stroke{};
        MermaidInk inside{};
        taskInks(task.flags, &fill, &stroke, &inside);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .fill = fill,
            .stroke = stroke,
            .strokeWidth = 2,
        };
        PrPath* path = prPathCreate();
        if (milestone) {
            // A square turned on its corner and scaled down, as mermaid.js's style draws a milestone.
            float half = kBarHeight * 0.5f * kMilestoneScale * 1.41421356f;
            prPathMoveTo(path, PrVec2{left, middle - half});
            prPathLineTo(path, PrVec2{left + half, middle});
            prPathLineTo(path, PrVec2{left, middle + half});
            prPathLineTo(path, PrVec2{left - half, middle});
            prPathClose(path);
            right = left + kBarHeight * 0.5f;
            left -= kBarHeight * 0.5f;
        } else {
            PrCornerRect rounded{.rect = PrRect{left, top, right, top + kBarHeight}};
            for (PrCorner& corner : rounded.corners)
                corner = PrCorner{.radii = PrVec2{kRadius, kRadius}, .shape = 1};
            prPathAddCornerRect(path, rounded);
        }
        mermaidDrawPath(render, path, paint);
        prPathDestroy(path);
        MermaidLabelIndex name = label(render, mermaidGanttText(gantt, task.label), kFontSize, milestone);
        PrVec2 extent = mermaidLabelExtent(render, name);
        MermaidInk outside = mermaidInk(MermaidRole_StrongText);
        if (extent.x > right - left) {
            // A name wider than its bar goes after the bar, or before it if it would run past the chart.
            if (right + extent.x + 1.5f * kLeftPadding > kWidth)
                mermaidLabelDraw(render, name, PrVec2{left - kTextGap - extent.x, middle - extent.y * 0.5f}, outside);
            else
                mermaidLabelDraw(render, name, PrVec2{right + kTextGap, middle - extent.y * 0.5f}, outside);
        } else {
            mermaidLabelDrawCentered(render, name, PrVec2{(left + right) * 0.5f, middle}, inside);
        }
        PrStringView id = task.id.size > 0 ? mermaidGanttText(gantt, task.id) : mermaidGanttText(gantt, task.label);
        mermaidItemAdd(render, PrMermaidItemKind_Node, PrRect{left, top, right, top + kBarHeight}, id, PrStringView{});
    }

    // The vert markers, over the bars: a thin bar at each marker's start from the top of the grid to past the rows, and
    // its name below it.
    MermaidInk vert = mermaidInk(MermaidRole_GanttVertLine);
    for (const MermaidTask& task : gantt.tasks.span()) {
        if ((task.flags & MermaidTaskFlag_Vert) == 0)
            continue;
        float x = xOf(task.start);
        PrRect line{x, kGridTop, x + kVertWidth, kGridTop + float(rows) * rowHeight + 2 * kBarHeight};
        MermaidInk fill{};
        MermaidInk stroke{};
        MermaidInk inside{};
        taskInks(task.flags, &fill, &stroke, &inside);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .fill = fill,
            .stroke = vert,
            .strokeWidth = 2,
        };
        PrPath* path = prPathCreate();
        prPathAddRect(path, line);
        mermaidDrawPath(render, path, paint);
        prPathDestroy(path);
        MermaidLabelIndex name = label(render, mermaidGanttText(gantt, task.label), kVertFontSize, false);
        PrVec2 extent = mermaidLabelExtent(render, name);
        float baseline = kGridTop + float(rows) * rowHeight + kVertTextDrop;
        mermaidLabelDraw(render, name, PrVec2{x - extent.x * 0.5f, baseline - extent.y * 0.75f}, vert);
        PrStringView id = task.id.size > 0 ? mermaidGanttText(gantt, task.id) : mermaidGanttText(gantt, task.label);
        mermaidItemAdd(render, PrMermaidItemKind_Node, line, id, PrStringView{});
    }

    // Each section's name, beside the middle of its rows.
    for (uint32_t s = 0; s < gantt.sections.count; ++s) {
        uint32_t count = sectionCounts.data[s];
        if (count == 0)
            continue;
        MermaidLabelIndex name = label(render, mermaidGanttText(gantt, gantt.sections.data[s]), kFontSize, false);
        PrVec2 extent = mermaidLabelExtent(render, name);
        float middle = (float(sectionRows.data[s]) + float(count) * 0.5f) * rowHeight + kTopPadding - 2;
        mermaidLabelDraw(render, name, PrVec2{10, middle - extent.y * 0.5f}, ink);
    }

    if (gantt.title.size > 0) {
        MermaidLabelIndex title = label(render, mermaidGanttText(gantt, gantt.title), kTitleFontSize, false);
        PrVec2 extent = mermaidLabelExtent(render, title);
        mermaidLabelDraw(render, title, PrVec2{(kWidth - extent.x) * 0.5f, kTitleBaseline - extent.y * 0.75f},
            mermaidInk(MermaidRole_Title));
    }
    taskRows.reset();
    sectionRows.reset();
    sectionCounts.reset();
    times.reset();
    text.reset();
    mermaidGanttReset(gantt);
    return PrStatus_Ok;
}

} // namespace pr_impl
