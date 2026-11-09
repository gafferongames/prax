// Flowcharts (mermaid_flowchart.h). The parser reads the source one line at a time, and semicolons separate the
// statements on a line. A statement is either a chain of nodes joined by links, or a statement that starts with a
// keyword.

#include "mermaid/src/mermaid_flowchart.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/integer.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// A link, as parsed between two groups of nodes.
struct Link {
    MermaidLinkStroke line;
    MermaidHead start;
    MermaidHead end;
    uint32_t length;
    MermaidSpan label;
    MermaidTextFormat format;
};

// The deepest that subgraphs nest. Layout costs grow with depth, and documents nest a few levels.
constexpr uint32_t kMaxNesting = 64;

// The most layers that a link spans. A longer stroke spans as many, so that a source cannot make a diagram of any
// height.
constexpr uint32_t kMaxLinkLength = 16;

struct Parser : MermaidParseState {
    MermaidFlowchart* chart;

    // The statement being parsed, from the column of MermaidParseState, and the parser's place in it.
    const char* data;
    size_t size;
    size_t at;

    RawArray<uint32_t> subgraphs;   // the open subgraphs, innermost last
    RawArray<uint32_t> groups[2];   // the nodes of the two groups that a link joins
    HashIndex nodeIndex;            // the nodes, by the hash of their IDs
    HashIndex edgeIndex;            // the links that the source names, by the hash of their names
    uint32_t autoSubgraphs;

    // The classes and styles of nodes, by node, and of links: the classes that links are given, by link, and whether a
    // linkStyle statement replaced a link's style, which `defaultLinkStyle` replaces otherwise.
    MermaidStyleSheet nodeStyles;
    RawArray<MermaidAssignedClass> edgeClasses;
    RawArray<uint8_t> edgeStyled;
    bool hasDefaultLinkStyle;
    MermaidPaint defaultLinkStyle;
};

// Fails at the parser's place in its statement.
bool fail(Parser& parser, const char* message) {
    parser.column += uint32_t(parser.at);
    return mermaidParseFail(parser, message);
}

// `text` without the double or single quotes around it, as YAML quotes a value.
PrStringView withoutQuotes(PrStringView text) {
    if (text.size >= 2 && (text.data[0] == '"' || text.data[0] == '\'') && text.data[text.size - 1] == text.data[0])
        return PrStringView{text.data + 1, text.size - 2};
    return text;
}

// Adds a label's source without its quotes, and without the backticks of a Markdown string, whose format it writes to
// `format`. The label is decoded when it is drawn.
MermaidSpan addLabel(Parser& parser, PrStringView raw, MermaidTextFormat* format) {
    PrStringView text = prStringTrim(raw);
    if (text.size >= 2 && text.data[0] == '"' && text.data[text.size - 1] == '"')
        text = PrStringView{text.data + 1, text.size - 2};
    *format = MermaidTextFormat_Html;
    if (text.size >= 2 && text.data[0] == '`' && text.data[text.size - 1] == '`') {
        text = PrStringView{text.data + 1, text.size - 2};
        *format = MermaidTextFormat_Markdown;
    }
    return mermaidTextAdd(parser.chart->text, prStringTrim(text));
}

bool isIdChar(char c) {
    switch (c) {
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '<':
        case '>':
        case '|':
        case '&':
        case ';':
        case '"':
        case '\'':
        case '`':
            return false;
        default:
            return !prAsciiIsSpace(c);
    }
}

// Returns true if a node's metadata block, "@{", starts at `at`.
bool shapeDataStartsAt(const Parser& parser, size_t at) {
    return at + 1 < parser.size && parser.data[at] == '@' && parser.data[at + 1] == '{';
}

// Returns true if a link starts at `at`: two dashes, a dash and a dot, two equals signs, or three tildes.
bool linkStartsAt(const Parser& parser, size_t at) {
    if (at + 1 >= parser.size)
        return false;
    char c = parser.data[at];
    char next = parser.data[at + 1];
    if (c == '-')
        return next == '-' || next == '.';
    if (c == '=')
        return next == '=';
    if (c == '~')
        return next == '~' && at + 2 < parser.size && parser.data[at + 2] == '~';
    return false;
}

void skipSpace(Parser& parser) {
    mermaidSkipSpace(PrStringView{parser.data, parser.size}, &parser.at);
}

bool consumeText(Parser& parser, PrStringView text) {
    if (parser.at + text.size > parser.size || !prMemEquals(parser.data + parser.at, text.data, text.size))
        return false;
    parser.at += text.size;
    return true;
}

PrStringView nodeId(const MermaidFlowchart& chart, uint32_t node) {
    return mermaidFlowchartText(chart, chart.nodes.data[node].id);
}

uint32_t nameHash(PrStringView id) {
    return uint32_t(prHashBytes(id.data, id.size, 0));
}

uint32_t findOrAddNode(Parser& parser, PrStringView id) {
    MermaidFlowchart& chart = *parser.chart;
    uint32_t hash = nameHash(id);
    uint32_t found = parser.nodeIndex.find(hash, [&](uint32_t node) {
        PrStringView other = nodeId(chart, node);
        return other.size == id.size && prMemEquals(other.data, id.data, id.size);
    });
    if (found != kHashMissing)
        return found;
    parser.nodeIndex.insert(hash, chart.nodes.count);
    MermaidSpan span = mermaidTextAdd(chart.text, id);
    chart.nodes.push(MermaidNode{
        .id = span,
        .label = span,
        .shape = MermaidShape_Rect,
        .isSubgraph = false,
        .parent = kMermaidNone,
        .direction = PrGraphDirection_Inherit,
    });
    return chart.nodes.count - 1;
}

// Puts a node named by a statement inside a subgraph into that subgraph, unless the node is already in one.
void mention(Parser& parser, uint32_t node) {
    MermaidFlowchart& chart = *parser.chart;
    if (parser.subgraphs.count == 0 || chart.nodes.data[node].parent != kMermaidNone)
        return;
    uint32_t subgraph = parser.subgraphs.last();
    if (!mermaidFlowchartHolds(chart, node, subgraph))
        chart.nodes.data[node].parent = subgraph;
}

// The brackets of each shape, longest first, so that the first match is the right shape.
struct ShapeSyntax {
    PrStringView open;
    PrStringView close;
    MermaidShape shape;
};

constexpr ShapeSyntax kShapes[] = {
    {"(((", ")))", MermaidShape_DoubleCircle},
    {"((", "))", MermaidShape_Circle},
    {"([", "])", MermaidShape_Stadium},
    {"(", ")", MermaidShape_Round},
    {"[[", "]]", MermaidShape_Subroutine},
    {"[(", ")]", MermaidShape_Cylinder},
    {"[/", "/]", MermaidShape_LeanRight},
    {"[/", "\\]", MermaidShape_Trapezoid},
    {"[\\", "\\]", MermaidShape_LeanLeft},
    {"[\\", "/]", MermaidShape_InvTrapezoid},
    {"[", "]", MermaidShape_Rect},
    {"{{", "}}", MermaidShape_Hexagon},
    {"{", "}", MermaidShape_Diamond},
    {">", "]", MermaidShape_Asymmetric},
};

// Parses a node's shape and label, if its brackets follow its name.
bool parseShape(Parser& parser, uint32_t node) {
    PrStringView rest{parser.data + parser.at, parser.size - parser.at};
    if (rest.size == 0 || (rest.data[0] != '(' && rest.data[0] != '[' && rest.data[0] != '{' && rest.data[0] != '>'))
        return true;
    // Picks the first shape whose opening bracket matches. Among the shapes that share that opening, it picks the one
    // whose closing bracket ends the label soonest.
    size_t bestEnd = 0;
    size_t bestLabelEnd = 0;
    const ShapeSyntax* best = nullptr;
    for (const ShapeSyntax& syntax : kShapes) {
        if (best != nullptr && syntax.open.size < best->open.size)
            break;
        if (!prStringStartsWith(rest, syntax.open))
            continue;
        size_t from = syntax.open.size;
        size_t labelEnd = kMermaidNone;
        if (from < rest.size && rest.data[from] == '"') {
            size_t quote = from + 1;
            while (quote < rest.size && rest.data[quote] != '"')
                ++quote;
            if (quote < rest.size &&
                    prStringStartsWith(PrStringView{rest.data + quote + 1, rest.size - quote - 1}, syntax.close))
                labelEnd = quote + 1;
        } else {
            for (size_t i = from; i + syntax.close.size <= rest.size; ++i) {
                if (prMemEquals(rest.data + i, syntax.close.data, syntax.close.size)) {
                    labelEnd = i;
                    break;
                }
            }
        }
        if (labelEnd == kMermaidNone)
            continue;
        if (best == nullptr || labelEnd < bestLabelEnd) {
            best = &syntax;
            bestLabelEnd = labelEnd;
            bestEnd = labelEnd + syntax.close.size;
        }
    }
    if (best == nullptr)
        return fail(parser, "a node's label is not closed");
    MermaidTextFormat format = MermaidTextFormat_Html;
    MermaidSpan label = addLabel(parser, PrStringView{rest.data + best->open.size, bestLabelEnd - best->open.size}, &format);
    MermaidNode& placed = parser.chart->nodes.data[node];
    placed.label = label;
    placed.format = format;
    if (!placed.isSubgraph)
        placed.shape = best->shape;
    parser.at += bestEnd;
    return true;
}

// The names that select a shape in a node's metadata: mermaid.js's short names and their aliases.
struct ShapeName {
    PrStringView name;
    MermaidShape shape;
};

constexpr ShapeName kShapeNames[] = {
    {"rect", MermaidShape_Rect}, {"proc", MermaidShape_Rect}, {"process", MermaidShape_Rect},
    {"rectangle", MermaidShape_Rect}, {"rounded", MermaidShape_Round}, {"event", MermaidShape_Round},
    {"stadium", MermaidShape_Stadium}, {"terminal", MermaidShape_Stadium}, {"pill", MermaidShape_Stadium},
    {"fr-rect", MermaidShape_Subroutine}, {"subprocess", MermaidShape_Subroutine},
    {"subproc", MermaidShape_Subroutine}, {"framed-rectangle", MermaidShape_Subroutine},
    {"subroutine", MermaidShape_Subroutine}, {"cyl", MermaidShape_Cylinder}, {"db", MermaidShape_Cylinder},
    {"database", MermaidShape_Cylinder}, {"cylinder", MermaidShape_Cylinder}, {"datastore", MermaidShape_DataStore},
    {"data-store", MermaidShape_DataStore}, {"folder", MermaidShape_Folder}, {"directory", MermaidShape_Folder},
    {"bucket", MermaidShape_Bucket}, {"console", MermaidShape_Console}, {"browser", MermaidShape_Browser},
    {"person", MermaidShape_Person}, {"circle", MermaidShape_Circle}, {"circ", MermaidShape_Circle},
    {"bang", MermaidShape_Bang}, {"cloud", MermaidShape_Cloud}, {"diam", MermaidShape_Diamond},
    {"decision", MermaidShape_Diamond}, {"diamond", MermaidShape_Diamond}, {"question", MermaidShape_Diamond},
    {"hex", MermaidShape_Hexagon}, {"hexagon", MermaidShape_Hexagon}, {"prepare", MermaidShape_Hexagon},
    {"lean-r", MermaidShape_LeanRight}, {"lean-right", MermaidShape_LeanRight}, {"in-out", MermaidShape_LeanRight},
    {"lean-l", MermaidShape_LeanLeft}, {"lean-left", MermaidShape_LeanLeft}, {"out-in", MermaidShape_LeanLeft},
    {"trap-b", MermaidShape_Trapezoid}, {"priority", MermaidShape_Trapezoid},
    {"trapezoid-bottom", MermaidShape_Trapezoid}, {"trapezoid", MermaidShape_Trapezoid},
    {"trap-t", MermaidShape_InvTrapezoid}, {"manual", MermaidShape_InvTrapezoid},
    {"trapezoid-top", MermaidShape_InvTrapezoid}, {"inv-trapezoid", MermaidShape_InvTrapezoid},
    {"dbl-circ", MermaidShape_DoubleCircle}, {"double-circle", MermaidShape_DoubleCircle},
    {"doublecircle", MermaidShape_DoubleCircle}, {"text", MermaidShape_Text},
    {"notch-rect", MermaidShape_NotchedRect}, {"card", MermaidShape_NotchedRect},
    {"notched-rectangle", MermaidShape_NotchedRect}, {"lin-rect", MermaidShape_LinedRect},
    {"lined-rectangle", MermaidShape_LinedRect}, {"lined-process", MermaidShape_LinedRect},
    {"lin-proc", MermaidShape_LinedRect}, {"shaded-process", MermaidShape_LinedRect},
    {"sm-circ", MermaidShape_SmallCircle}, {"start", MermaidShape_SmallCircle},
    {"small-circle", MermaidShape_SmallCircle}, {"fr-circ", MermaidShape_FramedCircle},
    {"stop", MermaidShape_FramedCircle}, {"framed-circle", MermaidShape_FramedCircle}, {"fork", MermaidShape_Fork},
    {"join", MermaidShape_Fork}, {"hourglass", MermaidShape_Hourglass}, {"collate", MermaidShape_Hourglass},
    {"brace", MermaidShape_BraceLeft}, {"comment", MermaidShape_BraceLeft}, {"brace-l", MermaidShape_BraceLeft},
    {"brace-r", MermaidShape_BraceRight}, {"braces", MermaidShape_Braces}, {"bolt", MermaidShape_Bolt},
    {"com-link", MermaidShape_Bolt}, {"lightning-bolt", MermaidShape_Bolt}, {"doc", MermaidShape_Document},
    {"document", MermaidShape_Document}, {"delay", MermaidShape_Delay},
    {"half-rounded-rectangle", MermaidShape_Delay}, {"h-cyl", MermaidShape_HorizontalCylinder},
    {"das", MermaidShape_HorizontalCylinder}, {"horizontal-cylinder", MermaidShape_HorizontalCylinder},
    {"lin-cyl", MermaidShape_LinedCylinder}, {"disk", MermaidShape_LinedCylinder},
    {"lined-cylinder", MermaidShape_LinedCylinder}, {"curv-trap", MermaidShape_CurvedTrapezoid},
    {"curved-trapezoid", MermaidShape_CurvedTrapezoid}, {"display", MermaidShape_CurvedTrapezoid},
    {"div-rect", MermaidShape_DividedRect}, {"div-proc", MermaidShape_DividedRect},
    {"divided-rectangle", MermaidShape_DividedRect}, {"divided-process", MermaidShape_DividedRect},
    {"tri", MermaidShape_Triangle}, {"extract", MermaidShape_Triangle}, {"triangle", MermaidShape_Triangle},
    {"win-pane", MermaidShape_WindowPane}, {"internal-storage", MermaidShape_WindowPane},
    {"window-pane", MermaidShape_WindowPane}, {"f-circ", MermaidShape_FilledCircle},
    {"junction", MermaidShape_FilledCircle}, {"filled-circle", MermaidShape_FilledCircle},
    {"notch-pent", MermaidShape_NotchedPentagon}, {"loop-limit", MermaidShape_NotchedPentagon},
    {"notched-pentagon", MermaidShape_NotchedPentagon}, {"flip-tri", MermaidShape_FlippedTriangle},
    {"manual-file", MermaidShape_FlippedTriangle}, {"flipped-triangle", MermaidShape_FlippedTriangle},
    {"sl-rect", MermaidShape_SlopedRect}, {"manual-input", MermaidShape_SlopedRect},
    {"sloped-rectangle", MermaidShape_SlopedRect}, {"docs", MermaidShape_StackedDocument},
    {"documents", MermaidShape_StackedDocument}, {"st-doc", MermaidShape_StackedDocument},
    {"stacked-document", MermaidShape_StackedDocument}, {"st-rect", MermaidShape_StackedRect},
    {"procs", MermaidShape_StackedRect}, {"processes", MermaidShape_StackedRect},
    {"stacked-rectangle", MermaidShape_StackedRect}, {"bow-rect", MermaidShape_BowTieRect},
    {"stored-data", MermaidShape_BowTieRect}, {"bow-tie-rectangle", MermaidShape_BowTieRect},
    {"cross-circ", MermaidShape_CrossedCircle}, {"summary", MermaidShape_CrossedCircle},
    {"crossed-circle", MermaidShape_CrossedCircle}, {"tag-doc", MermaidShape_TaggedDocument},
    {"tagged-document", MermaidShape_TaggedDocument}, {"tag-rect", MermaidShape_TaggedRect},
    {"tagged-rectangle", MermaidShape_TaggedRect}, {"tag-proc", MermaidShape_TaggedRect},
    {"tagged-process", MermaidShape_TaggedRect}, {"flag", MermaidShape_PaperTape},
    {"paper-tape", MermaidShape_PaperTape}, {"odd", MermaidShape_Asymmetric},
    {"lin-doc", MermaidShape_LinedDocument}, {"lined-document", MermaidShape_LinedDocument},
};

// Calls `fn` with the key and the value of each entry of a node's metadata, a YAML mapping whose entries commas or line
// breaks outside double quotes separate. An entry without a colon is a key without a value.
template <typename Fn>
bool eachEntry(PrStringView body, Fn fn) {
    size_t start = 0;
    bool quoted = false;
    for (size_t i = 0; i <= body.size; ++i) {
        if (i < body.size) {
            char c = body.data[i];
            if (c == '"')
                quoted = !quoted;
            if (quoted || (c != ',' && c != '\n'))
                continue;
        }
        PrStringView entry = prStringTrim(PrStringView{body.data + start, i - start});
        start = i + 1;
        size_t colon = 0;
        bool keyQuoted = false;
        while (colon < entry.size && (keyQuoted || entry.data[colon] != ':')) {
            keyQuoted = entry.data[colon] == '"' ? !keyQuoted : keyQuoted;
            ++colon;
        }
        if (colon >= entry.size)
            continue;
        PrStringView key = withoutQuotes(prStringTrim(PrStringView{entry.data, colon}));
        PrStringView value = prStringTrim(PrStringView{entry.data + colon + 1, entry.size - colon - 1});
        if (!fn(key, value))
            return false;
    }
    return true;
}

// Parses a node's metadata, if a block of it follows, as in `A@{ shape: cyl, label: "Disk" }`. The block selects the
// node's shape by name and sets its label, which is Markdown. The keys that the engine does not draw, such as icon,
// form, pos, img, w, h and constraint, are accepted and ignored. A subgraph keeps its shape.
bool parseShapeData(Parser& parser, uint32_t node) {
    if (!shapeDataStartsAt(parser, parser.at))
        return true;
    size_t start = parser.at + 2;
    size_t end = start;
    bool quoted = false;
    while (end < parser.size && (quoted || parser.data[end] != '}')) {
        quoted = parser.data[end] == '"' ? !quoted : quoted;
        ++end;
    }
    if (end >= parser.size)
        return fail(parser, "a node's metadata is not closed");
    PrStringView body{parser.data + start, end - start};
    bool valid = eachEntry(body, [&](PrStringView key, PrStringView value) {
        MermaidNode& placed = parser.chart->nodes.data[node];
        if (key == "shape") {
            PrStringView name = withoutQuotes(value);
            for (const ShapeName& known : kShapeNames) {
                if (known.name == name) {
                    if (!placed.isSubgraph)
                        placed.shape = known.shape;
                    return true;
                }
            }
            parser.at = size_t(name.data - parser.data);
            return fail(parser, "unknown shape");
        }
        // An empty label leaves the node's text as it was.
        if (key == "label" && withoutQuotes(value).size > 0) {
            MermaidTextFormat format = MermaidTextFormat_Html;
            placed.label = addLabel(parser, value.data[0] == '\'' ? withoutQuotes(value) : value, &format);
            placed.format = MermaidTextFormat_Markdown;
        }
        return true;
    });
    if (!valid)
        return false;
    parser.at = end + 1;
    return true;
}

// Parses a node with its shape, metadata and classes, and adds it to `group`.
bool parseNode(Parser& parser, RawArray<uint32_t>& group) {
    skipSpace(parser);
    size_t start = parser.at;
    while (parser.at < parser.size && isIdChar(parser.data[parser.at]) && !linkStartsAt(parser, parser.at) &&
            !shapeDataStartsAt(parser, parser.at) &&
            !(parser.data[parser.at] == ':' && parser.at + 2 < parser.size && parser.data[parser.at + 1] == ':' &&
                parser.data[parser.at + 2] == ':'))
        ++parser.at;
    if (parser.at == start)
        return fail(parser, "expected a node");
    uint32_t node = findOrAddNode(parser, PrStringView{parser.data + start, parser.at - start});
    if (!parseShape(parser, node) || !parseShapeData(parser, node))
        return false;
    if (consumeText(parser, ":::")) {
        size_t classStart = parser.at;
        while (parser.at < parser.size && isIdChar(parser.data[parser.at]) && !linkStartsAt(parser, parser.at) &&
                !shapeDataStartsAt(parser, parser.at))
            ++parser.at;
        mermaidStyleSheetAssign(parser.nodeStyles, node,
            PrStringView{parser.data + classStart, parser.at - classStart});
        if (!parseShapeData(parser, node))
            return false;
    }
    mention(parser, node);
    group.push(node);
    return true;
}

bool parseGroup(Parser& parser, RawArray<uint32_t>& group) {
    group.clear();
    for (;;) {
        if (!parseNode(parser, group))
            return false;
        skipSpace(parser);
        if (parser.at >= parser.size || parser.data[parser.at] != '&')
            return true;
        ++parser.at;
    }
}

uint32_t countRun(Parser& parser, char c) {
    uint32_t count = 0;
    while (parser.at < parser.size && parser.data[parser.at] == c) {
        ++parser.at;
        ++count;
    }
    return count;
}

// Parses the mark at a link's end, if one follows its stroke.
MermaidHead parseEndHead(Parser& parser) {
    if (parser.at >= parser.size)
        return MermaidHead_None;
    switch (parser.data[parser.at]) {
        case '>':
            ++parser.at;
            return MermaidHead_Arrow;
        case 'o':
            ++parser.at;
            return MermaidHead_Circle;
        case 'x':
            ++parser.at;
            return MermaidHead_Cross;
        default:
            return MermaidHead_None;
    }
}

// Parses a label written within a link's stroke, as in "-- text -->". The label ends at the closing stroke, which
// starts with two `stroke` characters, or with ".-" if the link is `dotted`.
bool parseInlineLabel(Parser& parser, char stroke, bool dotted, Link& link) {
    size_t start = parser.at;
    size_t close = kMermaidNone;
    for (size_t i = start; i + 1 < parser.size; ++i) {
        bool closes = parser.data[i] == stroke && parser.data[i + 1] == stroke;
        if (dotted)
            closes = parser.data[i] == '.' && parser.data[i + 1] == '-';
        if (closes) {
            close = i;
            break;
        }
    }
    if (close == kMermaidNone)
        return fail(parser, "a link's text is not closed");
    link.label = addLabel(parser, PrStringView{parser.data + start, close - start}, &link.format);
    parser.at = close;
    if (dotted) {
        link.length = countRun(parser, '.');
        ++parser.at;
        link.end = parseEndHead(parser);
        return true;
    }
    uint32_t run = countRun(parser, stroke);
    link.end = parseEndHead(parser);
    link.length = link.end != MermaidHead_None ? run - 1 : run - 2;
    if (link.length < 1)
        link.length = 1;
    return true;
}

bool parseLink(Parser& parser, Link& link) {
    skipSpace(parser);
    link = Link{.line = MermaidLinkStroke_Normal, .length = 1};
    if (parser.at + 1 < parser.size) {
        char c = parser.data[parser.at];
        char next = parser.data[parser.at + 1];
        if ((c == '<' || c == 'o' || c == 'x') && (next == '-' || next == '=' || next == '.')) {
            link.start = c == '<' ? MermaidHead_Arrow : c == 'o' ? MermaidHead_Circle : MermaidHead_Cross;
            ++parser.at;
        }
    }
    if (parser.at >= parser.size)
        return fail(parser, "expected a link");
    char c = parser.data[parser.at];
    if (c == '~') {
        uint32_t run = countRun(parser, '~');
        if (run < 3)
            return fail(parser, "expected a link");
        link.line = MermaidLinkStroke_Invisible;
        link.length = run - 2;
    } else if (c == '-' || c == '=') {
        link.line = c == '=' ? MermaidLinkStroke_Thick : MermaidLinkStroke_Normal;
        uint32_t run = countRun(parser, c);
        if (c == '-' && parser.at < parser.size && parser.data[parser.at] == '.') {
            link.line = MermaidLinkStroke_Dotted;
            uint32_t dots = countRun(parser, '.');
            if (parser.at < parser.size && parser.data[parser.at] == '-') {
                ++parser.at;
                link.end = parseEndHead(parser);
                link.length = dots;
            } else if (run == 1 && dots == 1) {
                if (!parseInlineLabel(parser, '-', true, link))
                    return false;
            } else {
                return fail(parser, "expected a link");
            }
        } else {
            link.end = run >= 2 ? parseEndHead(parser) : MermaidHead_None;
            if (link.end == MermaidHead_None && run == 2) {
                if (!parseInlineLabel(parser, c, false, link))
                    return false;
            } else if ((link.end != MermaidHead_None && run >= 2) || run >= 3) {
                link.length = link.end != MermaidHead_None ? run - 1 : run - 2;
            } else {
                return fail(parser, "expected a link");
            }
        }
    } else {
        return fail(parser, "expected a link");
    }
    skipSpace(parser);
    if (parser.at < parser.size && parser.data[parser.at] == '|') {
        size_t start = ++parser.at;
        while (parser.at < parser.size && parser.data[parser.at] != '|')
            ++parser.at;
        if (parser.at >= parser.size)
            return fail(parser, "a link's text is not closed");
        link.label = addLabel(parser, PrStringView{parser.data + start, parser.at - start}, &link.format);
        ++parser.at;
    }
    link.length = link.length < kMaxLinkLength ? link.length : kMaxLinkLength;
    return true;
}

// Returns the link that the source names `id`, or kMermaidNone.
uint32_t findEdge(const Parser& parser, PrStringView id) {
    const MermaidFlowchart& chart = *parser.chart;
    uint32_t found = parser.edgeIndex.find(nameHash(id), [&](uint32_t edge) {
        return mermaidFlowchartText(chart, chart.edges.data[edge].id) == id;
    });
    return found != kHashMissing ? found : kMermaidNone;
}

// Parses the name that a link is given before its stroke, as in `A e1@--> B`, if one is there. A name is any text
// without white space or quotes, followed by an `@` that no metadata block or quote follows.
PrStringView parseEdgeId(Parser& parser) {
    size_t start = parser.at;
    size_t end = start;
    while (end < parser.size && parser.data[end] != '@' && parser.data[end] != '"' && !prAsciiIsSpace(parser.data[end]))
        ++end;
    if (end == start || end + 1 >= parser.size || parser.data[end] != '@' || parser.data[end + 1] == '{' ||
            parser.data[end + 1] == '"')
        return PrStringView{};
    parser.at = end + 1;
    return PrStringView{parser.data + start, end - start};
}

bool parseChain(Parser& parser) {
    MermaidFlowchart& chart = *parser.chart;
    uint32_t from = 0;
    if (!parseGroup(parser, parser.groups[from]))
        return false;
    for (;;) {
        skipSpace(parser);
        if (parser.at >= parser.size)
            return true;
        PrStringView id = parseEdgeId(parser);
        Link link{};
        if (!parseLink(parser, link))
            return false;
        uint32_t to = 1 - from;
        if (!parseGroup(parser, parser.groups[to]))
            return false;
        // As in mermaid.js, a name goes to the first link that it makes, and only if no link has it yet.
        bool named = id.size > 0 && findEdge(parser, id) == kMermaidNone;
        for (uint32_t i = 0; i < parser.groups[from].count; ++i) {
            for (uint32_t j = 0; j < parser.groups[to].count; ++j) {
                MermaidEdge edge{
                    .source = parser.groups[from].data[i],
                    .target = parser.groups[to].data[j],
                    .label = link.label,
                    .format = link.format,
                    .line = link.line,
                    .start = link.start,
                    .end = link.end,
                    .length = link.length,
                };
                if (named) {
                    edge.id = mermaidTextAdd(chart.text, id);
                    parser.edgeIndex.insert(nameHash(id), chart.edges.count);
                    named = false;
                }
                chart.edges.push(edge);
                parser.edgeStyled.push(0);
            }
        }
        from = to;
    }
}

bool parseSubgraph(Parser& parser, PrStringView rest) {
    MermaidFlowchart& chart = *parser.chart;
    if (parser.subgraphs.count >= kMaxNesting)
        return fail(parser, "subgraphs nest too deeply");
    PrStringView id = rest;
    PrStringView title = rest;
    size_t bracket = prStringFindByte(rest, '[');
    if (rest.size > 0 && rest.data[0] == '"') {
        title = rest;
        id = prStringTrim(PrStringView{rest.data + 1, rest.size >= 2 ? rest.size - 2 : 0});
    } else if (bracket < rest.size) {
        size_t close = rest.size;
        while (close > bracket && rest.data[close - 1] != ']')
            --close;
        if (close <= bracket + 1)
            return fail(parser, "a subgraph's title is not closed");
        id = prStringTrim(PrStringView{rest.data, bracket});
        title = PrStringView{rest.data + bracket + 1, close - bracket - 2};
    }
    // A subgraph without an ID is named as mermaid.js names it.
    char generated[8 + kPrU64TextMaxBytes] = "subGraph";
    if (id.size == 0)
        id = PrStringView{generated, 8 + prU64Format(parser.autoSubgraphs++, generated + 8, 10)};
    uint32_t node = findOrAddNode(parser, id);
    chart.nodes.data[node].isSubgraph = true;
    chart.nodes.data[node].label = addLabel(parser, title, &chart.nodes.data[node].format);
    mention(parser, node);
    parser.subgraphs.push(node);
    return true;
}

// Parses a click statement's arguments for the node `id`: a URL, in quotes or after href, which gives the node its
// link, or a callback, which the engine does not call. A tooltip and a target may follow either.
bool parseClick(Parser& parser, PrStringView rest) {
    MermaidFlowchart& chart = *parser.chart;
    PrStringView id{};
    PrStringView arguments{};
    mermaidSplitWord(rest, &id, &arguments);
    if (id.size == 0)
        return fail(parser, "a click statement names no node");
    PrStringView word{};
    PrStringView after{};
    mermaidSplitWord(arguments, &word, &after);
    if (word == "href")
        arguments = after;
    if (arguments.size == 0 || arguments.data[0] != '"')
        return true;
    PrStringView url{};
    if (!mermaidQuotedRead(arguments, &url, &after))
        return fail(parser, "a click statement's URL is not closed");
    // As in mermaid.js, a click statement names a node that the source has already made, and makes none.
    uint32_t node = parser.nodeIndex.find(nameHash(id), [&](uint32_t n) {
        return nodeId(chart, n) == id;
    });
    if (node != kHashMissing && mermaidLinkIsSafe(url))
        chart.nodes.data[node].link = mermaidTextAdd(chart.text, url);
    return true;
}

// Parses a class statement: a comma-separated list of the nodes and links that it gives the class. As in mermaid.js, a
// name that the source has not given a node or a link names nothing.
void parseClass(Parser& parser, PrStringView rest) {
    MermaidFlowchart& chart = *parser.chart;
    PrStringView ids{};
    PrStringView name{};
    mermaidSplitWord(rest, &ids, &name);
    mermaidEachName(ids, [&](PrStringView id) {
        uint32_t node = parser.nodeIndex.find(nameHash(id), [&](uint32_t n) {
            return nodeId(chart, n) == id;
        });
        if (node != kHashMissing)
            mermaidStyleSheetAssign(parser.nodeStyles, node, name);
        uint32_t edge = findEdge(parser, id);
        if (edge != kMermaidNone)
            parser.edgeClasses.push(MermaidAssignedClass{edge, name});
    });
}

// Parses a linkStyle statement, which replaces the style of each link that it names by its index, or the default style
// of the links that none names. An index past the links made so far is an error, as in mermaid.js.
bool parseLinkStyle(Parser& parser, PrStringView rest) {
    MermaidFlowchart& chart = *parser.chart;
    PrStringView indices{};
    PrStringView declarations{};
    mermaidSplitWord(rest, &indices, &declarations);
    MermaidPaint style{};
    mermaidPaintParse(declarations, style);
    const char* problem = nullptr;
    mermaidEachName(indices, [&](PrStringView index) {
        if (index == "default") {
            parser.hasDefaultLinkStyle = true;
            parser.defaultLinkStyle = style;
            return;
        }
        double number = 0;
        if (mermaidNumberParse(index, &number) != index.size || number < 0 || number != prF64Floor(number)) {
            problem = "a link's index is not a number";
        } else if (number >= double(chart.edges.count)) {
            problem = "a link's index is past the last link";
        } else {
            chart.edges.data[uint32_t(number)].style = style;
            parser.edgeStyled.data[uint32_t(number)] = 1;
        }
    });
    return problem == nullptr || fail(parser, problem);
}

bool parseStatement(Parser& parser) {
    PrStringView statement{parser.data, parser.size};
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    MermaidFlowchart& chart = *parser.chart;
    // Keywords are lowercase, as mermaid.js reads them, so that `End` names a node.
    if (word == "subgraph")
        return parseSubgraph(parser, rest);
    if (word == "end" && rest.size == 0) {
        if (parser.subgraphs.count == 0)
            return fail(parser, "end closes no subgraph");
        parser.subgraphs.pop();
        return true;
    }
    if (word == "direction") {
        PrGraphDirection direction = PrGraphDirection_Down;
        if (!mermaidDirectionParse(rest, &direction))
            return fail(parser, "unknown direction");
        if (parser.subgraphs.count > 0)
            chart.nodes.data[parser.subgraphs.last()].direction = direction;
        else
            chart.direction = direction;
        return true;
    }
    if (word == "classDef") {
        PrStringView names{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &names, &declarations);
        mermaidEachName(names, [&](PrStringView name) {
            mermaidStyleSheetDefine(parser.nodeStyles, name, declarations);
        });
        return true;
    }
    if (word == "class") {
        parseClass(parser, rest);
        return true;
    }
    if (word == "style") {
        PrStringView id{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &id, &declarations);
        mermaidStyleSheetSetOwn(parser.nodeStyles, findOrAddNode(parser, id), declarations);
        return true;
    }
    if (word == "linkStyle")
        return parseLinkStyle(parser, rest);
    if (word == "click")
        return parseClick(parser, rest);
    // A link's metadata, as in `e1@{ animate: true }`, sets only what the engine does not draw.
    size_t at = prStringFind(statement, "@{");
    if (at < statement.size && findEdge(parser, PrStringView{statement.data, at}) != kMermaidNone) {
        parser.at = at;
        size_t close = prStringFindLastByte(statement, '}');
        if (close < at || close + 1 != statement.size)
            return fail(parser, "a link's metadata is not closed");
        return true;
    }
    return parseChain(parser);
}

// Calls `fn` with each statement of a line. Semicolons outside quotes and brackets separate the statements.
template <typename Fn>
bool eachStatement(PrStringView line, Fn fn) {
    size_t start = 0;
    uint32_t depth = 0;
    bool quoted = false;
    for (size_t i = 0; i <= line.size; ++i) {
        if (i < line.size) {
            char c = line.data[i];
            if (c == '"')
                quoted = !quoted;
            else if (!quoted && (c == '[' || c == '(' || c == '{'))
                ++depth;
            else if (!quoted && (c == ']' || c == ')' || c == '}') && depth > 0)
                --depth;
            if (c != ';' || quoted || depth > 0)
                continue;
        }
        if (!fn(PrStringView{line.data + start, i - start}))
            return false;
        start = i + 1;
    }
    return true;
}

// Whether a statement's text leaves a double quote or a node's metadata block, "@{", open, read a byte at a time.
struct Openness {
    bool quoted;
    bool inMetadata;
};

void scanOpenness(Openness& open, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '"')
            open.quoted = !open.quoted;
        else if (!open.quoted && !open.inMetadata && c == '@' && i + 1 < text.size && text.data[i + 1] == '{')
            open.inMetadata = true;
        else if (!open.quoted && open.inMetadata && c == '}')
            open.inMetadata = false;
    }
}

// Parses the statements of `line` from the offset `from` of its text. A quoted label may go on past its line, as a
// Markdown string's lines do, and so may a node's metadata block, so a line whose quote or block is open runs on to the
// line that closes it. Errors name the line that the statements start on.
bool parseLine(Parser& parser, MermaidLines& lines, MermaidLine line, size_t from) {
    parser.line = line.number;
    Openness open{};
    scanOpenness(open, mermaidAfter(line.text, from));
    size_t scanned = line.text.size;
    while ((open.quoted || open.inMetadata) && mermaidLinesExtend(lines, &line)) {
        scanOpenness(open, mermaidAfter(line.text, scanned));
        scanned = line.text.size;
    }
    return eachStatement(mermaidAfter(line.text, from), [&](PrStringView statement) {
        PrStringView trimmed = prStringTrim(statement);
        if (trimmed.size == 0)
            return true;
        parser.data = trimmed.data;
        parser.size = trimmed.size;
        parser.at = 0;
        parser.column = uint32_t(trimmed.data - line.text.data);
        return parseStatement(parser);
    });
}

// Gives each node its style, and each link its style: its classes, then its linkStyle or else the default linkStyle.
void resolveStyles(Parser& parser) {
    MermaidFlowchart& chart = *parser.chart;
    mermaidStyleSheetFinish(parser.nodeStyles, chart.nodes.count);
    for (uint32_t n = 0; n < chart.nodes.count; ++n) {
        chart.nodes.data[n].style = MermaidPaint{};
        mermaidStyleSheetResolve(parser.nodeStyles, n, true, chart.nodes.data[n].style);
    }
    RawArray<MermaidPaint> base{};
    base.assignZeroed(chart.edges.count);
    for (const MermaidAssignedClass& assigned : parser.edgeClasses.span()) {
        if (const MermaidPaint* paint = mermaidStyleSheetClass(parser.nodeStyles, assigned.name))
            mermaidPaintMerge(base.data[assigned.node], *paint);
    }
    for (uint32_t e = 0; e < chart.edges.count; ++e) {
        MermaidEdge& edge = chart.edges.data[e];
        if (parser.edgeStyled.data[e] != 0)
            mermaidPaintMerge(base.data[e], edge.style);
        else if (parser.hasDefaultLinkStyle)
            mermaidPaintMerge(base.data[e], parser.defaultLinkStyle);
        edge.style = base.data[e];
    }
    base.reset();
}

void resetParser(Parser& parser) {
    parser.subgraphs.reset();
    parser.groups[0].reset();
    parser.groups[1].reset();
    parser.nodeIndex.reset();
    parser.edgeIndex.reset();
    mermaidStyleSheetReset(parser.nodeStyles);
    parser.edgeClasses.reset();
    parser.edgeStyled.reset();
}

} // namespace

PrStatus mermaidFlowchartParse(MermaidFlowchart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    chart.direction = PrGraphDirection_Down;
    chart.title = MermaidSpan{};
    chart.text.clear();
    chart.nodes.clear();
    chart.edges.clear();
    Parser parser{};
    parser.chart = &chart;
    parser.error = error;
    MermaidPreamble preamble = mermaidPreambleRead(source);
    if (preamble.title.size > 0)
        chart.title = mermaidTextAdd(chart.text, preamble.title);
    // The header: its keyword, then a direction, then any statements, after a semicolon.
    const MermaidLine& header = preamble.header;
    PrStringView rest{};
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Flowchart, &rest))
        return PrStatus_Syntax;
    size_t directionEnd = 0;
    while (directionEnd < rest.size && !prAsciiIsSpace(rest.data[directionEnd]) && rest.data[directionEnd] != ';')
        ++directionEnd;
    if (directionEnd > 0 && mermaidDirectionParse(PrStringView{rest.data, directionEnd}, &chart.direction))
        rest = prStringTrim(mermaidAfter(rest, directionEnd));
    if (rest.size > 0 && rest.data[0] == ';')
        rest = mermaidAfter(rest, 1);

    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    bool parsed = parseLine(parser, lines, header, size_t(rest.data - header.text.data));
    MermaidLine line{};
    while (parsed && mermaidLinesNext(lines, &line))
        parsed = parseLine(parser, lines, line, 0);
    if (parsed && parser.subgraphs.count > 0) {
        parser.at = 0;
        parsed = fail(parser, "a subgraph is not closed");
    }
    if (parsed)
        resolveStyles(parser);
    resetParser(parser);
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidFlowchartText(const MermaidFlowchart& chart, MermaidSpan span) {
    return mermaidTextAt(chart.text, span);
}

bool mermaidFlowchartHolds(const MermaidFlowchart& chart, uint32_t ancestor, uint32_t node) {
    for (uint32_t n = node; n != kMermaidNone; n = chart.nodes.data[n].parent) {
        if (n == ancestor)
            return true;
    }
    return false;
}

void mermaidFlowchartReset(MermaidFlowchart& chart) {
    chart.text.reset();
    chart.nodes.reset();
    chart.edges.reset();
}

} // namespace pr_impl
