// Class diagrams (mermaid_class.h). The parser reads the source one line at a time. A statement starts with a keyword,
// adds a member to a class with a colon after the class's name, or relates two classes. A class's body runs from the
// brace that opens it to the brace that closes it, with one member on each line. Keywords are case-sensitive, as in
// mermaid.js.

#include "mermaid/src/mermaid_class.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// The deepest that namespaces nest. A namespace's layout reads the namespaces around each node.
constexpr uint32_t kMaxNamespaceDepth = 64;

struct Parser : MermaidParseState {
    MermaidClassDiagram* diagram;
    MermaidNames classIds;                  // the node of each class's ID, which views the source
    MermaidStyleSheet styles;               // by node
    uint32_t body;                          // the class whose body is open, or kMermaidNone
    uint32_t bodyLine;                      // the line that opens the body
    RawArray<uint32_t> spaces;              // the open namespaces, innermost last
    RawArray<uint32_t> spaceLines;          // by open namespace: the line that opens it
    RawArray<char> scratch;
};

MermaidSpan addText(Parser& parser, PrStringView text) {
    return mermaidTextAdd(parser.diagram->text, text);
}

uint32_t countOf(PrStringView text, char c) {
    uint32_t count = 0;
    for (size_t i = 0; i < text.size; ++i)
        count += text.data[i] == c ? 1 : 0;
    return count;
}

// Appends one type of a generic list, its pairs of tildes turned into angle brackets from the outside in.
void appendGenericPart(RawArray<char>& out, PrStringView text) {
    uint32_t tildes = countOf(text, '~');
    if (tildes <= 1) {
        out.append(text.data, uint32_t(text.size));
        return;
    }
    // An odd tilde at the start belongs to the text before it, as Mermaid reads it.
    bool leading = tildes % 2 != 0 && text.data[0] == '~';
    if (leading) {
        out.push('~');
        text = mermaidAfter(text, 1);
    }
    uint32_t start = out.count;
    out.append(text.data, uint32_t(text.size));
    // The first tilde pairs with the last, the second with the one before the last, and so on.
    uint32_t first = start;
    uint32_t last = out.count;
    for (;;) {
        while (first < last && out.data[first] != '~')
            ++first;
        while (last > first && out.data[last - 1] != '~')
            --last;
        if (last <= first + 1)
            break;
        out.data[first++] = '<';
        out.data[--last] = '>';
    }
}

// Appends `text` with Mermaid's generic types, written between tildes, in angle brackets: List~int~ becomes List<int>.
// Commas separate the types of a list, except between the two tildes of one type, as in Map~K,V~.
void appendGenerics(RawArray<char>& out, PrStringView text) {
    size_t start = 0;
    while (start <= text.size) {
        size_t end = start;
        while (end < text.size && text.data[end] != ',')
            ++end;
        // A part with one tilde joins the next part if that has one too.
        if (end < text.size && countOf(PrStringView{text.data + start, end - start}, '~') == 1) {
            size_t next = end + 1;
            while (next < text.size && text.data[next] != ',')
                ++next;
            if (countOf(PrStringView{text.data + end + 1, next - end - 1}, '~') == 1)
                end = next;
        }
        appendGenericPart(out, PrStringView{text.data + start, end - start});
        if (end >= text.size)
            break;
        out.push(',');
        start = end + 1;
    }
}

bool isNameByte(char c) {
    return prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '_' || (unsigned char)c >= 0x80;
}

// A class's name as a statement writes it: its ID, and its generic type between tildes, which may be empty.
struct ClassName {
    PrStringView id;
    PrStringView generic;
};

// Reads a class's name at `at`, and moves `at` past it. A name in backticks may hold any characters. Otherwise a name
// holds letters, digits and underscores, and dashes or dots between them.
bool readName(PrStringView text, size_t& at, ClassName* out) {
    *out = ClassName{};
    if (at < text.size && text.data[at] == '`') {
        size_t close = prStringFindByte(mermaidAfter(text, at + 1), '`') + at + 1;
        if (close >= text.size)
            return false;
        out->id = PrStringView{text.data + at + 1, close - at - 1};
        at = close + 1;
    } else {
        size_t start = at;
        while (at < text.size) {
            char c = text.data[at];
            bool joins = (c == '-' || c == '.') && at > start && at + 1 < text.size && isNameByte(text.data[at + 1]);
            if (!isNameByte(c) && !joins)
                break;
            ++at;
        }
        if (at == start)
            return false;
        out->id = PrStringView{text.data + start, at - start};
    }
    if (at < text.size && text.data[at] == '~') {
        // The type runs to the last tilde before the name ends.
        size_t last = at;
        for (size_t i = at + 1; i < text.size; ++i) {
            char c = text.data[i];
            if (prAsciiIsSpace(c) || c == '{' || c == '[' || c == ':' || c == '"')
                break;
            if (c == '~')
                last = i;
        }
        if (last > at) {
            out->generic = PrStringView{text.data + at + 1, last - at - 1};
            at = last + 1;
        }
    }
    return true;
}

// Reads a whole statement's text as a class's name, as a style or click statement names its class.
bool readWholeName(PrStringView text, ClassName* out) {
    size_t at = 0;
    return readName(text, at, out);
}

uint32_t addNode(Parser& parser, MermaidClassNodeKind kind, MermaidSpan id, MermaidSpan label) {
    MermaidClassDiagram& diagram = *parser.diagram;
    diagram.nodes.push(MermaidClassNode{
        .id = id,
        .label = label,
        .kind = kind,
        .space = parser.spaces.count > 0 ? parser.spaces.last() : kMermaidNone,
    });
    return diagram.nodes.count - 1;
}

// Returns the class named `id`, or kMermaidNone if the source has not named it yet.
uint32_t findClass(const Parser& parser, PrStringView id) {
    return mermaidNamesFind(parser.classIds, id);
}

// Returns the class named `name`, which is added to the open namespace if the source has not named it yet.
uint32_t classOf(Parser& parser, const ClassName& name) {
    uint32_t found = findClass(parser, name.id);
    if (found != kMermaidNone)
        return found;
    MermaidSpan id = addText(parser, name.id);
    MermaidSpan label = id;
    if (name.generic.size > 0) {
        parser.scratch.clear();
        parser.scratch.append(name.id.data, uint32_t(name.id.size));
        parser.scratch.push('<');
        appendGenerics(parser.scratch, name.generic);
        parser.scratch.push('>');
        label = addText(parser, PrStringView{parser.scratch.data, parser.scratch.count});
    }
    uint32_t node = addNode(parser, MermaidClassNodeKind_Class, id, label);
    mermaidNamesSet(parser.classIds, name.id, node);
    return node;
}

// Adds a member to a class, as its text in the source reads: an annotation between << and >>, a method if it holds a
// closing parenthesis, and an attribute otherwise.
void addMember(Parser& parser, uint32_t node, PrStringView text) {
    MermaidClassDiagram& diagram = *parser.diagram;
    text = prStringTrim(text);
    if (text.size == 0)
        return;
    if (prStringStartsWith(text, "<<") && prStringEndsWith(text, ">>") && text.size >= 4) {
        if (diagram.nodes.data[node].annotation.size == 0)
            diagram.nodes.data[node].annotation = addText(parser, prStringTrim(PrStringView{text.data + 2, text.size - 4}));
        return;
    }
    MermaidClassMember member{.node = node};
    size_t close = text.size;
    for (size_t i = text.size; i-- > 1;) {
        if (text.data[i] == ')') {
            close = i;
            break;
        }
    }
    size_t open = close;
    for (size_t i = close; i-- > 0 && close < text.size;) {
        if (text.data[i] == '(') {
            open = i;
            break;
        }
    }
    bool visible = prStringFindByte("#+~-", text.data[0]) < 4;
    RawArray<char>& out = parser.scratch;
    out.clear();
    if (close < text.size && open < close && open >= (visible ? 2u : 1u)) {
        member.method = true;
        size_t start = visible ? 1 : 0;
        PrStringView id{text.data + start, open - start};
        PrStringView parameters = prStringTrim(PrStringView{text.data + open + 1, close - open - 1});
        PrStringView rest = mermaidAfter(text, close + 1);
        char classifier = 0;
        if (rest.size > 0 && (rest.data[0] == '$' || rest.data[0] == '*' || prAsciiIsSpace(rest.data[0]))) {
            classifier = prAsciiIsSpace(rest.data[0]) ? 0 : rest.data[0];
            rest = mermaidAfter(rest, 1);
        }
        PrStringView returns = prStringTrim(rest);
        if (classifier == 0 && (prStringEndsWith(returns, "$") || prStringEndsWith(returns, "*"))) {
            classifier = returns.data[returns.size - 1];
            returns = prStringTrim(PrStringView{returns.data, returns.size - 1});
        }
        if (classifier == '$')
            member.classifier = MermaidClassifier_Static;
        else if (classifier == '*')
            member.classifier = MermaidClassifier_Abstract;
        if (visible)
            out.push(text.data[0]);
        if (id.size > 0 && prAsciiIsSpace(id.data[0]))
            out.push(' ');
        appendGenerics(out, prStringTrim(id));
        out.push('(');
        appendGenerics(out, parameters);
        out.push(')');
        if (returns.size > 0) {
            out.append(" : ", 3);
            appendGenerics(out, returns);
        }
    } else {
        char last = text.data[text.size - 1];
        bool classified = text.size > 1 && (last == '$' || last == '*');
        if (classified)
            member.classifier = last == '$' ? MermaidClassifier_Static : MermaidClassifier_Abstract;
        size_t start = visible ? 1 : 0;
        size_t end = classified ? text.size - 1 : text.size;
        PrStringView id{text.data + start, end > start ? end - start : 0};
        if (visible)
            out.push(text.data[0]);
        if (id.size > 0 && prAsciiIsSpace(id.data[0]))
            out.push(' ');
        appendGenerics(out, prStringTrim(id));
    }
    member.text = addText(parser, prStringTrim(PrStringView{out.data, out.count}));
    diagram.members.push(member);
}

// Reads `:::name` at `at`, if it is there, and gives the class to `node`.
void readCssClass(Parser& parser, PrStringView text, size_t& at, uint32_t node) {
    if (!prStringStartsWith(mermaidAfter(text, at), ":::"))
        return;
    at += 3;
    size_t start = at;
    while (at < text.size && !prAsciiIsSpace(text.data[at]) && text.data[at] != '{')
        ++at;
    mermaidStyleSheetAssign(parser.styles, node, PrStringView{text.data + start, at - start});
}

// class Name[~T~][["Label"]][:::style] [{ members }]
bool parseClass(Parser& parser, PrStringView rest) {
    size_t at = 0;
    ClassName name{};
    if (!readName(rest, at, &name))
        return mermaidParseFail(parser, "a class has no name");
    uint32_t node = classOf(parser, name);
    MermaidClassDiagram& diagram = *parser.diagram;
    if (parser.spaces.count > 0)
        diagram.nodes.data[node].space = parser.spaces.last();
    if (prStringStartsWith(mermaidAfter(rest, at), "[")) {
        size_t close = prStringFindByte(mermaidAfter(rest, at + 1), ']') + at + 1;
        if (close >= rest.size)
            return mermaidParseFail(parser, "a class's label has no closing bracket");
        diagram.nodes.data[node].label = addText(parser, mermaidUnquoted(PrStringView{rest.data + at + 1, close - at - 1}));
        at = close + 1;
    }
    readCssClass(parser, rest, at, node);
    mermaidSkipSpace(rest, &at);
    if (at >= rest.size)
        return true;
    if (rest.data[at] != '{')
        return mermaidParseFail(parser, "unexpected text after a class's name");
    // A body that closes on the same line holds the members between its braces.
    PrStringView inside = prStringTrim(mermaidAfter(rest, at + 1));
    if (prStringEndsWith(inside, "}")) {
        addMember(parser, node, PrStringView{inside.data, inside.size - 1});
        return true;
    }
    addMember(parser, node, inside);
    parser.body = node;
    parser.bodyLine = parser.line;
    return true;
}

// Reads a relation's marks and stroke at `at`: [<|, *, o, < or ()] -- or .. [|>, *, o, > or ()].
bool readRelation(PrStringView text, size_t& at, MermaidRelationEnd* start, MermaidRelationEnd* end, bool* dashed) {
    PrStringView rest = mermaidAfter(text, at);
    size_t i = 0;
    *start = MermaidRelationEnd_None;
    if (prStringStartsWith(rest, "<|")) {
        *start = MermaidRelationEnd_Extension;
        i = 2;
    } else if (prStringStartsWith(rest, "()")) {
        *start = MermaidRelationEnd_Lollipop;
        i = 2;
    } else if (prStringStartsWith(rest, "*")) {
        *start = MermaidRelationEnd_Composition;
        i = 1;
    } else if (prStringStartsWith(rest, "o-") || prStringStartsWith(rest, "o.")) {
        *start = MermaidRelationEnd_Aggregation;
        i = 1;
    } else if (prStringStartsWith(rest, "<")) {
        *start = MermaidRelationEnd_Dependency;
        i = 1;
    }
    PrStringView line = mermaidAfter(rest, i);
    if (prStringStartsWith(line, "--"))
        *dashed = false;
    else if (prStringStartsWith(line, ".."))
        *dashed = true;
    else
        return false;
    i += 2;
    PrStringView tail = mermaidAfter(rest, i);
    *end = MermaidRelationEnd_None;
    if (prStringStartsWith(tail, "|>")) {
        *end = MermaidRelationEnd_Extension;
        i += 2;
    } else if (prStringStartsWith(tail, "()")) {
        *end = MermaidRelationEnd_Lollipop;
        i += 2;
    } else if (prStringStartsWith(tail, "*")) {
        *end = MermaidRelationEnd_Composition;
        i += 1;
    } else if (prStringStartsWith(tail, "o") && (tail.size == 1 || !isNameByte(tail.data[1]))) {
        *end = MermaidRelationEnd_Aggregation;
        i += 1;
    } else if (prStringStartsWith(tail, ">")) {
        *end = MermaidRelationEnd_Dependency;
        i += 1;
    }
    at += i;
    return true;
}

// Reads a quoted cardinality at `at`, if one is there.
PrStringView readCardinality(PrStringView text, size_t& at) {
    mermaidSkipSpace(text, &at);
    if (at >= text.size || text.data[at] != '"')
        return PrStringView{};
    size_t close = prStringFindByte(mermaidAfter(text, at + 1), '"') + at + 1;
    PrStringView quoted{text.data + at + 1, (close < text.size ? close : text.size) - at - 1};
    at = close < text.size ? close + 1 : text.size;
    mermaidSkipSpace(text, &at);
    return quoted;
}

// Returns the node at one end of a relation: the class that `name` names, or a new interface for the end with a
// lollipop.
uint32_t relationEnd(Parser& parser, const ClassName& name, bool lollipop) {
    if (!lollipop)
        return classOf(parser, name);
    return addNode(parser, MermaidClassNodeKind_Interface, MermaidSpan{}, addText(parser, name.id));
}

// A statement that starts with a class's name: Name : member, Name:::style, or a relation, A ["1"] <|-- ["*"] B [:
// label].
bool parseNamed(Parser& parser, PrStringView statement) {
    MermaidClassDiagram& diagram = *parser.diagram;
    size_t at = 0;
    ClassName first{};
    if (!readName(statement, at, &first))
        return mermaidParseFail(parser, "unknown statement");
    mermaidSkipSpace(statement, &at);
    if (at >= statement.size) {
        classOf(parser, first);
        return true;
    }
    if (prStringStartsWith(mermaidAfter(statement, at), ":::")) {
        readCssClass(parser, statement, at, classOf(parser, first));
        return true;
    }
    if (statement.data[at] == ':') {
        addMember(parser, classOf(parser, first), mermaidAfter(statement, at + 1));
        return true;
    }
    PrStringView sourceCardinality = readCardinality(statement, at);
    MermaidRelation relation{};
    if (!readRelation(statement, at, &relation.start, &relation.end, &relation.dashed))
        return mermaidParseFail(parser, "unknown statement");
    PrStringView targetCardinality = readCardinality(statement, at);
    ClassName second{};
    if (!readName(statement, at, &second))
        return mermaidParseFail(parser, "a relation names no second class");
    mermaidSkipSpace(statement, &at);
    PrStringView label{};
    if (at < statement.size) {
        if (statement.data[at] != ':')
            return mermaidParseFail(parser, "unexpected text after a relation");
        label = prStringTrim(mermaidAfter(statement, at + 1));
    }
    relation.source = relationEnd(parser, first, relation.start == MermaidRelationEnd_Lollipop);
    relation.target = relationEnd(parser, second, relation.end == MermaidRelationEnd_Lollipop);
    relation.label = addText(parser, label);
    relation.sourceCardinality = addText(parser, sourceCardinality);
    relation.targetCardinality = addText(parser, targetCardinality);
    diagram.relations.push(relation);
    return true;
}

// note "text", or note for Class "text".
bool parseNote(Parser& parser, PrStringView rest) {
    MermaidClassDiagram& diagram = *parser.diagram;
    uint32_t target = kMermaidNone;
    if (mermaidStartsWithWord(rest, "for")) {
        size_t at = 3;
        mermaidSkipSpace(rest, &at);
        ClassName name{};
        if (!readName(rest, at, &name))
            return mermaidParseFail(parser, "a note names no class");
        target = classOf(parser, name);
        rest = mermaidAfter(rest, at);
    }
    uint32_t note = addNode(parser, MermaidClassNodeKind_Note, MermaidSpan{}, addText(parser, mermaidUnquoted(rest)));
    if (target != kMermaidNone)
        diagram.relations.push(MermaidRelation{.source = note, .target = target, .note = true});
    return true;
}

// link Class "url" [...], click Class href "url" [...], or a click or callback statement that calls a function, which
// gives no link. As in mermaid.js, a link applies only to a class that is already named, and the URL must be safe.
bool parseLink(Parser& parser, PrStringView word, PrStringView rest) {
    PrStringView id{};
    PrStringView tail{};
    mermaidSplitWord(rest, &id, &tail);
    ClassName name{};
    if (!readWholeName(id, &name))
        return mermaidParseFail(parser, "a click or link statement names no class");
    if (word == "callback")
        return true;
    if (word == "click") {
        if (!mermaidStartsWithWord(tail, "href"))
            return true;
        tail = prStringTrim(mermaidAfter(tail, 4));
    }
    PrStringView url{};
    PrStringView after{};
    if (!mermaidQuotedRead(tail, &url, &after))
        return mermaidParseFail(parser, "a link has no quoted URL");
    uint32_t node = findClass(parser, name.id);
    if (node != kMermaidNone && mermaidLinkIsSafe(url))
        parser.diagram->nodes.data[node].link = addText(parser, url);
    return true;
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidClassDiagram& diagram = *parser.diagram;
    if (parser.body != kMermaidNone) {
        bool closes = prStringEndsWith(statement, "}");
        addMember(parser, parser.body, closes ? PrStringView{statement.data, statement.size - 1} : statement);
        if (closes)
            parser.body = kMermaidNone;
        return true;
    }
    if (statement == "}") {
        if (parser.spaces.count == 0)
            return mermaidParseFail(parser, "a brace closes nothing");
        parser.spaces.pop();
        parser.spaceLines.pop();
        return true;
    }
    if (statement == "{")
        return true;
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    if (word == "direction") {
        if (!mermaidDirectionParse(rest, &diagram.direction))
            return mermaidParseFail(parser, "unknown direction");
        return true;
    }
    if (word == "class")
        return parseClass(parser, rest);
    if (word == "namespace") {
        PrStringView name = prStringTrim(mermaidBefore(rest, prStringFindByte(rest, '{')));
        if (name.size == 0)
            return mermaidParseFail(parser, "a namespace has no name");
        if (parser.spaces.count >= kMaxNamespaceDepth)
            return mermaidParseFail(parser, "a namespace nests too deeply");
        diagram.namespaces.push(MermaidNamespace{
            .name = addText(parser, name),
            .parent = parser.spaces.count > 0 ? parser.spaces.last() : kMermaidNone,
        });
        parser.spaces.push(diagram.namespaces.count - 1);
        parser.spaceLines.push(parser.line);
        return true;
    }
    if (word == "note")
        return parseNote(parser, rest);
    if (word == "style") {
        // As in mermaid.js, a style applies only to a class that is already named.
        PrStringView id{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &id, &declarations);
        ClassName name{};
        if (!readWholeName(id, &name))
            return mermaidParseFail(parser, "style names no class");
        uint32_t node = findClass(parser, name.id);
        if (node != kMermaidNone)
            mermaidStyleSheetSetOwn(parser.styles, node, declarations);
        return true;
    }
    if (word == "classDef") {
        PrStringView names{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &names, &declarations);
        mermaidEachName(names, [&](PrStringView name) {
            mermaidStyleSheetDefine(parser.styles, name, declarations);
        });
        return true;
    }
    if (word == "cssClass") {
        // As in mermaid.js, the classes apply only to classes that are already named.
        PrStringView classes{};
        PrStringView name{};
        if (!mermaidQuotedRead(rest, &classes, &name))
            return mermaidParseFail(parser, "cssClass names no classes");
        mermaidEachName(classes, [&](PrStringView id) {
            ClassName parsed{};
            if (!readWholeName(id, &parsed))
                return;
            uint32_t node = findClass(parser, parsed.id);
            if (node != kMermaidNone)
                mermaidStyleSheetAssign(parser.styles, node, name);
        });
        return true;
    }
    if (prStringStartsWith(statement, "<<")) {
        size_t close = prStringFind(statement, ">>");
        if (close >= statement.size)
            return mermaidParseFail(parser, "an annotation has no closing >>");
        size_t at = close + 2;
        mermaidSkipSpace(statement, &at);
        ClassName name{};
        if (!readName(statement, at, &name))
            return mermaidParseFail(parser, "an annotation names no class");
        addMember(parser, classOf(parser, name), PrStringView{statement.data, close + 2});
        return true;
    }
    if (word == "click" || word == "link" || word == "callback")
        return parseLink(parser, word, rest);
    return parseNamed(parser, statement);
}

bool parse(Parser& parser, PrStringView source) {
    MermaidPreamble preamble = mermaidPreambleRead(source);
    parser.diagram->title = addText(parser, preamble.title);
    parser.line = preamble.header.number;
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Class, nullptr))
        return false;
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        if (!parseStatement(parser, line.content))
            return false;
    }
    if (parser.body != kMermaidNone || parser.spaces.count > 0) {
        parser.line = parser.body != kMermaidNone ? parser.bodyLine : parser.spaceLines.last();
        parser.column = 0;
        return mermaidParseFail(parser, "a class or namespace has no closing brace");
    }
    // Each node's style: the default class, the classes that it is given in source order, then its style statements.
    MermaidClassDiagram& diagram = *parser.diagram;
    mermaidStyleSheetFinish(parser.styles, diagram.nodes.count);
    for (uint32_t n = 0; n < diagram.nodes.count; ++n)
        mermaidStyleSheetResolve(parser.styles, n, true, diagram.nodes.data[n].style);
    return true;
}

} // namespace

PrStatus mermaidClassParse(MermaidClassDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    diagram.direction = PrGraphDirection_Down;
    diagram.title = MermaidSpan{};
    diagram.text.clear();
    diagram.nodes.clear();
    diagram.members.clear();
    diagram.relations.clear();
    diagram.namespaces.clear();
    Parser parser{};
    parser.error = error;
    parser.diagram = &diagram;
    parser.body = kMermaidNone;
    bool parsed = parse(parser, source);
    mermaidStyleSheetReset(parser.styles);
    mermaidNamesReset(parser.classIds);
    parser.spaces.reset();
    parser.spaceLines.reset();
    parser.scratch.reset();
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidClassText(const MermaidClassDiagram& diagram, MermaidSpan span) {
    return mermaidTextAt(diagram.text, span);
}

void mermaidClassReset(MermaidClassDiagram& diagram) {
    diagram.text.reset();
    diagram.nodes.reset();
    diagram.members.reset();
    diagram.relations.reset();
    diagram.namespaces.reset();
}

} // namespace pr_impl
