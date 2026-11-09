// State diagrams (mermaid_state.h). The parser reads the source one line at a time. A statement starts with a keyword,
// adds a description to a state with a colon after its name, or is a transition between two states. A composite state's
// body runs from the brace that opens it to the brace that closes it, and -- divides it into concurrent regions. Each
// composite state and region has its own start and end, which [*] names. Keywords ignore case, as in mermaid.js.

#include "mermaid/src/mermaid_state.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// The deepest that composite states nest. The layout reads the composite states around each node.
constexpr uint32_t kMaxStateDepth = 64;

// An open composite state, and the region of it that statements add to, which is the state itself until -- divides it.
struct Scope {
    uint32_t composite;
    uint32_t current;
    uint32_t line;                      // the line that opens the composite state
};

// What the parser knows of a node as a scope, and the descriptions that it collects for it.
struct NodeRecord {
    // The start and the end that [*] names in the node's scope, or kMermaidNone.
    uint32_t start;
    uint32_t end;

    // A composite state's first and last regions, or kMermaidNone until -- divides it.
    uint32_t firstRegion;
    uint32_t lastRegion;

    // The node's first and last descriptions, in Parser::descriptions, or kMermaidNone.
    uint32_t firstDescription;
    uint32_t lastDescription;
};

// A line of a state's description, which views the source.
struct Description {
    PrStringView text;
    uint32_t next;                      // the state's next description, or kMermaidNone
};

struct Parser : MermaidParseState {
    MermaidStateDiagram* diagram;
    MermaidNames stateIds;                  // the node of each state's ID, which views the source
    MermaidStyleSheet styles;               // by node
    RawArray<Scope> scopes;                 // the open composite states, innermost last
    RawArray<NodeRecord> records;           // by node
    NodeRecord root;                        // the diagram's own scope
    RawArray<Description> descriptions;

    // A note that runs over several lines, until `end note`, or kMermaidNone.
    uint32_t note;
    uint32_t noteLine;
    RawArray<char> noteText;
};

constexpr NodeRecord kEmptyRecord = {
    .start = kMermaidNone,
    .end = kMermaidNone,
    .firstRegion = kMermaidNone,
    .lastRegion = kMermaidNone,
    .firstDescription = kMermaidNone,
    .lastDescription = kMermaidNone,
};

MermaidSpan addText(Parser& parser, PrStringView text) {
    return mermaidTextAdd(parser.diagram->text, text);
}

uint32_t currentScope(const Parser& parser) {
    return parser.scopes.count > 0 ? parser.scopes.last().current : kMermaidNone;
}

uint32_t addNode(Parser& parser, MermaidStateKind kind, MermaidSpan id, MermaidSpan label, uint32_t parent) {
    MermaidStateDiagram& diagram = *parser.diagram;
    diagram.nodes.push(MermaidStateNode{.id = id, .label = label, .kind = kind, .parent = parent});
    parser.records.push(kEmptyRecord);
    return diagram.nodes.count - 1;
}

// Returns the state named `id`, which is added to the current scope if the source has not named it yet. Parses a click
// statement's arguments: a state's ID, then a URL in quotes or after href, which a tooltip may follow. As in
// mermaid.js, the statement names a state that the source has made, and makes none, and a URL that runs script gives no
// link.
bool parseClick(Parser& parser, PrStringView rest) {
    PrStringView id{};
    PrStringView arguments{};
    mermaidSplitWord(rest, &id, &arguments);
    if (id.size == 0)
        return mermaidParseFail(parser, "a click statement names no state");
    if (mermaidStartsWithWord(arguments, "href"))
        arguments = prStringTrim(mermaidAfter(arguments, 4));
    PrStringView url{};
    PrStringView tooltip{};
    if (!mermaidQuotedRead(arguments, &url, &tooltip))
        return mermaidParseFail(parser, "a click statement has no URL");
    uint32_t node = mermaidNamesFind(parser.stateIds, id);
    if (node != kMermaidNone && mermaidLinkIsSafe(url))
        parser.diagram->nodes.data[node].link = addText(parser, url);
    return true;
}

uint32_t stateOf(Parser& parser, PrStringView id) {
    uint32_t found = mermaidNamesFind(parser.stateIds, id);
    if (found != kMermaidNone)
        return found;
    MermaidSpan span = addText(parser, id);
    uint32_t node = addNode(parser, MermaidStateKind_State, span, span, currentScope(parser));
    mermaidNamesSet(parser.stateIds, id, node);
    return node;
}

// Returns the start or the end of the current scope.
uint32_t pseudostate(Parser& parser, MermaidStateKind kind) {
    uint32_t scope = currentScope(parser);
    NodeRecord& record = scope != kMermaidNone ? parser.records.data[scope] : parser.root;
    uint32_t& slot = kind == MermaidStateKind_Start ? record.start : record.end;
    if (slot == kMermaidNone) {
        uint32_t node = addNode(parser, kind, MermaidSpan{}, MermaidSpan{}, scope);
        // The record moves if adding the node grows the records, so the slot is found again.
        NodeRecord& added = scope != kMermaidNone ? parser.records.data[scope] : parser.root;
        (kind == MermaidStateKind_Start ? added.start : added.end) = node;
        return node;
    }
    return slot;
}

// Reads a state's name in a transition: [*], or an ID with an optional :::class.
uint32_t transitionEnd(Parser& parser, PrStringView text, bool source) {
    text = prStringTrim(text);
    if (text == "[*]")
        return pseudostate(parser, source ? MermaidStateKind_Start : MermaidStateKind_End);
    size_t classAt = prStringFind(text, ":::");
    uint32_t node = stateOf(parser, prStringTrim(mermaidBefore(text, classAt)));
    if (classAt < text.size)
        mermaidStyleSheetAssign(parser.styles, node, prStringTrim(mermaidAfter(text, classAt + 3)));
    return node;
}

// Adds a line to a state's description.
void addDescription(Parser& parser, uint32_t node, PrStringView text) {
    uint32_t index = parser.descriptions.count;
    parser.descriptions.push(Description{text, kMermaidNone});
    NodeRecord& record = parser.records.data[node];
    if (record.lastDescription != kMermaidNone)
        parser.descriptions.data[record.lastDescription].next = index;
    else
        record.firstDescription = index;
    record.lastDescription = index;
}

// state X, state "Label" as X, either followed by <<fork>>, <<join>> or <<choice>>, or by { to open a composite state.
bool parseState(Parser& parser, PrStringView rest) {
    MermaidStateDiagram& diagram = *parser.diagram;
    bool opens = rest.size > 0 && rest.data[rest.size - 1] == '{';
    if (opens)
        rest = prStringTrim(PrStringView{rest.data, rest.size - 1});
    MermaidStateKind kind = MermaidStateKind_State;
    size_t marker = prStringFind(rest, "<<");
    if (marker < rest.size) {
        PrStringView name = prStringTrim(mermaidAfter(rest, marker + 2));
        if (prAsciiStartsWithFold(name, "fork"))
            kind = MermaidStateKind_Fork;
        else if (prAsciiStartsWithFold(name, "join"))
            kind = MermaidStateKind_Join;
        else if (prAsciiStartsWithFold(name, "choice"))
            kind = MermaidStateKind_Choice;
        rest = prStringTrim(PrStringView{rest.data, marker});
    }
    PrStringView label{};
    PrStringView id = rest;
    if (rest.size > 0 && rest.data[0] == '"') {
        size_t close = prStringFindByte(mermaidAfter(rest, 1), '"') + 1;
        if (close >= rest.size)
            return mermaidParseFail(parser, "a state's label has no closing quote");
        label = PrStringView{rest.data + 1, close - 1};
        PrStringView tail = prStringTrim(mermaidAfter(rest, close + 1));
        if (!prAsciiStartsWithFold(tail, "as "))
            return mermaidParseFail(parser, "a state's label is not followed by as and its name");
        id = prStringTrim(mermaidAfter(tail, 3));
    } else {
        // state X : description declares X with a description.
        size_t colon = prStringFindByte(rest, ':');
        if (colon < rest.size) {
            id = prStringTrim(PrStringView{rest.data, colon});
            addDescription(parser, stateOf(parser, id), prStringTrim(mermaidAfter(rest, colon + 1)));
            return true;
        }
    }
    size_t classAt = prStringFind(id, ":::");
    PrStringView cssClass{};
    if (classAt < id.size) {
        cssClass = prStringTrim(mermaidAfter(id, classAt + 3));
        id = prStringTrim(PrStringView{id.data, classAt});
    }
    if (id.size == 0)
        return mermaidParseFail(parser, "a state has no name");
    if (opens && parser.scopes.count >= kMaxStateDepth)
        return mermaidParseFail(parser, "a composite state nests too deeply");
    uint32_t node = stateOf(parser, id);
    MermaidStateNode& state = diagram.nodes.data[node];
    if (label.size > 0)
        state.label = addText(parser, label);
    if (kind != MermaidStateKind_State)
        state.kind = kind;
    if (cssClass.size > 0)
        mermaidStyleSheetAssign(parser.styles, node, cssClass);
    if (opens) {
        // A composite state that opens again continues in its last region.
        uint32_t last = parser.records.data[node].lastRegion;
        parser.scopes.push(Scope{node, last != kMermaidNone ? last : node, parser.line});
    }
    return true;
}

// Divides the innermost composite state into another concurrent region. The first -- gives the state its first region,
// which takes what the state holds once the source is read.
bool parseDivider(Parser& parser) {
    if (parser.scopes.count == 0)
        return mermaidParseFail(parser, "-- divides no composite state");
    uint32_t composite = parser.scopes.last().composite;
    if (parser.records.data[composite].firstRegion == kMermaidNone) {
        uint32_t first = addNode(parser, MermaidStateKind_Region, MermaidSpan{}, MermaidSpan{}, composite);
        parser.records.data[composite].firstRegion = first;
    }
    uint32_t region = addNode(parser, MermaidStateKind_Region, MermaidSpan{}, MermaidSpan{}, composite);
    parser.records.data[composite].lastRegion = region;
    parser.scopes.last().current = region;
    return true;
}

// note left of X : text, note right of X : text, or the same without the text, which then runs to `end note`.
bool parseNote(Parser& parser, PrStringView rest) {
    if (prAsciiStartsWithFold(rest, "left of "))
        rest = mermaidAfter(rest, 8);
    else if (prAsciiStartsWithFold(rest, "right of "))
        rest = mermaidAfter(rest, 9);
    else
        return mermaidParseFail(parser, "a note is not left of or right of a state");
    size_t colon = prStringFindByte(rest, ':');
    PrStringView id = prStringTrim(mermaidBefore(rest, colon));
    if (id.size == 0)
        return mermaidParseFail(parser, "a note names no state");
    MermaidStateDiagram& diagram = *parser.diagram;
    uint32_t state = stateOf(parser, id);
    uint32_t note =
        addNode(parser, MermaidStateKind_Note, MermaidSpan{}, MermaidSpan{}, diagram.nodes.data[state].parent);
    diagram.transitions.push(MermaidTransition{.source = state, .target = note, .note = true});
    if (colon < rest.size) {
        diagram.nodes.data[note].label = addText(parser, prStringTrim(mermaidAfter(rest, colon + 1)));
    } else {
        parser.note = note;
        parser.noteLine = parser.line;
        parser.noteText.clear();
    }
    return true;
}

// A --> B [: label], where either end may be [*] or carry a :::class.
bool parseTransition(Parser& parser, PrStringView statement, size_t arrow) {
    PrStringView tail = mermaidAfter(statement, arrow + 3);
    size_t colon = kMermaidNone;
    // The label follows the first colon that is not part of :::.
    for (size_t i = 0; i < tail.size; ++i) {
        if (prStringStartsWith(mermaidAfter(tail, i), ":::")) {
            i += 2;
            continue;
        }
        if (tail.data[i] == ':') {
            colon = i;
            break;
        }
    }
    PrStringView target = mermaidBefore(tail, colon);
    PrStringView sourceText = PrStringView{statement.data, arrow};
    if (prStringTrim(sourceText).size == 0 || prStringTrim(target).size == 0)
        return mermaidParseFail(parser, "a transition does not name both of its states");
    MermaidTransition transition{
        .source = transitionEnd(parser, sourceText, true),
        .target = transitionEnd(parser, target, false),
    };
    if (colon < tail.size)
        transition.label = addText(parser, prStringTrim(mermaidAfter(tail, colon + 1)));
    parser.diagram->transitions.push(transition);
    return true;
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidStateDiagram& diagram = *parser.diagram;
    if (parser.note != kMermaidNone) {
        if (prAsciiEqualsFold(statement, "end note")) {
            diagram.nodes.data[parser.note].label =
                addText(parser, PrStringView{parser.noteText.data, parser.noteText.count});
            parser.note = kMermaidNone;
        } else {
            if (parser.noteText.count > 0)
                parser.noteText.append("<br>", 4);
            parser.noteText.append(statement.data, uint32_t(statement.size));
        }
        return true;
    }
    if (statement == "}") {
        if (parser.scopes.count == 0)
            return mermaidParseFail(parser, "a brace closes nothing");
        parser.scopes.pop();
        return true;
    }
    if (statement == "--")
        return parseDivider(parser);
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    if (prAsciiEqualsFold(word, "direction")) {
        PrGraphDirection direction = PrGraphDirection_Down;
        if (!mermaidDirectionParse(rest, &direction))
            return mermaidParseFail(parser, "unknown direction");
        if (parser.scopes.count > 0)
            diagram.nodes.data[parser.scopes.last().composite].direction = direction;
        else
            diagram.direction = direction;
        return true;
    }
    if (prAsciiEqualsFold(word, "state"))
        return parseState(parser, rest);
    if (prAsciiEqualsFold(word, "note"))
        return parseNote(parser, rest);
    if (prAsciiEqualsFold(word, "classDef")) {
        PrStringView names{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &names, &declarations);
        mermaidEachName(names, [&](PrStringView name) {
            mermaidStyleSheetDefine(parser.styles, name, declarations);
        });
        return true;
    }
    if (prAsciiEqualsFold(word, "class")) {
        PrStringView ids{};
        PrStringView name{};
        mermaidSplitWord(rest, &ids, &name);
        mermaidEachName(ids, [&](PrStringView id) {
            mermaidStyleSheetAssign(parser.styles, stateOf(parser, id), name);
        });
        return true;
    }
    if (prAsciiEqualsFold(word, "style")) {
        PrStringView ids{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &ids, &declarations);
        mermaidEachName(ids, [&](PrStringView id) {
            mermaidStyleSheetSetOwn(parser.styles, stateOf(parser, id), declarations);
        });
        return true;
    }
    if (prAsciiEqualsFold(word, "click"))
        return parseClick(parser, rest);
    if (prAsciiEqualsFold(word, "hide") || prAsciiEqualsFold(word, "scale"))
        return true;
    size_t arrow = prStringFind(statement, "-->");
    if (arrow < statement.size)
        return parseTransition(parser, statement, arrow);
    size_t colon = prStringFindByte(statement, ':');
    if (colon < statement.size && !prStringStartsWith(mermaidAfter(statement, colon), ":::")) {
        PrStringView id = prStringTrim(PrStringView{statement.data, colon});
        if (id.size == 0)
            return mermaidParseFail(parser, "a description names no state");
        addDescription(parser, stateOf(parser, id), prStringTrim(mermaidAfter(statement, colon + 1)));
        return true;
    }
    if (prAsciiFindSpace(statement, 0) < statement.size)
        return mermaidParseFail(parser, "unknown statement");
    transitionEnd(parser, statement, true);
    return true;
}

// Completes the diagram once its statements are read: each divided composite state's first region takes what the state
// holds, each state's descriptions join with line breaks, and each node gets its style.
void finish(Parser& parser) {
    MermaidStateDiagram& diagram = *parser.diagram;
    for (MermaidStateNode& node : diagram.nodes.span()) {
        if (node.parent == kMermaidNone || node.kind == MermaidStateKind_Region)
            continue;
        uint32_t first = parser.records.data[node.parent].firstRegion;
        if (first != kMermaidNone)
            node.parent = first;
    }
    for (uint32_t n = 0; n < diagram.nodes.count; ++n) {
        uint32_t d = parser.records.data[n].firstDescription;
        if (d == kMermaidNone)
            continue;
        uint32_t start = diagram.text.count;
        for (; d != kMermaidNone; d = parser.descriptions.data[d].next) {
            if (diagram.text.count > start)
                diagram.text.append("<br>", 4);
            PrStringView text = parser.descriptions.data[d].text;
            diagram.text.append(text.data, uint32_t(text.size));
        }
        diagram.nodes.data[n].description = MermaidSpan{start, diagram.text.count - start};
    }
    // A state's style is the default class, the classes that it is given in source order, then its style statements.
    mermaidStyleSheetFinish(parser.styles, diagram.nodes.count);
    for (uint32_t n = 0; n < diagram.nodes.count; ++n) {
        MermaidStateNode& node = diagram.nodes.data[n];
        mermaidStyleSheetResolve(parser.styles, n, node.kind == MermaidStateKind_State, node.style);
    }
}

bool parse(Parser& parser, PrStringView source) {
    MermaidPreamble preamble = mermaidPreambleRead(source);
    parser.diagram->title = addText(parser, preamble.title);
    parser.line = preamble.header.number;
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_State, nullptr))
        return false;
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        if (!parseStatement(parser, line.content))
            return false;
    }
    if (parser.scopes.count > 0 || parser.note != kMermaidNone) {
        parser.line = parser.note != kMermaidNone ? parser.noteLine : parser.scopes.last().line;
        parser.column = 0;
        return mermaidParseFail(parser, "a composite state or note has no end");
    }
    finish(parser);
    return true;
}

} // namespace

PrStatus mermaidStateParse(MermaidStateDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    diagram.direction = PrGraphDirection_Down;
    diagram.title = MermaidSpan{};
    diagram.text.clear();
    diagram.nodes.clear();
    diagram.transitions.clear();
    Parser parser{};
    parser.error = error;
    parser.diagram = &diagram;
    parser.root = kEmptyRecord;
    parser.note = kMermaidNone;
    bool parsed = parse(parser, source);
    mermaidStyleSheetReset(parser.styles);
    mermaidNamesReset(parser.stateIds);
    parser.scopes.reset();
    parser.records.reset();
    parser.descriptions.reset();
    parser.noteText.reset();
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidStateText(const MermaidStateDiagram& diagram, MermaidSpan span) {
    return mermaidTextAt(diagram.text, span);
}

void mermaidStateReset(MermaidStateDiagram& diagram) {
    diagram.text.reset();
    diagram.nodes.reset();
    diagram.transitions.reset();
}

} // namespace pr_impl
