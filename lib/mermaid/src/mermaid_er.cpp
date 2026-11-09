// Entity relationship diagrams (mermaid_er.h). The parser reads the source one line at a time. A statement relates two
// entities, opens an entity's block of attributes, or starts with a keyword. An attribute block runs from the brace
// that opens it to the brace that closes it, with one attribute on each line. Keywords ignore case, as in mermaid.js.

#include "mermaid/src/mermaid_er.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

struct Parser : MermaidParseState {
    MermaidErDiagram* diagram;
    MermaidNames entityIds;                 // the entity of each ID, which views the source
    MermaidStyleSheet styles;               // by entity
    uint32_t block;                         // the entity whose attribute block is open, or kMermaidNone
    uint32_t blockLine;                     // the line that opens the block
};

MermaidSpan addText(Parser& parser, PrStringView text) {
    return mermaidTextAdd(parser.diagram->text, text);
}

// Returns the entity named `id`, or kMermaidNone if the source has not named it yet.
uint32_t findEntity(const Parser& parser, PrStringView id) {
    return mermaidNamesFind(parser.entityIds, id);
}

// Returns the entity named `id`, which is added if the source has not named it yet.
uint32_t entityOf(Parser& parser, PrStringView id) {
    uint32_t found = findEntity(parser, id);
    if (found != kMermaidNone)
        return found;
    MermaidErDiagram& diagram = *parser.diagram;
    MermaidSpan span = addText(parser, id);
    diagram.entities.push(MermaidEntity{.id = span, .label = span});
    mermaidNamesSet(parser.entityIds, id, diagram.entities.count - 1);
    return diagram.entities.count - 1;
}

// Reads an entity's name at `at`, quoted or up to white space or a bracket, and moves `at` past it and any :::class.
bool readEntity(Parser& parser, PrStringView text, size_t& at, uint32_t* out) {
    while (at < text.size && prAsciiIsSpace(text.data[at]))
        ++at;
    size_t start = at;
    PrStringView id{};
    if (at < text.size && text.data[at] == '"') {
        size_t close = prStringFindByte(mermaidAfter(text, at + 1), '"') + at + 1;
        if (close >= text.size)
            return false;
        id = PrStringView{text.data + at + 1, close - at - 1};
        at = close + 1;
    } else {
        while (at < text.size && !prAsciiIsSpace(text.data[at]) && prStringFindByte("{[:|}", text.data[at]) >= 5)
            ++at;
        id = PrStringView{text.data + start, at - start};
    }
    if (id.size == 0)
        return false;
    *out = entityOf(parser, id);
    if (prStringStartsWith(mermaidAfter(text, at), ":::")) {
        size_t name = at + 3;
        at = name;
        while (at < text.size && !prAsciiIsSpace(text.data[at]) && text.data[at] != '{')
            ++at;
        mermaidStyleSheetAssign(parser.styles, *out, PrStringView{text.data + name, at - name});
    }
    return true;
}

bool readCardinality(PrStringView token, bool left, MermaidCardinality* out) {
    struct Named {
        PrStringView left;
        PrStringView right;
        MermaidCardinality cardinality;
    };
    static const Named kNames[] = {
        {"|o", "o|", MermaidCardinality_ZeroOrOne},
        {"||", "||", MermaidCardinality_ExactlyOne},
        {"}o", "o{", MermaidCardinality_ZeroOrMore},
        {"}|", "|{", MermaidCardinality_OneOrMore},
    };
    for (const Named& named : kNames) {
        if (prAsciiEqualsFold(token, left ? named.left : named.right)) {
            *out = named.cardinality;
            return true;
        }
    }
    return false;
}

// A ||--o{ B : label, where the marks on either side of -- or .. give each end's cardinality.
bool parseRelation(Parser& parser, PrStringView statement) {
    size_t at = 0;
    uint32_t source = kMermaidNone;
    if (!readEntity(parser, statement, at, &source))
        return mermaidParseFail(parser, "unknown statement");
    while (at < statement.size && prAsciiIsSpace(statement.data[at]))
        ++at;
    PrStringView rest = mermaidAfter(statement, at);
    MermaidErRelation relation{.source = source};
    if (rest.size < 6 || !readCardinality(PrStringView{rest.data, 2}, true, &relation.sourceCardinality) ||
            !readCardinality(PrStringView{rest.data + 4, 2}, false, &relation.targetCardinality))
        return mermaidParseFail(parser, "unknown statement");
    PrStringView stroke{rest.data + 2, 2};
    if (stroke == "--")
        relation.identifying = true;
    else if (!(stroke == ".."))
        return mermaidParseFail(parser, "a relationship's line is neither -- nor ..");
    at += 6;
    if (!readEntity(parser, statement, at, &relation.target))
        return mermaidParseFail(parser, "a relationship names no second entity");
    PrStringView tail = prStringTrim(mermaidAfter(statement, at));
    if (tail.size > 0) {
        if (tail.data[0] != ':')
            return mermaidParseFail(parser, "unexpected text after a relationship");
        relation.label = addText(parser, mermaidUnquoted(mermaidAfter(tail, 1)));
    }
    parser.diagram->relations.push(relation);
    return true;
}

// type name [PK, FK, UK] ["comment"]
bool parseAttribute(Parser& parser, PrStringView line) {
    MermaidErDiagram& diagram = *parser.diagram;
    PrStringView type{};
    PrStringView rest{};
    mermaidSplitWord(line, &type, &rest);
    PrStringView name{};
    PrStringView tail{};
    mermaidSplitWord(rest, &name, &tail);
    if (type.size == 0 || name.size == 0)
        return mermaidParseFail(parser, "an attribute needs a type and a name");
    size_t quote = prStringFindByte(tail, '"');
    PrStringView keys = prStringTrim(mermaidBefore(tail, quote));
    PrStringView comment = quote < tail.size ? mermaidUnquoted(mermaidAfter(tail, quote)) : PrStringView{};
    diagram.attributes.push(MermaidAttribute{
        .entity = parser.block,
        .type = addText(parser, type),
        .name = addText(parser, name),
        .keys = addText(parser, keys),
        .comment = addText(parser, comment),
    });
    return true;
}

// An entity alone, with an alias in brackets or an attribute block, or a relationship.
bool parseEntity(Parser& parser, PrStringView statement) {
    MermaidErDiagram& diagram = *parser.diagram;
    size_t at = 0;
    uint32_t entity = kMermaidNone;
    if (!readEntity(parser, statement, at, &entity))
        return mermaidParseFail(parser, "unknown statement");
    PrStringView tail = prStringTrim(mermaidAfter(statement, at));
    if (prStringStartsWith(tail, "[")) {
        size_t close = prStringFindByte(tail, ']');
        if (close >= tail.size)
            return mermaidParseFail(parser, "an entity's alias has no closing bracket");
        diagram.entities.data[entity].label = addText(parser, mermaidUnquoted(PrStringView{tail.data + 1, close - 1}));
        tail = prStringTrim(mermaidAfter(tail, close + 1));
    }
    if (tail.size == 0)
        return true;
    if (tail.data[0] != '{')
        return parseRelation(parser, statement);
    PrStringView inside = prStringTrim(mermaidAfter(tail, 1));
    bool closes = inside.size > 0 && inside.data[inside.size - 1] == '}';
    if (closes)
        inside = prStringTrim(PrStringView{inside.data, inside.size - 1});
    parser.block = entity;
    parser.blockLine = parser.line;
    bool parsed = inside.size == 0 || parseAttribute(parser, inside);
    if (closes)
        parser.block = kMermaidNone;
    return parsed;
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidErDiagram& diagram = *parser.diagram;
    if (parser.block != kMermaidNone) {
        if (statement == "}") {
            parser.block = kMermaidNone;
            return true;
        }
        return parseAttribute(parser, statement);
    }
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    if (prAsciiEqualsFold(word, "direction")) {
        if (!mermaidDirectionParse(rest, &diagram.direction))
            return mermaidParseFail(parser, "unknown direction");
        return true;
    }
    if (prAsciiEqualsFold(word, "classDef")) {
        PrStringView names{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &names, &declarations);
        mermaidEachName(names, [&](PrStringView name) {
            mermaidStyleSheetDefine(parser.styles, name, declarations);
        });
        return true;
    }
    // As in mermaid.js, class and style statements apply only to entities that the source has already named.
    if (prAsciiEqualsFold(word, "class")) {
        PrStringView ids{};
        PrStringView name{};
        mermaidSplitWord(rest, &ids, &name);
        mermaidEachName(ids, [&](PrStringView id) {
            uint32_t entity = findEntity(parser, id);
            if (entity != kMermaidNone)
                mermaidStyleSheetAssign(parser.styles, entity, name);
        });
        return true;
    }
    if (prAsciiEqualsFold(word, "style")) {
        PrStringView ids{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &ids, &declarations);
        mermaidEachName(ids, [&](PrStringView id) {
            uint32_t entity = findEntity(parser, id);
            if (entity != kMermaidNone)
                mermaidStyleSheetSetOwn(parser.styles, entity, declarations);
        });
        return true;
    }
    return parseEntity(parser, statement);
}

bool parse(Parser& parser, PrStringView source) {
    MermaidPreamble preamble = mermaidPreambleRead(source);
    parser.diagram->title = addText(parser, preamble.title);
    parser.line = preamble.header.number;
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Er, nullptr))
        return false;
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        if (!parseStatement(parser, line.content))
            return false;
    }
    if (parser.block != kMermaidNone) {
        parser.line = parser.blockLine;
        parser.column = 0;
        return mermaidParseFail(parser, "an entity's attributes have no closing brace");
    }
    // An entity's style is the default class, the classes that it is given in source order, then its style statements.
    MermaidErDiagram& diagram = *parser.diagram;
    mermaidStyleSheetFinish(parser.styles, diagram.entities.count);
    for (uint32_t e = 0; e < diagram.entities.count; ++e)
        mermaidStyleSheetResolve(parser.styles, e, true, diagram.entities.data[e].style);
    return true;
}

} // namespace

PrStatus mermaidErParse(MermaidErDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    diagram.direction = PrGraphDirection_Down;
    diagram.title = MermaidSpan{};
    diagram.text.clear();
    diagram.entities.clear();
    diagram.attributes.clear();
    diagram.relations.clear();
    Parser parser{};
    parser.error = error;
    parser.diagram = &diagram;
    parser.block = kMermaidNone;
    bool parsed = parse(parser, source);
    mermaidStyleSheetReset(parser.styles);
    mermaidNamesReset(parser.entityIds);
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidErText(const MermaidErDiagram& diagram, MermaidSpan span) {
    return mermaidTextAt(diagram.text, span);
}

void mermaidErReset(MermaidErDiagram& diagram) {
    diagram.text.reset();
    diagram.entities.reset();
    diagram.attributes.reset();
    diagram.relations.reset();
}

} // namespace pr_impl
