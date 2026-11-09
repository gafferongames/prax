// Requirement diagrams (mermaid_requirement.h). The parser reads the source one line at a time. A statement opens a
// requirement's or an element's block, relates two blocks, or starts with a keyword. A block runs from the brace that
// opens it to the brace that closes it, with one field on each line. Keywords and the values of risks and verification
// methods ignore case, as in mermaid.js.

#include "mermaid/src/mermaid_requirement.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// A class that a class statement or ::: gives the block named `block`, whether or not it is defined yet. Names view the
// source.
struct PendingClass {
    PrStringView block;
    PrStringView name;
};

// The declarations that a style statement gives the block named `block`.
struct PendingStyle {
    PrStringView block;
    PrStringView declarations;
};

// A relation as written, whose names are resolved once every block is defined.
struct PendingLink {
    PrStringView source;
    PrStringView target;
    MermaidRequirementRelation relation;
    uint32_t line;
    uint32_t column;
};

struct Parser : MermaidParseState {
    MermaidRequirementDiagram* diagram;
    MermaidNames blockNames;                // the block of each name, which views the source
    MermaidStyleSheet styles;               // by block
    bool inBlock;
    uint32_t blockLine;                     // the line that opens the block
    MermaidRequirementBlock pending;        // the open block, which joins the diagram when it closes
    PrStringView pendingName;               // the open block's name, which views the source
    RawArray<PendingClass> classes;
    RawArray<PendingStyle> ownStyles;
    RawArray<PendingLink> links;
};

struct Keyword {
    PrStringView word;
    uint8_t value;
};

constexpr Keyword kKinds[] = {
    {"requirement", MermaidRequirementKind_Requirement},
    {"functionalRequirement", MermaidRequirementKind_Functional},
    {"interfaceRequirement", MermaidRequirementKind_Interface},
    {"performanceRequirement", MermaidRequirementKind_Performance},
    {"physicalRequirement", MermaidRequirementKind_Physical},
    {"designConstraint", MermaidRequirementKind_DesignConstraint},
    {"element", MermaidRequirementKind_Element},
};

constexpr Keyword kRisks[] = {{"low", MermaidRisk_Low}, {"medium", MermaidRisk_Medium}, {"high", MermaidRisk_High}};

constexpr Keyword kVerifyMethods[] = {
    {"analysis", MermaidVerifyMethod_Analysis},
    {"demonstration", MermaidVerifyMethod_Demonstration},
    {"inspection", MermaidVerifyMethod_Inspection},
    {"test", MermaidVerifyMethod_Test},
};

// By relation.
constexpr PrStringView kRelationNames[] = {
    "contains", "copies", "derives", "satisfies", "verifies", "refines", "traces",
};
static_assert(countOf(kRelationNames) == MermaidRequirementRelation_Count, "a name for each relation");

// Finds `word` among `keywords`, ignoring case.
template <size_t N>
bool lookup(const Keyword (&keywords)[N], PrStringView word, uint8_t* out) {
    for (const Keyword& keyword : keywords) {
        if (prAsciiEqualsFold(word, keyword.word)) {
            *out = keyword.value;
            return true;
        }
    }
    return false;
}

MermaidSpan addText(Parser& parser, PrStringView text) {
    return mermaidTextAdd(parser.diagram->text, text);
}

// Reads a name at `at`, quoted or up to white space or any of `stops`, and moves `at` past it.
bool readName(PrStringView text, size_t& at, PrStringView stops, PrStringView* out) {
    mermaidSkipSpace(text, &at);
    if (at < text.size && text.data[at] == '"') {
        size_t close = prStringFindByte(mermaidAfter(text, at + 1), '"') + at + 1;
        if (close >= text.size)
            return false;
        *out = PrStringView{text.data + at + 1, close - at - 1};
        at = close + 1;
        return out->size > 0;
    }
    size_t start = at;
    while (at < text.size && !prAsciiIsSpace(text.data[at]) && prStringFindByte(stops, text.data[at]) >= stops.size)
        ++at;
    *out = PrStringView{text.data + start, at - start};
    return out->size > 0;
}

// Reads `:::name,name` at `at` if it is there, and gives the classes to the block named `block`.
void readClasses(Parser& parser, PrStringView text, size_t& at, PrStringView block) {
    if (!prStringStartsWith(mermaidAfter(text, at), ":::"))
        return;
    at += 3;
    size_t start = at;
    while (at < text.size && !prAsciiIsSpace(text.data[at]) && text.data[at] != '{')
        ++at;
    mermaidEachName(PrStringView{text.data + start, at - start}, [&](PrStringView name) {
        parser.classes.push(PendingClass{block, name});
    });
}

// Adds the block that has just closed, unless a block of its name is already defined, as mermaid.js keeps the first.
void closeBlock(Parser& parser) {
    MermaidRequirementDiagram& diagram = *parser.diagram;
    parser.inBlock = false;
    if (mermaidNamesFind(parser.blockNames, parser.pendingName) != kMermaidNone)
        return;
    diagram.blocks.push(parser.pending);
    mermaidNamesSet(parser.blockNames, parser.pendingName, diagram.blocks.count - 1);
}

// <kind> name[:::class] {
bool parseBlockStart(Parser& parser, MermaidRequirementKind kind, PrStringView rest) {
    size_t at = 0;
    PrStringView name{};
    if (!readName(rest, at, "{:", &name))
        return mermaidParseFail(parser, "a block has no name");
    readClasses(parser, rest, at, name);
    PrStringView tail = prStringTrim(mermaidAfter(rest, at));
    if (tail.size == 0 || tail.data[0] != '{')
        return mermaidParseFail(parser, "a block has no opening brace");
    parser.pending = MermaidRequirementBlock{.kind = kind, .name = addText(parser, name)};
    parser.pendingName = name;
    parser.inBlock = true;
    parser.blockLine = parser.line;
    tail = prStringTrim(mermaidAfter(tail, 1));
    if (tail == "}")
        closeBlock(parser);
    else if (tail.size > 0)
        return mermaidParseFail(parser, "unexpected text after a block's opening brace");
    return true;
}

// key: value
bool parseField(Parser& parser, PrStringView line) {
    MermaidRequirementBlock& block = parser.pending;
    size_t colon = prStringFindByte(line, ':');
    if (colon >= line.size)
        return mermaidParseFail(parser, "a field has no colon");
    PrStringView key = prStringTrim(PrStringView{line.data, colon});
    PrStringView value = prStringTrim(mermaidAfter(line, colon + 1));
    if (value.size > 0 && value.data[value.size - 1] == ';')
        value = prStringTrim(PrStringView{value.data, value.size - 1});
    value = mermaidUnquoted(value);
    bool element = block.kind == MermaidRequirementKind_Element;
    if (!element && prAsciiEqualsFold(key, "id")) {
        block.id = addText(parser, value);
    } else if (!element && prAsciiEqualsFold(key, "text")) {
        block.text = addText(parser, value);
    } else if (!element && prAsciiEqualsFold(key, "risk")) {
        if (!lookup(kRisks, value, &block.risk))
            return mermaidParseFail(parser, "a risk is not low, medium or high");
    } else if (!element && prAsciiEqualsFold(key, "verifymethod")) {
        if (!lookup(kVerifyMethods, value, &block.verifyMethod))
            return mermaidParseFail(parser, "a verification method is not analysis, demonstration, inspection or test");
    } else if (element && prAsciiEqualsFold(key, "type")) {
        block.type = addText(parser, value);
    } else if (element && prAsciiEqualsFold(key, "docref")) {
        block.docRef = addText(parser, value);
    } else {
        return mermaidParseFail(parser, "unknown field");
    }
    return true;
}

// a - type -> b, or b <- type - a.
bool parseRelation(Parser& parser, PrStringView statement) {
    size_t at = 0;
    PrStringView first{};
    if (!readName(statement, at, "-<:", &first))
        return mermaidParseFail(parser, "unknown statement");
    mermaidSkipSpace(statement, &at);
    PrStringView rest = mermaidAfter(statement, at);
    bool reversed = prStringStartsWith(rest, "<-");
    if (!reversed && !prStringStartsWith(rest, "-"))
        return mermaidParseFail(parser, "unknown statement");
    at += reversed ? 2 : 1;
    mermaidSkipSpace(statement, &at);
    size_t start = at;
    while (at < statement.size && prAsciiIsLetter(statement.data[at]))
        ++at;
    PrStringView type{statement.data + start, at - start};
    uint32_t relation = MermaidRequirementRelation_Count;
    for (uint32_t r = 0; r < MermaidRequirementRelation_Count; ++r) {
        if (prAsciiEqualsFold(type, kRelationNames[r]))
            relation = r;
    }
    if (relation == MermaidRequirementRelation_Count)
        return mermaidParseFail(parser, "unknown kind of relation");
    mermaidSkipSpace(statement, &at);
    rest = mermaidAfter(statement, at);
    bool closes = prStringStartsWith(rest, "->");
    if (reversed)
        closes = prStringStartsWith(rest, "-") && !closes;
    if (!closes)
        return mermaidParseFail(parser, "a relation's arrow does not close");
    at += reversed ? 1 : 2;
    PrStringView second{};
    if (!readName(statement, at, "-<:", &second))
        return mermaidParseFail(parser, "a relation names no second block");
    if (prStringTrim(mermaidAfter(statement, at)).size > 0)
        return mermaidParseFail(parser, "unexpected text after a relation");
    parser.links.push(PendingLink{
        .source = reversed ? second : first,
        .target = reversed ? first : second,
        .relation = MermaidRequirementRelation(relation),
        .line = parser.line,
        .column = parser.column,
    });
    return true;
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidRequirementDiagram& diagram = *parser.diagram;
    if (parser.inBlock) {
        if (statement == "}") {
            closeBlock(parser);
            return true;
        }
        return parseField(parser, statement);
    }
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    uint8_t kind = 0;
    if (lookup(kKinds, word, &kind))
        return parseBlockStart(parser, MermaidRequirementKind(kind), rest);
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
    if (prAsciiEqualsFold(word, "class")) {
        PrStringView ids{};
        PrStringView names{};
        mermaidSplitWord(rest, &ids, &names);
        mermaidEachName(ids, [&](PrStringView id) {
            mermaidEachName(names, [&](PrStringView name) {
                parser.classes.push(PendingClass{id, name});
            });
        });
        return true;
    }
    if (prAsciiEqualsFold(word, "style")) {
        PrStringView ids{};
        PrStringView declarations{};
        mermaidSplitWord(rest, &ids, &declarations);
        mermaidEachName(ids, [&](PrStringView id) {
            parser.ownStyles.push(PendingStyle{id, declarations});
        });
        return true;
    }
    // name:::class, or a relation.
    size_t at = 0;
    PrStringView name{};
    if (readName(statement, at, "-<:", &name) && prStringStartsWith(mermaidAfter(statement, at), ":::")) {
        readClasses(parser, statement, at, name);
        if (prStringTrim(mermaidAfter(statement, at)).size > 0)
            return mermaidParseFail(parser, "unexpected text after a block's classes");
        return true;
    }
    return parseRelation(parser, statement);
}

// Returns the block named `name`, or kMermaidNone if the source does not define it.
uint32_t findBlock(const Parser& parser, PrStringView name) {
    return mermaidNamesFind(parser.blockNames, name);
}

// Gives each relation its blocks. Fails at the first relation that names a block that the source does not define.
bool resolveLinks(Parser& parser) {
    for (const PendingLink& pending : parser.links.span()) {
        uint32_t source = findBlock(parser, pending.source);
        uint32_t target = findBlock(parser, pending.target);
        if (source == kMermaidNone || target == kMermaidNone) {
            parser.line = pending.line;
            parser.column = pending.column;
            return mermaidParseFail(parser, "a relation names a block that the diagram does not define");
        }
        parser.diagram->links.push(MermaidRequirementLink{source, target, pending.relation});
    }
    return true;
}

// Gives each block its style: the default class, the classes that it is given in source order, then its style
// statements. A statement that names a block that the source does not define is ignored.
void resolveStyles(Parser& parser) {
    MermaidRequirementDiagram& diagram = *parser.diagram;
    for (const PendingClass& assigned : parser.classes.span()) {
        uint32_t block = findBlock(parser, assigned.block);
        if (block != kMermaidNone)
            mermaidStyleSheetAssign(parser.styles, block, assigned.name);
    }
    for (const PendingStyle& own : parser.ownStyles.span()) {
        uint32_t block = findBlock(parser, own.block);
        if (block != kMermaidNone)
            mermaidStyleSheetSetOwn(parser.styles, block, own.declarations);
    }
    mermaidStyleSheetFinish(parser.styles, diagram.blocks.count);
    for (uint32_t b = 0; b < diagram.blocks.count; ++b)
        mermaidStyleSheetResolve(parser.styles, b, true, diagram.blocks.data[b].style);
}

bool parse(Parser& parser, PrStringView source) {
    MermaidPreamble preamble = mermaidPreambleRead(source);
    parser.diagram->title = addText(parser, preamble.title);
    parser.line = preamble.header.number;
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Requirement, nullptr))
        return false;
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        if (!parseStatement(parser, line.content))
            return false;
    }
    if (parser.inBlock) {
        parser.line = parser.blockLine;
        parser.column = 0;
        return mermaidParseFail(parser, "a block has no closing brace");
    }
    if (!resolveLinks(parser))
        return false;
    resolveStyles(parser);
    return true;
}

} // namespace

PrStatus mermaidRequirementParse(MermaidRequirementDiagram& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    diagram.direction = PrGraphDirection_Down;
    diagram.title = MermaidSpan{};
    diagram.text.clear();
    diagram.blocks.clear();
    diagram.links.clear();
    Parser parser{};
    parser.error = error;
    parser.diagram = &diagram;
    bool parsed = parse(parser, source);
    mermaidStyleSheetReset(parser.styles);
    mermaidNamesReset(parser.blockNames);
    parser.classes.reset();
    parser.ownStyles.reset();
    parser.links.reset();
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidRequirementText(const MermaidRequirementDiagram& diagram, MermaidSpan span) {
    return mermaidTextAt(diagram.text, span);
}

PrStringView mermaidRequirementRelationName(MermaidRequirementRelation relation) {
    return relation < MermaidRequirementRelation_Count ? kRelationNames[relation] : PrStringView{};
}

void mermaidRequirementReset(MermaidRequirementDiagram& diagram) {
    diagram.text.reset();
    diagram.blocks.reset();
    diagram.links.reset();
}

} // namespace pr_impl
