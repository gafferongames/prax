// Gantt charts (mermaid_gantt.h). The parser reads the source one line at a time. A statement starts with a keyword, or
// is a task: its name, a colon, and its data, separated by commas. Tasks may name other tasks that come after them, so
// their times are resolved once every task is read.

#include "mermaid/src/mermaid_gantt.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

constexpr PrStringView kMonthNames[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
    "December",
};
constexpr PrStringView kWeekdayNames[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Days since 1970-01-01 of a date in the proleptic Gregorian calendar.
int64_t daysFromCivil(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2 ? 1 : 0;
    int64_t era = floorDiv(year, 400);
    int64_t yearOfEra = year - era * 400;
    int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

void civilFromDays(int64_t days, int32_t* year, int32_t* month, int32_t* day) {
    days += 719468;
    int64_t era = floorDiv(days, 146097);
    int64_t dayOfEra = days - era * 146097;
    int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    int64_t mp = (5 * dayOfYear + 2) / 153;
    *day = int32_t(dayOfYear - (153 * mp + 2) / 5 + 1);
    *month = int32_t(mp < 10 ? mp + 3 : mp - 9);
    *year = int32_t(yearOfEra + era * 400 + (*month <= 2 ? 1 : 0));
}

int32_t daysInMonth(int32_t year, int32_t month) {
    return int32_t(daysFromCivil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) -
        daysFromCivil(year, month, 1));
}

// Reads up to `most` digits at `at`, at least `least` of them.
bool readNumber(PrStringView text, size_t& at, uint32_t least, uint32_t most, int32_t* out) {
    int32_t value = 0;
    uint32_t count = 0;
    while (count < most && at < text.size && prAsciiIsDigit(text.data[at])) {
        value = value * 10 + (text.data[at] - '0');
        ++at;
        ++count;
    }
    *out = value;
    return count >= least;
}

// Reads a month's name, in full or its first three letters, at `at`.
bool readMonthName(PrStringView text, size_t& at, bool full, int32_t* out) {
    for (int32_t m = 0; m < 12; ++m) {
        PrStringView name = full ? kMonthNames[m] : PrStringView{kMonthNames[m].data, 3};
        if (at + name.size <= text.size && prAsciiEqualsFold(PrStringView{text.data + at, name.size}, name)) {
            at += name.size;
            *out = m + 1;
            return true;
        }
    }
    return false;
}

// Reads `text` as a time written in a dayjs format, such as YYYY-MM-DD. Characters that are not tokens match
// themselves.
bool parseFormatted(PrStringView text, PrStringView format, double* out) {
    int32_t year = 1970;
    int32_t month = 1;
    int32_t day = 1;
    int32_t hour = 0;
    int32_t minute = 0;
    int32_t second = 0;
    int32_t millisecond = 0;
    bool pm = false;
    bool twelve = false;
    size_t at = 0;
    for (size_t f = 0; f < format.size;) {
        PrStringView rest = mermaidAfter(format, f);
        bool ok = true;
        if (prStringStartsWith(rest, "YYYY")) {
            ok = readNumber(text, at, 4, 4, &year);
            f += 4;
        } else if (prStringStartsWith(rest, "YY")) {
            ok = readNumber(text, at, 2, 2, &year);
            year += year < 69 ? 2000 : 1900;
            f += 2;
        } else if (prStringStartsWith(rest, "MMMM")) {
            ok = readMonthName(text, at, true, &month);
            f += 4;
        } else if (prStringStartsWith(rest, "MMM")) {
            ok = readMonthName(text, at, false, &month);
            f += 3;
        } else if (prStringStartsWith(rest, "MM")) {
            ok = readNumber(text, at, 2, 2, &month);
            f += 2;
        } else if (prStringStartsWith(rest, "M")) {
            ok = readNumber(text, at, 1, 2, &month);
            f += 1;
        } else if (prStringStartsWith(rest, "DD")) {
            ok = readNumber(text, at, 2, 2, &day);
            f += 2;
        } else if (prStringStartsWith(rest, "Do")) {
            ok = readNumber(text, at, 1, 2, &day);
            at += at + 2 <= text.size ? 2 : 0;
            f += 2;
        } else if (prStringStartsWith(rest, "D")) {
            ok = readNumber(text, at, 1, 2, &day);
            f += 1;
        } else if (prStringStartsWith(rest, "HH") || prStringStartsWith(rest, "hh")) {
            twelve = rest.data[0] == 'h';
            ok = readNumber(text, at, 2, 2, &hour);
            f += 2;
        } else if (prStringStartsWith(rest, "H") || prStringStartsWith(rest, "h")) {
            twelve = rest.data[0] == 'h';
            ok = readNumber(text, at, 1, 2, &hour);
            f += 1;
        } else if (prStringStartsWith(rest, "mm")) {
            ok = readNumber(text, at, 2, 2, &minute);
            f += 2;
        } else if (prStringStartsWith(rest, "m")) {
            ok = readNumber(text, at, 1, 2, &minute);
            f += 1;
        } else if (prStringStartsWith(rest, "ss")) {
            ok = readNumber(text, at, 2, 2, &second);
            f += 2;
        } else if (prStringStartsWith(rest, "s")) {
            ok = readNumber(text, at, 1, 2, &second);
            f += 1;
        } else if (prStringStartsWith(rest, "SSS")) {
            ok = readNumber(text, at, 3, 3, &millisecond);
            f += 3;
        } else if (prStringStartsWith(rest, "A") || prStringStartsWith(rest, "a")) {
            ok = at + 2 <= text.size;
            pm = ok && (text.data[at] | 0x20) == 'p';
            at += 2;
            f += 1;
        } else if (prStringStartsWith(rest, "X") || prStringStartsWith(rest, "x")) {
            double number = 0;
            size_t used = mermaidNumberParse(mermaidAfter(text, at), &number);
            ok = used > 0;
            *out = rest.data[0] == 'X' ? number * 1000 : number;
            return ok && at + used == text.size;
        } else {
            ok = at < text.size && text.data[at] == rest.data[0];
            ++at;
            ++f;
        }
        if (!ok)
            return false;
    }
    if (at != text.size || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month))
        return false;
    if (twelve)
        hour = hour % 12 + (pm ? 12 : 0);
    *out = mermaidTimeOf(year, month, day, hour, minute, second, millisecond);
    return true;
}

// The greatest magnitude of a time, as a JavaScript Date holds it, which keeps the calendar arithmetic in its integers.
constexpr double kMaxTime = 8.64e15;

// Reads a date in the chart's format, or failing that as an ISO date, with an optional time.
bool parseKnownDate(PrStringView text, PrStringView format, double* out) {
    if (parseFormatted(text, format, out))
        return true;
    return parseFormatted(text, "YYYY-MM-DD", out) || parseFormatted(text, "YYYY-MM-DDTHH:mm:ss", out) ||
        parseFormatted(text, "YYYY-MM-DDTHH:mm", out) || parseFormatted(text, "YYYY-MM-DD HH:mm:ss", out) ||
        parseFormatted(text, "YYYY-MM-DD HH:mm", out);
}

// Reads a date as parseKnownDate does, if it is a time that a Date holds.
bool parseDate(PrStringView text, PrStringView format, double* out) {
    return parseKnownDate(prStringTrim(text), format, out) && prF64Abs(*out) <= kMaxTime;
}

// Returns `time` moved on by a duration, such as 3d or 1.5h, and true if `text` is one.
bool addDuration(PrStringView text, double time, double* out) {
    text = prStringTrim(text);
    double amount = 0;
    size_t used = mermaidNumberParse(text, &amount);
    if (used == 0 || amount < 0)
        return false;
    PrStringView unit = mermaidAfter(text, used);
    double step = 0;
    if (unit == "ms")
        step = 1;
    else if (unit == "s")
        step = 1000;
    else if (unit == "m")
        step = 60000;
    else if (unit == "h")
        step = 3600000;
    else if (unit == "d")
        step = kMermaidDay;
    else if (unit == "w")
        step = 7 * kMermaidDay;
    if (step > 0) {
        *out = time + amount * step;
        return prF64Abs(*out) <= kMaxTime;
    }
    // A Date spans fewer months than this, so a duration of more ends past any time.
    double months = unit == "y" ? prF64Floor(amount) * 12 : amount;
    if ((unit == "M" || unit == "y") && months < 4e6) {
        *out = mermaidAddMonths(time, int32_t(months));
        return prF64Abs(*out) <= kMaxTime;
    }
    return false;
}

// A task as written: its data's start and end, which name other tasks or need the chart's date format.
struct RawTask {
    PrStringView start;                 // empty to follow the task before it
    PrStringView end;
    uint32_t line;
};

// Where a task is in the resolution of times: not yet visited, waiting on the tasks that it depends on, or resolved.
enum {
    TaskState_New,
    TaskState_Waiting,
    TaskState_Resolved,
};
typedef uint8_t TaskState;

// A task whose dependencies are being visited, and the next of them to visit.
struct Visit {
    uint32_t task;
    uint32_t next;                      // in Parser::dependencies
};

struct Parser : MermaidParseState {
    MermaidGantt* gantt;
    PrStringView dateFormat;
    bool inclusiveEndDates;
    RawArray<RawTask> raw;              // by task
    RawArray<PrStringView> excludes;    // the excludes statements' items, read once the date format is known

    // The days that excludes skips: a bit for each weekday from Sunday, and the dates on the other weekdays, as days
    // from 1970-01-01, in ascending order and each once.
    uint32_t excludedWeekdays;
    RawArray<int64_t> excludedDates;

    // The tasks by their IDs, which view the source. A later task with the same ID replaces an earlier one, as in
    // mermaid.js.
    MermaidNames ids;

    // The tasks whose times each task's times depend on: the task before it, and the tasks that its `after` and `until`
    // name. Task t's dependencies run from dependencyStart[t] to dependencyStart[t + 1].
    RawArray<uint32_t> dependencyStart; // by task, and one past the last
    RawArray<uint32_t> dependencies;
};

// Calls `fn` with each item of a list separated by commas or white space.
template <typename Fn>
void eachItem(PrStringView list, Fn fn) {
    size_t start = 0;
    for (size_t i = 0; i <= list.size; ++i) {
        if (i == list.size || list.data[i] == ',' || prAsciiIsSpace(list.data[i])) {
            if (i > start)
                fn(PrStringView{list.data + start, i - start});
            start = i + 1;
        }
    }
}

// Calls `fn` with each task that an ID in `list` names. An ID that names no task is skipped.
template <typename Fn>
void eachNamedTask(const Parser& parser, PrStringView list, Fn fn) {
    eachItem(list, [&](PrStringView id) {
        uint32_t task = mermaidNamesFind(parser.ids, id);
        if (task != kMermaidNone)
            fn(task);
    });
}

// The IDs that a task's start names after `after`, or empty.
PrStringView afterList(const RawTask& raw) {
    return prStringStartsWith(raw.start, "after ") ? mermaidAfter(raw.start, 6) : PrStringView{};
}

// The IDs that a task's end names after `until`, or empty.
PrStringView untilList(const RawTask& raw) {
    return prStringStartsWith(raw.end, "until ") ? mermaidAfter(raw.end, 6) : PrStringView{};
}

// Name : [tags,] [[id,] start,] end
bool parseTask(Parser& parser, PrStringView name, PrStringView data, uint32_t section) {
    MermaidGantt& gantt = *parser.gantt;
    PrStringView items[8];
    uint32_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= data.size && count < 8; ++i) {
        if (i == data.size || data.data[i] == ',') {
            items[count++] = prStringTrim(PrStringView{data.data + start, i - start});
            start = i + 1;
        }
    }
    MermaidTask task{.section = section};
    uint32_t first = 0;
    while (first < count) {
        PrStringView tag = items[first];
        if (tag == "done")
            task.flags |= MermaidTaskFlag_Done;
        else if (tag == "active")
            task.flags |= MermaidTaskFlag_Active;
        else if (tag == "crit")
            task.flags |= MermaidTaskFlag_Crit;
        else if (tag == "milestone")
            task.flags |= MermaidTaskFlag_Milestone;
        else if (tag == "vert")
            task.flags |= MermaidTaskFlag_Vert;
        else
            break;
        ++first;
    }
    uint32_t left = count - first;
    RawTask raw{.line = parser.line};
    if (left == 1) {
        raw.end = items[first];
    } else if (left == 2) {
        raw.start = items[first];
        raw.end = items[first + 1];
    } else if (left >= 3) {
        task.id = mermaidTextAdd(gantt.text, items[first]);
        mermaidNamesSet(parser.ids, items[first], gantt.tasks.count);
        raw.start = items[first + 1];
        raw.end = items[first + 2];
    } else {
        return mermaidParseFail(parser, "a task has no end or duration");
    }
    if (raw.start.size == 0 && gantt.tasks.count == 0)
        return mermaidParseFail(parser, "the first task has no start");
    task.label = mermaidTextAdd(gantt.text, prStringTrim(name));
    gantt.tasks.push(task);
    parser.raw.push(raw);
    return true;
}

// The weekday of a day counted from 1970-01-01, which was a Thursday: 0 for Sunday to 6 for Saturday.
uint32_t weekdayOf(int64_t day) {
    return uint32_t(((day + 4) % 7 + 7) % 7);
}

// The number of weekdays that `weekdays`, a bit for each weekday from Sunday, does not have.
int64_t openWeekdays(uint32_t weekdays) {
    int64_t open = 0;
    for (uint32_t d = 0; d < 7; ++d)
        open += (weekdays >> d & 1) == 0 ? 1 : 0;
    return open;
}

// The number of days that are not excluded among the `count` days from `first`. The cost is logarithmic in the excluded
// dates.
int64_t workingDays(const Parser& parser, int64_t first, int64_t count) {
    int64_t weeks = count / 7;
    int64_t working = weeks * openWeekdays(parser.excludedWeekdays);
    for (int64_t day = first + weeks * 7; day < first + count; ++day)
        working += (parser.excludedWeekdays >> weekdayOf(day) & 1) == 0 ? 1 : 0;
    Span<const int64_t> dates = parser.excludedDates.span();
    return working - (int64_t(lowerBound(dates, first + count)) - int64_t(lowerBound(dates, first)));
}

// Extends a task that runs for a duration past the excluded days that it spans, as mermaid.js does. mermaid.js steps a
// day at a time from the start, and moves the end a day later for each excluded day that it steps on. The task so
// covers as many open days as its duration's steps, and the excluded days among them. A binary search on the count of
// open days finds its end, so that a huge duration costs no more than a short one. A week that excludes every day would
// extend a task without end, and so extends none.
double skipExcluded(const Parser& parser, double start, double end) {
    int64_t open = openWeekdays(parser.excludedWeekdays);
    if ((open == 7 && parser.excludedDates.count == 0) || open == 0)
        return end;
    int64_t first = int64_t(prF64Floor(start / kMermaidDay));
    int64_t needed = int64_t(end - start) / int64_t(kMermaidDay) + 1;
    int64_t later = int64_t(parser.excludedDates.count) - int64_t(lowerBound(parser.excludedDates.span(), first));
    int64_t low = needed;
    int64_t high = ((needed + later) / open + 1) * 7;
    while (low < high) {
        int64_t middle = low + (high - low) / 2;
        if (workingDays(parser, first, middle) >= needed)
            high = middle;
        else
            low = middle + 1;
    }
    return end + double(low - needed) * kMermaidDay;
}

// Records the tasks that each task's times depend on.
void findDependencies(Parser& parser) {
    uint32_t count = parser.gantt->tasks.count;
    parser.dependencyStart.resize(count + 1);
    auto depend = [&](uint32_t other) {
        parser.dependencies.push(other);
    };
    for (uint32_t t = 0; t < count; ++t) {
        parser.dependencyStart.data[t] = parser.dependencies.count;
        const RawTask& raw = parser.raw.data[t];
        if (raw.start.size == 0)
            depend(t - 1);
        eachNamedTask(parser, afterList(raw), depend);
        eachNamedTask(parser, untilList(raw), depend);
    }
    parser.dependencyStart.data[count] = parser.dependencies.count;
}

// Resolves a task's times, which needs the times of the tasks that it depends on. Returns false and fails if its data
// is not a time.
bool resolveTask(Parser& parser, uint32_t t) {
    MermaidGantt& gantt = *parser.gantt;
    const RawTask& raw = parser.raw.data[t];
    MermaidTask& task = gantt.tasks.data[t];
    parser.line = raw.line;
    parser.column = 0;
    bool any = false;
    if (raw.start.size == 0) {
        task.start = gantt.tasks.data[t - 1].end;
    } else if (prStringStartsWith(raw.start, "after ")) {
        eachNamedTask(parser, afterList(raw), [&](uint32_t other) {
            double end = gantt.tasks.data[other].end;
            task.start = any ? prF64Max(task.start, end) : end;
            any = true;
        });
        if (!any)
            return mermaidParseFail(parser, "a task starts after no task that the chart has");
    } else if (!parseDate(raw.start, parser.dateFormat, &task.start)) {
        return mermaidParseFail(parser, "a task's start is not a date");
    }
    any = false;
    if (prStringStartsWith(raw.end, "until ")) {
        eachNamedTask(parser, untilList(raw), [&](uint32_t other) {
            double start = gantt.tasks.data[other].start;
            task.end = any ? prF64Min(task.end, start) : start;
            any = true;
        });
        if (!any)
            return mermaidParseFail(parser, "a task runs until no task that the chart has");
    } else if (addDuration(raw.end, task.start, &task.end)) {
        task.end = skipExcluded(parser, task.start, task.end);
        if (!(task.end <= kMaxTime))
            return mermaidParseFail(parser, "a task's end is past the times that a chart holds");
    } else if (parseDate(raw.end, parser.dateFormat, &task.end)) {
        if (parser.inclusiveEndDates)
            task.end += kMermaidDay;
    } else {
        return mermaidParseFail(parser, "a task's end is neither a date nor a duration");
    }
    if (task.end < task.start)
        task.end = task.start;
    return true;
}

// Resolves every task's times, each after the times that it depends on. A depth-first walk starts from each task in
// source order, with a stack of its own so that a long chain of tasks does not recurse. A walk that comes back to a
// task that waits on it has found a cycle, which fails at the task that the walk started from. The cost is linear in
// the tasks and their dependencies.
bool resolveTasks(Parser& parser) {
    uint32_t count = parser.gantt->tasks.count;
    findDependencies(parser);
    RawArray<TaskState> states{};
    states.assignZeroed(count);
    RawArray<Visit> stack{};
    bool ok = true;
    for (uint32_t root = 0; root < count && ok; ++root) {
        if (states.data[root] != TaskState_New)
            continue;
        states.data[root] = TaskState_Waiting;
        stack.push(Visit{root, parser.dependencyStart.data[root]});
        while (stack.count > 0 && ok) {
            Visit& visit = stack.data[stack.count - 1];
            if (visit.next < parser.dependencyStart.data[visit.task + 1]) {
                uint32_t other = parser.dependencies.data[visit.next++];
                if (states.data[other] == TaskState_Waiting) {
                    parser.line = parser.raw.data[root].line;
                    parser.column = 0;
                    ok = mermaidParseFail(parser, "a task's times depend on each other");
                } else if (states.data[other] == TaskState_New) {
                    states.data[other] = TaskState_Waiting;
                    stack.push(Visit{other, parser.dependencyStart.data[other]});
                }
                continue;
            }
            uint32_t task = visit.task;
            stack.pop();
            ok = resolveTask(parser, task);
            states.data[task] = TaskState_Resolved;
        }
    }
    states.reset();
    stack.reset();
    return ok;
}

// Reads the excludes statements' items: weekends, weekday names, and dates in the chart's format. The dates are sorted,
// so that mermaidGanttExcluded finds a day among them by a binary search.
void readExcludes(Parser& parser) {
    MermaidGantt& gantt = *parser.gantt;
    for (PrStringView item : parser.excludes.span()) {
        if (prAsciiEqualsFold(item, "weekends")) {
            gantt.excludeWeekends = true;
            continue;
        }
        bool weekday = false;
        for (int32_t d = 0; d < 7; ++d) {
            if (prAsciiEqualsFold(item, kWeekdayNames[d])) {
                gantt.excludedWeekdays = uint8_t(gantt.excludedWeekdays | (1u << d));
                weekday = true;
            }
        }
        double day = 0;
        if (!weekday && parseDate(item, parser.dateFormat, &day))
            gantt.excludedDays.push(prF64Floor(day / kMermaidDay) * kMermaidDay);
    }
    sortItems(gantt.excludedDays.span(), [](double a, double b) { return a < b; });
    parser.excludedWeekdays = gantt.excludedWeekdays;
    if (gantt.excludeWeekends)
        parser.excludedWeekdays |= (1u << gantt.weekendStart) | (1u << ((gantt.weekendStart + 1) % 7));
    for (double day : gantt.excludedDays.span()) {
        int64_t date = int64_t(day / kMermaidDay);
        bool repeated = parser.excludedDates.count > 0 && parser.excludedDates.data[parser.excludedDates.count - 1] == date;
        if (!repeated && (parser.excludedWeekdays >> weekdayOf(date) & 1) == 0)
            parser.excludedDates.push(date);
    }
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidGantt& gantt = *parser.gantt;
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    if (word == "title") {
        gantt.title = mermaidTextAdd(gantt.text, rest);
        return true;
    }
    if (word == "dateFormat") {
        parser.dateFormat = rest;
        return true;
    }
    if (word == "axisFormat") {
        gantt.axisFormat = mermaidTextAdd(gantt.text, rest);
        return true;
    }
    if (word == "tickInterval") {
        struct Unit {
            PrStringView name;
            MermaidTimeUnit unit;
        };
        static const Unit kUnits[] = {
            {"millisecond", MermaidTimeUnit_Millisecond}, {"second", MermaidTimeUnit_Second},
            {"minute", MermaidTimeUnit_Minute}, {"hour", MermaidTimeUnit_Hour}, {"day", MermaidTimeUnit_Day},
            {"week", MermaidTimeUnit_Week}, {"month", MermaidTimeUnit_Month},
        };
        size_t digits = 0;
        int32_t count = 0;
        readNumber(rest, digits, 1, 9, &count);
        PrStringView name = mermaidAfter(rest, digits);
        for (const Unit& unit : kUnits) {
            if (name == unit.name && count > 0) {
                gantt.tickUnit = unit.unit;
                gantt.tickCount = uint32_t(count);
                return true;
            }
        }
        return mermaidParseFail(parser, "a tick interval is not a count and a unit");
    }
    if (word == "excludes") {
        eachItem(rest, [&](PrStringView item) {
            parser.excludes.push(item);
        });
        return true;
    }
    if (word == "weekday") {
        for (int32_t d = 0; d < 7; ++d) {
            if (prAsciiEqualsFold(rest, kWeekdayNames[d]))
                gantt.weekStart = d;
        }
        return true;
    }
    if (word == "weekend") {
        gantt.weekendStart = prAsciiEqualsFold(rest, "friday") ? 5 : 6;
        return true;
    }
    if (word == "inclusiveEndDates") {
        parser.inclusiveEndDates = true;
        return true;
    }
    if (word == "topAxis") {
        gantt.topAxis = true;
        return true;
    }
    if (word == "section") {
        gantt.sections.push(mermaidTextAdd(gantt.text, rest));
        return true;
    }
    if (word == "includes" || word == "todayMarker" || word == "displayMode" || word == "click")
        return true;
    size_t colon = prStringFindByte(statement, ':');
    if (colon >= statement.size)
        return mermaidParseFail(parser, "unknown statement");
    uint32_t section = gantt.sections.count > 0 ? gantt.sections.count - 1 : kMermaidNone;
    return parseTask(parser, PrStringView{statement.data, colon}, mermaidAfter(statement, colon + 1), section);
}

// Frees the parser's storage.
void parserReset(Parser& parser) {
    mermaidNamesReset(parser.ids);
    parser.raw.reset();
    parser.excludes.reset();
    parser.excludedDates.reset();
    parser.dependencyStart.reset();
    parser.dependencies.reset();
}

} // namespace

MermaidCivilTime mermaidCivilOf(double time) {
    MermaidCivilTime civil{};
    double days = prF64Floor(time / kMermaidDay);
    int64_t day = int64_t(days);
    civilFromDays(day, &civil.year, &civil.month, &civil.day);
    int64_t within = int64_t(time - days * kMermaidDay);
    civil.hour = int32_t(within / 3600000);
    civil.minute = int32_t(within / 60000 % 60);
    civil.second = int32_t(within / 1000 % 60);
    civil.millisecond = int32_t(within % 1000);
    civil.weekday = int32_t(((day + 4) % 7 + 7) % 7);
    civil.yearDay = int32_t(day - daysFromCivil(civil.year, 1, 1) + 1);
    return civil;
}

double mermaidTimeOf(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute, int32_t second,
        int32_t millisecond) {
    double days = double(daysFromCivil(year, month, day));
    return days * kMermaidDay + double(hour) * 3600000 + double(minute) * 60000 + double(second) * 1000 +
        double(millisecond);
}

double mermaidAddMonths(double time, int32_t months) {
    MermaidCivilTime civil = mermaidCivilOf(time);
    int32_t index = civil.year * 12 + (civil.month - 1) + months;
    int32_t year = int32_t(floorDiv(index, 12));
    int32_t month = index - year * 12 + 1;
    int32_t day = civil.day < daysInMonth(year, month) ? civil.day : daysInMonth(year, month);
    return mermaidTimeOf(year, month, day, civil.hour, civil.minute, civil.second, civil.millisecond);
}

void mermaidFormatTime(double time, PrStringView format, RawArray<char>& out) {
    MermaidCivilTime c = mermaidCivilOf(time);
    for (size_t i = 0; i < format.size; ++i) {
        if (format.data[i] != '%' || i + 1 >= format.size) {
            out.push(format.data[i]);
            continue;
        }
        char pad = '0';
        char spec = format.data[++i];
        if ((spec == '-' || spec == '_' || spec == '0') && i + 1 < format.size) {
            pad = spec == '-' ? 0 : spec == '_' ? ' ' : '0';
            spec = format.data[++i];
        }
        auto number = [&](int32_t value, uint32_t width) {
            char digits[12];
            uint32_t count = 0;
            uint32_t v = uint32_t(value < 0 ? -value : value);
            do {
                digits[count++] = char('0' + v % 10);
                v /= 10;
            } while (v > 0);
            if (value < 0)
                out.push('-');
            for (uint32_t k = count; k < width && pad != 0; ++k)
                out.push(pad);
            while (count > 0)
                out.push(digits[--count]);
        };
        switch (spec) {
            case 'Y':
                number(c.year, 4);
                break;
            case 'y':
                number(c.year % 100, 2);
                break;
            case 'm':
                number(c.month, 2);
                break;
            case 'd':
                number(c.day, 2);
                break;
            case 'e':
                pad = pad == '0' ? ' ' : pad;
                number(c.day, 2);
                break;
            case 'H':
                number(c.hour, 2);
                break;
            case 'I':
                number(c.hour % 12 == 0 ? 12 : c.hour % 12, 2);
                break;
            case 'M':
                number(c.minute, 2);
                break;
            case 'S':
                number(c.second, 2);
                break;
            case 'L':
                number(c.millisecond, 3);
                break;
            case 'j':
                number(c.yearDay, 3);
                break;
            case 'w':
                number(c.weekday, 1);
                break;
            case 'p':
                out.append(c.hour < 12 ? "AM" : "PM", 2);
                break;
            case 'a':
                out.append(kWeekdayNames[c.weekday].data, 3);
                break;
            case 'A':
                out.append(kWeekdayNames[c.weekday].data, uint32_t(kWeekdayNames[c.weekday].size));
                break;
            case 'b':
                out.append(kMonthNames[c.month - 1].data, 3);
                break;
            case 'B':
                out.append(kMonthNames[c.month - 1].data, uint32_t(kMonthNames[c.month - 1].size));
                break;
            case '%':
                out.push('%');
                break;
            default:
                out.push('%');
                out.push(spec);
                break;
        }
    }
}

bool mermaidGanttExcluded(const MermaidGantt& gantt, double time) {
    MermaidCivilTime civil = mermaidCivilOf(time);
    if (gantt.excludeWeekends) {
        int32_t first = gantt.weekendStart;
        if (civil.weekday == first || civil.weekday == (first + 1) % 7)
            return true;
    }
    if ((gantt.excludedWeekdays & (1u << civil.weekday)) != 0)
        return true;
    double day = prF64Floor(time / kMermaidDay) * kMermaidDay;
    uint32_t at = lowerBound(gantt.excludedDays.span(), day);
    return at < gantt.excludedDays.count && gantt.excludedDays.data[at] == day;
}

PrStatus mermaidGanttParse(MermaidGantt& gantt, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    gantt.title = MermaidSpan{};
    gantt.axisFormat = MermaidSpan{};
    gantt.tickCount = 0;
    gantt.weekStart = 0;
    gantt.topAxis = false;
    gantt.excludeWeekends = false;
    gantt.weekendStart = 6;
    gantt.excludedWeekdays = 0;
    gantt.excludedDays.clear();
    gantt.text.clear();
    gantt.sections.clear();
    gantt.tasks.clear();
    Parser parser{};
    parser.error = error;
    parser.gantt = &gantt;
    parser.dateFormat = "YYYY-MM-DD";
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_Gantt, nullptr);
    gantt.title = mermaidTextAdd(gantt.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        ok = parseStatement(parser, line.content);
    }
    if (ok) {
        readExcludes(parser);
        ok = resolveTasks(parser);
    }
    parserReset(parser);
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidGanttText(const MermaidGantt& gantt, MermaidSpan span) {
    return mermaidTextAt(gantt.text, span);
}

void mermaidGanttReset(MermaidGantt& gantt) {
    gantt.excludedDays.reset();
    gantt.text.reset();
    gantt.sections.reset();
    gantt.tasks.reset();
}

} // namespace pr_impl
