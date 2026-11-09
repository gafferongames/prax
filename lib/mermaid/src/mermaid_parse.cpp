// Parsing (mermaid_parse.h): the preamble, the body's lines, and what every kind's grammar reads with.

#include "mermaid/src/mermaid_parse.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/hash.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// The keyword of each kind, as its header writes it, with the other keywords that name it. The longest keyword that a
// header starts with names its kind.
struct KindName {
    PrStringView keyword;
    PrMermaidKind kind;
};

constexpr KindName kKindNames[] = {
    {"flowchart", PrMermaidKind_Flowchart},
    {"graph", PrMermaidKind_Flowchart},
    {"flowchart-elk", PrMermaidKind_Flowchart},
    {"sequenceDiagram", PrMermaidKind_Sequence},
    {"classDiagram", PrMermaidKind_Class},
    {"classDiagram-v2", PrMermaidKind_Class},
    {"stateDiagram", PrMermaidKind_State},
    {"stateDiagram-v2", PrMermaidKind_State},
    {"erDiagram", PrMermaidKind_Er},
    {"gantt", PrMermaidKind_Gantt},
    {"pie", PrMermaidKind_Pie},
    {"gitGraph", PrMermaidKind_GitGraph},
    {"mindmap", PrMermaidKind_Mindmap},
    {"timeline", PrMermaidKind_Timeline},
    {"journey", PrMermaidKind_Journey},
    {"quadrantChart", PrMermaidKind_Quadrant},
    {"xychart", PrMermaidKind_XyChart},
    {"xychart-beta", PrMermaidKind_XyChart},
    {"sankey", PrMermaidKind_Sankey},
    {"sankey-beta", PrMermaidKind_Sankey},
    {"radar-beta", PrMermaidKind_Radar},
    {"treemap", PrMermaidKind_Treemap},
    {"treemap-beta", PrMermaidKind_Treemap},
    {"block", PrMermaidKind_Block},
    {"block-beta", PrMermaidKind_Block},
    {"packet", PrMermaidKind_Packet},
    {"packet-beta", PrMermaidKind_Packet},
    {"kanban", PrMermaidKind_Kanban},
    {"requirementDiagram", PrMermaidKind_Requirement},
    {"requirement", PrMermaidKind_Requirement},
    {"C4Context", PrMermaidKind_C4},
    {"C4Container", PrMermaidKind_C4},
    {"C4Component", PrMermaidKind_C4},
    {"C4Dynamic", PrMermaidKind_C4},
    {"C4Deployment", PrMermaidKind_C4},
    {"architecture", PrMermaidKind_Architecture},
    {"architecture-beta", PrMermaidKind_Architecture},
    {"venn-beta", PrMermaidKind_Venn},
    {"usecase-beta", PrMermaidKind_UseCase},
    {"swimlane-beta", PrMermaidKind_Swimlane},
    {"agentflow-beta", PrMermaidKind_Agentflow},
    {"treeView-beta", PrMermaidKind_TreeView},
    {"ishikawa", PrMermaidKind_Ishikawa},
    {"ishikawa-beta", PrMermaidKind_Ishikawa},
    {"wardley-beta", PrMermaidKind_Wardley},
    {"cynefin-beta", PrMermaidKind_Cynefin},
    {"eventmodeling", PrMermaidKind_EventModeling},
    {"railroad-beta", PrMermaidKind_Railroad},
    {"railroad-ebnf-beta", PrMermaidKind_Railroad},
    {"railroad-abnf-beta", PrMermaidKind_Railroad},
    {"railroad-peg-beta", PrMermaidKind_Railroad},
    {"zenuml", PrMermaidKind_ZenUml},
    {"info", PrMermaidKind_Info},
};

// Reads the line at `lines.at` into `out`, and moves past it.
bool nextLine(MermaidLines& lines, MermaidLine* out) {
    PrStringView source = lines.source;
    if (lines.at > source.size)
        return false;
    size_t start = lines.at;
    size_t end = start;
    while (end < source.size && source.data[end] != '\n')
        ++end;
    PrStringView text{source.data + start, end - start};
    if (text.size > 0 && text.data[text.size - 1] == '\r')
        text.size -= 1;
    lines.at = end + 1;
    PrStringView content = prStringTrim(text);
    *out = MermaidLine{
        .number = ++lines.number,
        .text = text,
        .content = content,
        .column = uint32_t(content.data - text.data),
    };
    return true;
}

// Returns true if `content` opens a directive, %%{ ... }%%, that a later line closes.
bool opensDirective(PrStringView content) {
    return prStringStartsWith(content, "%%{") && !prStringContains(mermaidAfter(content, 3), "}%%");
}

// Skips the lines of a directive that `first` opens, through the line that closes it, and returns the directive's text.
PrStringView skipDirective(MermaidLines& lines, const MermaidLine& first) {
    MermaidLine line = first;
    while (!prStringContains(line.content, "}%%") && mermaidLinesExtend(lines, &line)) {
    }
    return line.text;
}

// Reads the theme that a configuration's text names as the scheme that it is drawn in, and the wrap setting.
void readConfiguration(PrStringView text, MermaidPreamble& p) {
    PrStringView value{};
    if (mermaidConfigValue(text, "theme", &value)) {
        p.hasScheme = true;
        p.scheme = prAsciiEqualsFold(value, "dark") ? PrColorScheme_Dark : PrColorScheme_Light;
    }
    if (mermaidConfigValue(text, "wrap", &value)) {
        p.hasWrap = true;
        p.wrap = prAsciiEqualsFold(value, "true");
    } else if (prStringContains(text, "%%{wrap}%%")) {
        p.hasWrap = true;
        p.wrap = true;
    }
}

// Returns true if `content` is an accessibility statement: accTitle or accDescr, followed by a colon, a brace or space.
bool isAccessibility(PrStringView content) {
    if (!prStringStartsWith(content, "accTitle") && !prStringStartsWith(content, "accDescr"))
        return false;
    PrStringView rest = prStringTrim(mermaidAfter(content, 8));
    return rest.size > 0 && (rest.data[0] == ':' || rest.data[0] == '{');
}

} // namespace

bool mermaidParseFail(MermaidParseState& state, const char* message) {
    if (!state.failed) {
        state.failed = true;
        if (state.error != nullptr)
            *state.error = PrMermaidError{state.line, state.column + 1, message};
    }
    return false;
}

MermaidPreamble mermaidPreambleRead(PrStringView source) {
    MermaidPreamble p{};
    MermaidLines lines{.source = source};
    MermaidLine line{};
    bool any = false;
    while (nextLine(lines, &line)) {
        if (line.content.size == 0)
            continue;
        if (!any && line.content == "---") {
            // The front matter runs to the next line of dashes. A source whose front matter does not close has no
            // header.
            size_t start = lines.at;
            bool closed = false;
            while (nextLine(lines, &line)) {
                if (line.content == "---") {
                    closed = true;
                    break;
                }
                PrStringView title{};
                if (prStringStartsWith(line.content, "title:")) {
                    title = prStringTrim(mermaidAfter(line.content, 6));
                    if (title.size >= 2 && (title.data[0] == '"' || title.data[0] == '\'') &&
                            title.data[title.size - 1] == title.data[0])
                        title = PrStringView{title.data + 1, title.size - 2};
                    p.title = title;
                }
            }
            any = true;
            if (!closed)
                return p;
            p.frontMatter = PrStringView{source.data + start, size_t(line.text.data - source.data) - start};
            readConfiguration(p.frontMatter, p);
            continue;
        }
        any = true;
        if (prStringStartsWith(line.content, "%%{")) {
            readConfiguration(skipDirective(lines, line), p);
            continue;
        }
        if (prStringStartsWith(line.content, "%%"))
            continue;
        p.header = line;
        p.body = lines.at;
        return p;
    }
    return p;
}

MermaidLines mermaidLinesOfBody(PrStringView source, const MermaidPreamble& preamble) {
    if (preamble.header.number == 0)
        return MermaidLines{.source = source, .at = source.size + 1, .number = 0};
    return MermaidLines{.source = source, .at = preamble.body, .number = preamble.header.number};
}

bool mermaidLinesNext(MermaidLines& lines, MermaidLine* out) {
    while (nextLine(lines, out)) {
        PrStringView content = out->content;
        if (content.size == 0)
            continue;
        if (opensDirective(content)) {
            skipDirective(lines, *out);
            continue;
        }
        if (prStringStartsWith(content, "%%"))
            continue;
        if (isAccessibility(content)) {
            // A description's block runs to its closing brace.
            size_t colon = prStringFindByte(content, ':');
            size_t brace = prStringFindByte(content, '{');
            if (brace < colon && prStringFindByte(mermaidAfter(content, brace), '}') == content.size - brace) {
                MermaidLine block{};
                while (nextLine(lines, &block) && prStringFindByte(block.content, '}') == block.content.size) {
                }
            }
            continue;
        }
        return true;
    }
    return false;
}

bool mermaidLinesNextRaw(MermaidLines& lines, MermaidLine* out) {
    return nextLine(lines, out);
}

bool mermaidLinesExtend(MermaidLines& lines, MermaidLine* line) {
    MermaidLine next{};
    if (!nextLine(lines, &next))
        return false;
    line->text = PrStringView{line->text.data, size_t(next.text.data + next.text.size - line->text.data)};
    line->content = prStringTrim(line->text);
    line->column = uint32_t(line->content.data - line->text.data);
    return true;
}

bool mermaidKindOfHeader(PrStringView header, PrMermaidKind* kind, PrStringView* PR_NULLABLE rest) {
    size_t end = 0;
    while (end < header.size && !prAsciiIsSpace(header.data[end]) && header.data[end] != ';' && header.data[end] != ':')
        ++end;
    PrStringView word{header.data, end};
    for (const KindName& name : kKindNames) {
        if (prAsciiEqualsFold(word, name.keyword)) {
            *kind = name.kind;
            if (rest != nullptr)
                *rest = prStringTrim(mermaidAfter(header, end));
            return true;
        }
    }
    return false;
}

bool mermaidParseHeader(MermaidParseState& parser, const MermaidPreamble& preamble, PrMermaidKind kind,
        PrStringView* PR_NULLABLE rest) {
    if (rest != nullptr)
        *rest = PrStringView{};
    if (preamble.header.number == 0) {
        parser.line = 1;
        parser.column = 0;
        return mermaidParseFail(parser, "the source names no diagram");
    }
    parser.line = preamble.header.number;
    parser.column = preamble.header.column;
    PrMermaidKind named = PrMermaidKind_Count;
    if (!mermaidKindOfHeader(preamble.header.content, &named, rest) || named != kind)
        return mermaidParseFail(parser, "unknown kind of diagram");
    return true;
}

PrStringView mermaidAfter(PrStringView text, size_t at) {
    return at >= text.size ? PrStringView{text.data + text.size, 0} : PrStringView{text.data + at, text.size - at};
}

PrStringView mermaidBefore(PrStringView text, size_t at) {
    return at >= text.size ? text : PrStringView{text.data, at};
}

void mermaidSkipSpace(PrStringView text, size_t* at) {
    while (*at < text.size && prAsciiIsSpace(text.data[*at]))
        ++*at;
}

bool mermaidStartsWithWord(PrStringView text, PrStringView word) {
    return prStringStartsWith(text, word) && (text.size == word.size || prAsciiIsSpace(text.data[word.size]));
}

void mermaidSplitWord(PrStringView text, PrStringView* word, PrStringView* rest) {
    text = prStringTrim(text);
    size_t end = 0;
    while (end < text.size && !prAsciiIsSpace(text.data[end]))
        ++end;
    *word = PrStringView{text.data, end};
    *rest = prStringTrim(mermaidAfter(text, end));
}

bool mermaidQuotedRead(PrStringView text, PrStringView* quoted, PrStringView* rest) {
    if (text.size == 0 || text.data[0] != '"')
        return false;
    size_t close = prStringFindByte(mermaidAfter(text, 1), '"') + 1;
    if (close >= text.size)
        return false;
    *quoted = PrStringView{text.data + 1, close - 1};
    *rest = prStringTrim(mermaidAfter(text, close + 1));
    return true;
}

PrStringView mermaidUnquoted(PrStringView text) {
    text = prStringTrim(text);
    if (text.size >= 2 && text.data[0] == '"' && text.data[text.size - 1] == '"')
        return PrStringView{text.data + 1, text.size - 2};
    return text;
}

MermaidSpan mermaidTextAdd(RawArray<char>& text, PrStringView added) {
    MermaidSpan span{text.count, uint32_t(added.size)};
    text.append(added.data, uint32_t(added.size));
    return span;
}

PrStringView mermaidTextAt(const RawArray<char>& text, MermaidSpan span) {
    return PrStringView{text.data + span.start, span.size};
}

bool mermaidDirectionParse(PrStringView text, PrGraphDirection* out) {
    if (text == "TB" || text == "TD")
        *out = PrGraphDirection_Down;
    else if (text == "BT")
        *out = PrGraphDirection_Up;
    else if (text == "LR")
        *out = PrGraphDirection_Right;
    else if (text == "RL")
        *out = PrGraphDirection_Left;
    else
        return false;
    return true;
}

size_t mermaidNumberParse(PrStringView text, double* out) {
    double number = 0;
    size_t used = prF64ParsePrefix(text, &number);
    if (used == 0 || !(prF64Abs(number) <= 1e30))
        return 0;
    *out = number;
    return used;
}

bool mermaidLinkIsSafe(PrStringView url) {
    // The scheme's letters, lowered, without the bytes that a browser skips in it. A URL without a scheme, such as a
    // relative one, is safe.
    char scheme[16];
    size_t length = 0;
    for (size_t i = 0; i < url.size; ++i) {
        char c = url.data[i];
        if (uint8_t(c) <= 0x20 || c == 0x7f)
            continue;
        if (c == ':') {
            PrStringView found{scheme, length};
            return found != "javascript" && found != "vbscript" && found != "data";
        }
        bool part = prAsciiIsLetter(c) || (length > 0 && (prAsciiIsDigit(c) || c == '+' || c == '-' || c == '.'));
        if (!part || length == sizeof scheme)
            return true;
        scheme[length++] = prAsciiLower(c);
    }
    return true;
}

bool mermaidConfigValue(PrStringView text, PrStringView key, PrStringView* value) {
    auto separates = [](char c) {
        return c == ':' || c == '"' || c == '\'' || prAsciiIsSpace(c);
    };
    for (size_t at = 0; at + key.size <= text.size; ++at) {
        if (!prMemEquals(text.data + at, key.data, key.size))
            continue;
        bool starts = at == 0 || !(prAsciiIsLetter(text.data[at - 1]) || prAsciiIsDigit(text.data[at - 1]));
        size_t after = at + key.size;
        if (!starts || after >= text.size || !separates(text.data[after]))
            continue;
        size_t start = after;
        while (start < text.size && separates(text.data[start]))
            ++start;
        size_t end = start;
        while (end < text.size && (prAsciiIsLetter(text.data[end]) || prAsciiIsDigit(text.data[end]) || text.data[end] == '-'))
            ++end;
        if (end > start) {
            *value = PrStringView{text.data + start, end - start};
            return true;
        }
    }
    return false;
}

uint32_t mermaidNamesFind(const MermaidNames& names, PrStringView name) {
    uint32_t entry = names.slots.find(uint32_t(prHashBytes(name.data, name.size, 0)), [&](uint32_t at) {
        return names.names.data[at] == name;
    });
    return entry != kHashMissing ? names.values.data[entry] : kMermaidNone;
}

void mermaidNamesSet(MermaidNames& names, PrStringView name, uint32_t value) {
    uint32_t hash = uint32_t(prHashBytes(name.data, name.size, 0));
    uint32_t entry = names.slots.find(hash, [&](uint32_t at) {
        return names.names.data[at] == name;
    });
    if (entry != kHashMissing) {
        names.values.data[entry] = value;
        return;
    }
    names.slots.insert(hash, names.names.count);
    names.names.push(name);
    names.values.push(value);
}

void mermaidNamesReset(MermaidNames& names) {
    names.names.reset();
    names.values.reset();
    names.slots.reset();
}

const MermaidPaint* PR_NULLABLE mermaidStyleSheetClass(const MermaidStyleSheet& sheet, PrStringView name) {
    uint32_t found = mermaidNamesFind(sheet.classNames, name);
    return found != kMermaidNone ? &sheet.classes.data[found] : nullptr;
}

void mermaidStyleSheetDefine(MermaidStyleSheet& sheet, PrStringView name, PrStringView declarations) {
    uint32_t found = mermaidNamesFind(sheet.classNames, name);
    if (found == kMermaidNone) {
        found = sheet.classes.count;
        sheet.classes.push(MermaidPaint{});
        mermaidNamesSet(sheet.classNames, name, found);
    }
    mermaidPaintParse(declarations, sheet.classes.data[found]);
}

void mermaidStyleSheetAssign(MermaidStyleSheet& sheet, uint32_t node, PrStringView name) {
    sheet.assigned.push(MermaidAssignedClass{node, name});
}

void mermaidStyleSheetSetOwn(MermaidStyleSheet& sheet, uint32_t node, PrStringView declarations) {
    while (sheet.own.count <= node)
        sheet.own.push(MermaidPaint{});
    mermaidPaintParse(declarations, sheet.own.data[node]);
}

void mermaidStyleSheetFinish(MermaidStyleSheet& sheet, uint32_t nodeCount) {
    // A counting sort, which keeps each node's classes in the order that they were assigned.
    sheet.assignedStart.assignZeroed(nodeCount + 1);
    for (const MermaidAssignedClass& assigned : sheet.assigned.span()) {
        if (assigned.node < nodeCount)
            ++sheet.assignedStart.data[assigned.node + 1];
    }
    for (uint32_t n = 0; n < nodeCount; ++n)
        sheet.assignedStart.data[n + 1] += sheet.assignedStart.data[n];
    sheet.byNode.resize(sheet.assignedStart.data[nodeCount]);
    RawArray<uint32_t> next{};
    next.resize(nodeCount);
    for (uint32_t n = 0; n < nodeCount; ++n)
        next.data[n] = sheet.assignedStart.data[n];
    for (uint32_t i = 0; i < sheet.assigned.count; ++i) {
        uint32_t node = sheet.assigned.data[i].node;
        if (node < nodeCount)
            sheet.byNode.data[next.data[node]++] = i;
    }
    next.reset();
}

void mermaidStyleSheetResolve(const MermaidStyleSheet& sheet, uint32_t node, bool withDefault, MermaidPaint& style) {
    if (withDefault) {
        if (const MermaidPaint* paint = mermaidStyleSheetClass(sheet, "default"))
            mermaidPaintMerge(style, *paint);
    }
    if (node + 1 < sheet.assignedStart.count) {
        for (uint32_t i = sheet.assignedStart.data[node]; i < sheet.assignedStart.data[node + 1]; ++i) {
            PrStringView name = sheet.assigned.data[sheet.byNode.data[i]].name;
            if (const MermaidPaint* paint = mermaidStyleSheetClass(sheet, name))
                mermaidPaintMerge(style, *paint);
        }
    }
    if (node < sheet.own.count)
        mermaidPaintMerge(style, sheet.own.data[node]);
}

void mermaidStyleSheetReset(MermaidStyleSheet& sheet) {
    mermaidNamesReset(sheet.classNames);
    sheet.classes.reset();
    sheet.assigned.reset();
    sheet.own.reset();
    sheet.byNode.reset();
    sheet.assignedStart.reset();
}

} // namespace pr_impl
