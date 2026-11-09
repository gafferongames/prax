// Sequence diagrams (mermaid_sequence.h). The parser reads the source one line at a time, and semicolons separate the
// statements on a line. A statement starts with a keyword, or is a message: a participant, an arrow, a participant, and
// the message's text after a colon. Participants that a message or note names first are declared by it. Keywords ignore
// case, as in mermaid.js.

#include "mermaid/src/mermaid_sequence.h"

#include "pr/ascii.h"
#include "pr/color_css.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// The deepest that blocks nest, and the most activations that may be open at once. The layout updates every open block
// and activation as it places each statement.
constexpr uint32_t kMaxBlockDepth = 64;
constexpr uint32_t kMaxOpenActivations = 64;

struct Parser : MermaidParseState {
    MermaidSequence* diagram;
    MermaidNames actorIds;                  // the participant of each ID, which views the source
    RawArray<uint32_t> activations;         // by participant: its open activations
    uint32_t openActivations;
    RawArray<MermaidBlock> blocks;          // the open blocks, innermost last
    RawArray<uint32_t> blockLines;          // by open block: the line that opens it
    uint32_t box;                           // the open box, or kMermaidNone
    uint32_t boxBlocks;                     // the number of blocks open when the box opened
    uint32_t boxLine;                       // the line that opens the box
    bool wrap;                              // text wraps unless a wrap: or nowrap: prefix says otherwise

    // The participants that a create or destroy statement names, which the next message must create or destroy, or
    // kMermaidNone.
    uint32_t creating;
    uint32_t destroying;
};

// The statement's first word, up to space or a colon, and what follows it, trimmed.
PrStringView firstWord(PrStringView text, PrStringView* rest) {
    size_t end = 0;
    while (end < text.size && !prAsciiIsSpace(text.data[end]) && text.data[end] != ':')
        ++end;
    *rest = prStringTrim(mermaidAfter(text, end));
    return PrStringView{text.data, end};
}

MermaidSpan addText(Parser& parser, PrStringView text) {
    return mermaidTextAdd(parser.diagram->text, text);
}

// Adds the text after a statement's colon, without Mermaid's wrap: or nowrap: prefix. Sets `wrap` to true if the text
// wraps, as the prefix or else the diagram's configuration says.
MermaidSpan addMessageText(Parser& parser, PrStringView text, bool* wrap) {
    text = prStringTrim(text);
    *wrap = parser.wrap;
    if (prAsciiStartsWithFold(text, "wrap:")) {
        text = prStringTrim(mermaidAfter(text, 5));
        *wrap = true;
    } else if (prAsciiStartsWithFold(text, "nowrap:")) {
        text = prStringTrim(mermaidAfter(text, 7));
        *wrap = false;
    }
    return addText(parser, text);
}

// Returns the participant named `id`, or kMermaidNone if the source has not named it yet.
uint32_t findActor(const Parser& parser, PrStringView id) {
    return mermaidNamesFind(parser.actorIds, id);
}

// Returns the participant named `id`, which is declared as a plain participant if the source has not named it yet.
uint32_t actorOf(Parser& parser, PrStringView id) {
    MermaidSequence& diagram = *parser.diagram;
    uint32_t found = findActor(parser, id);
    if (found != kMermaidNone)
        return found;
    MermaidSpan span = addText(parser, id);
    diagram.actors.push(MermaidActor{
        .id = span,
        .label = span,
        .kind = MermaidActorKind_Participant,
        .box = parser.box,
        .wrap = parser.wrap,
        .created = kMermaidNone,
        .destroyed = kMermaidNone,
    });
    mermaidNamesSet(parser.actorIds, id, diagram.actors.count - 1);
    parser.activations.push(0);
    return diagram.actors.count - 1;
}

// Starts an activation of `actor`, which fails if too many are open, or ends its last one if it has one.
bool activate(Parser& parser, uint32_t actor) {
    if (parser.openActivations >= kMaxOpenActivations)
        return mermaidParseFail(parser, "activations nest too deeply");
    ++parser.activations.data[actor];
    ++parser.openActivations;
    parser.diagram->steps.push(MermaidStep{.kind = MermaidStepKind_Activate, .from = actor});
    return true;
}
void deactivate(Parser& parser, uint32_t actor) {
    if (parser.activations.data[actor] > 0) {
        --parser.activations.data[actor];
        --parser.openActivations;
    }
    parser.diagram->steps.push(MermaidStep{.kind = MermaidStepKind_Deactivate, .from = actor});
}

// The ID of a declaration: participant ID[@{...}] [as Label].
PrStringView declaredId(PrStringView rest) {
    for (size_t i = 0; i < rest.size; ++i) {
        if (prAsciiIsSpace(rest.data[i]) || rest.data[i] == '@')
            return PrStringView{rest.data, i};
    }
    return rest;
}

// Reads the "type" of a participant's @{...} metadata.
bool kindOfMetadata(PrStringView metadata, MermaidActorKind* out) {
    size_t at = prStringFind(metadata, "type");
    if (at >= metadata.size)
        return false;
    PrStringView rest = mermaidAfter(metadata, at + 4);
    size_t start = 0;
    while (start < rest.size && (rest.data[start] == '"' || rest.data[start] == '\'' || rest.data[start] == ':' ||
            prAsciiIsSpace(rest.data[start])))
        ++start;
    size_t end = start;
    while (end < rest.size && prAsciiIsLetter(rest.data[end]))
        ++end;
    PrStringView kind{rest.data + start, end - start};
    struct Named {
        PrStringView name;
        MermaidActorKind kind;
    };
    static const Named kKinds[] = {
        {"participant", MermaidActorKind_Participant}, {"actor", MermaidActorKind_Actor},
        {"boundary", MermaidActorKind_Boundary}, {"control", MermaidActorKind_Control},
        {"entity", MermaidActorKind_Entity}, {"database", MermaidActorKind_Database},
        {"collections", MermaidActorKind_Collections}, {"queue", MermaidActorKind_Queue},
    };
    for (const Named& named : kKinds) {
        if (prAsciiEqualsFold(kind, named.name)) {
            *out = named.kind;
            return true;
        }
    }
    return false;
}

// participant ID[@{...}] [as Label], or the same with actor. Sets `actor` to the participant that it declares.
bool parseDeclaration(Parser& parser, PrStringView rest, MermaidActorKind kind, uint32_t* actor) {
    MermaidSequence& diagram = *parser.diagram;
    PrStringView label{};
    // " as " ends the ID, and the label runs to the end.
    for (size_t i = 0; i + 4 <= rest.size; ++i) {
        if (prAsciiIsSpace(rest.data[i]) && prAsciiEqualsFold(PrStringView{rest.data + i + 1, 2}, "as") &&
                prAsciiIsSpace(rest.data[i + 3])) {
            label = prStringTrim(mermaidAfter(rest, i + 4));
            rest = PrStringView{rest.data, i};
            break;
        }
    }
    PrStringView id = prStringTrim(rest);
    size_t metadata = prStringFind(id, "@{");
    if (metadata < id.size) {
        kindOfMetadata(mermaidAfter(id, metadata), &kind);
        id = prStringTrim(PrStringView{id.data, metadata});
    }
    if (id.size == 0)
        return mermaidParseFail(parser, "a participant has no name");
    *actor = actorOf(parser, id);
    MermaidActor& declared = diagram.actors.data[*actor];
    declared.kind = kind;
    declared.box = parser.box;
    if (label.size > 0)
        declared.label = addMessageText(parser, label, &declared.wrap);
    return true;
}

// box [color] [label]: a color that CSS reads, and a label, either of which may be missing.
bool parseBox(Parser& parser, PrStringView rest) {
    if (parser.box != kMermaidNone)
        return mermaidParseFail(parser, "a box is inside another box");
    MermaidActorBox box{};
    size_t colorEnd = 0;
    if (prAsciiStartsWithFold(rest, "rgb") || prAsciiStartsWithFold(rest, "hsl")) {
        size_t close = prStringFindByte(rest, ')');
        colorEnd = close < rest.size ? close + 1 : 0;
    } else {
        while (colorEnd < rest.size && !prAsciiIsSpace(rest.data[colorEnd]))
            ++colorEnd;
    }
    PrColor8 color{};
    PrStringView label = rest;
    if (colorEnd > 0 && prColor8FromCss(PrStringView{rest.data, colorEnd}, &color)) {
        box.hasColor = color.a > 0;
        box.color = color;
        label = prStringTrim(mermaidAfter(rest, colorEnd));
    }
    box.label = addMessageText(parser, label, &box.wrap);
    parser.diagram->boxes.push(box);
    parser.box = parser.diagram->boxes.count - 1;
    parser.boxLine = parser.line;
    parser.boxBlocks = parser.blocks.count;
    return true;
}

// Note left of A: text, Note right of A: text, Note over A: text, or Note over A,B: text.
bool parseNote(Parser& parser, PrStringView rest) {
    MermaidStep step{.kind = MermaidStepKind_Note};
    if (prAsciiStartsWithFold(rest, "left of")) {
        step.place = MermaidNotePlace_LeftOf;
        rest = prStringTrim(mermaidAfter(rest, 7));
    } else if (prAsciiStartsWithFold(rest, "right of")) {
        step.place = MermaidNotePlace_RightOf;
        rest = prStringTrim(mermaidAfter(rest, 8));
    } else if (prAsciiStartsWithFold(rest, "over")) {
        step.place = MermaidNotePlace_Over;
        rest = prStringTrim(mermaidAfter(rest, 4));
    } else {
        return mermaidParseFail(parser, "a note is not left of, right of or over a participant");
    }
    size_t colon = prStringFindByte(rest, ':');
    PrStringView actors = mermaidBefore(rest, colon);
    size_t comma = prStringFindByte(actors, ',');
    PrStringView first = prStringTrim(mermaidBefore(actors, comma));
    PrStringView second = prStringTrim(mermaidAfter(actors, comma + 1));
    if (first.size == 0)
        return mermaidParseFail(parser, "a note names no participant");
    step.from = actorOf(parser, first);
    step.to = comma < actors.size && second.size > 0 ? actorOf(parser, second) : step.from;
    step.text = addMessageText(parser, mermaidAfter(rest, colon + 1), &step.wrap);
    parser.diagram->steps.push(step);
    return true;
}

// The arrows, longest first where one starts another.
struct ArrowToken {
    PrStringView text;
    MermaidArrow arrow;
};

constexpr ArrowToken kArrows[] = {
    {"<<-->>", MermaidArrow_BothDotted}, {"<<->>", MermaidArrow_BothSolid}, {"-->>", MermaidArrow_Dotted},
    {"->>", MermaidArrow_Solid}, {"--x", MermaidArrow_DottedCross}, {"-x", MermaidArrow_SolidCross},
    {"--)", MermaidArrow_DottedPoint}, {"-)", MermaidArrow_SolidPoint}, {"-->", MermaidArrow_DottedOpen},
    {"->", MermaidArrow_SolidOpen},
};

// A ->> B: text, with an optional + or - after the arrow, which activates B or deactivates A.
bool parseMessage(Parser& parser, PrStringView statement) {
    MermaidSequence& diagram = *parser.diagram;
    for (size_t i = 0; i < statement.size; ++i) {
        for (const ArrowToken& token : kArrows) {
            if (!prStringStartsWith(mermaidAfter(statement, i), token.text))
                continue;
            PrStringView source = prStringTrim(PrStringView{statement.data, i});
            PrStringView rest = prStringTrim(mermaidAfter(statement, i + token.text.size));
            bool activates = false;
            bool deactivates = false;
            if (rest.size > 0 && (rest.data[0] == '+' || rest.data[0] == '-')) {
                activates = rest.data[0] == '+';
                deactivates = rest.data[0] == '-';
                rest = prStringTrim(mermaidAfter(rest, 1));
            }
            size_t colon = prStringFindByte(rest, ':');
            PrStringView target = prStringTrim(mermaidBefore(rest, colon));
            if (source.size == 0 || target.size == 0)
                return mermaidParseFail(parser, "a message does not name both of its participants");
            MermaidStep step{
                .kind = MermaidStepKind_Message,
                .from = actorOf(parser, source),
                .to = actorOf(parser, target),
                .arrow = token.arrow,
                .activates = activates,
            };
            step.text = addMessageText(parser, mermaidAfter(rest, colon + 1), &step.wrap);
            uint32_t index = diagram.steps.count;
            if (parser.creating != kMermaidNone) {
                if (step.to != parser.creating)
                    return mermaidParseFail(parser, "a created participant is not the target of the next message");
                diagram.actors.data[parser.creating].created = index;
                parser.creating = kMermaidNone;
            } else if (parser.destroying != kMermaidNone) {
                if (step.from != parser.destroying && step.to != parser.destroying)
                    return mermaidParseFail(parser, "a destroyed participant is not in the next message");
                diagram.actors.data[parser.destroying].destroyed = index;
                parser.destroying = kMermaidNone;
            }
            diagram.steps.push(step);
            // As Mermaid does, the activation follows the message.
            if (activates)
                return activate(parser, step.to);
            if (deactivates)
                deactivate(parser, step.from);
            return true;
        }
    }
    return mermaidParseFail(parser, "unknown statement");
}

struct BlockKeyword {
    PrStringView word;
    MermaidBlock block;
    bool starts;                    // false for a section's keyword
};

constexpr BlockKeyword kBlockKeywords[] = {
    {"loop", MermaidBlock_Loop, true}, {"alt", MermaidBlock_Alt, true}, {"opt", MermaidBlock_Opt, true},
    {"par", MermaidBlock_Par, true}, {"par_over", MermaidBlock_Par, true},
    {"critical", MermaidBlock_Critical, true}, {"break", MermaidBlock_Break, true},
    {"rect", MermaidBlock_Rect, true}, {"else", MermaidBlock_Alt, false}, {"and", MermaidBlock_Par, false},
    {"option", MermaidBlock_Critical, false},
};

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidSequence& diagram = *parser.diagram;
    PrStringView rest{};
    PrStringView word = firstWord(statement, &rest);
    uint32_t actor = kMermaidNone;
    if (prAsciiEqualsFold(word, "participant"))
        return parseDeclaration(parser, rest, MermaidActorKind_Participant, &actor);
    if (prAsciiEqualsFold(word, "actor"))
        return parseDeclaration(parser, rest, MermaidActorKind_Actor, &actor);
    if (prAsciiEqualsFold(word, "create")) {
        // The next message creates the participant, which is declared here. Its ID must be new, as Mermaid requires.
        PrStringView declared{};
        PrStringView kind = firstWord(rest, &declared);
        bool participant = prAsciiEqualsFold(kind, "participant");
        if (!participant && !prAsciiEqualsFold(kind, "actor"))
            return mermaidParseFail(parser, "create names neither a participant nor an actor");
        if (findActor(parser, declaredId(declared)) != kMermaidNone)
            return mermaidParseFail(parser, "a created participant is already named");
        MermaidActorKind actorKind = participant ? MermaidActorKind_Participant : MermaidActorKind_Actor;
        if (!parseDeclaration(parser, declared, actorKind, &actor))
            return false;
        parser.creating = actor;
        return true;
    }
    if (prAsciiEqualsFold(word, "destroy")) {
        if (rest.size == 0)
            return mermaidParseFail(parser, "destroy names no participant");
        parser.destroying = actorOf(parser, rest);
        return true;
    }
    if (prAsciiEqualsFold(word, "links") || prAsciiEqualsFold(word, "link") ||
            prAsciiEqualsFold(word, "properties") || prAsciiEqualsFold(word, "details"))
        return true;
    if (prAsciiEqualsFold(word, "title")) {
        if (rest.size > 0 && rest.data[0] == ':')
            rest = prStringTrim(mermaidAfter(rest, 1));
        diagram.title = addText(parser, rest);
        return true;
    }
    if (prAsciiEqualsFold(word, "box"))
        return parseBox(parser, rest);
    if (prAsciiEqualsFold(word, "note"))
        return parseNote(parser, rest);
    if (prAsciiEqualsFold(word, "activate") || prAsciiEqualsFold(word, "deactivate")) {
        if (rest.size == 0)
            return mermaidParseFail(parser, "an activation names no participant");
        actor = actorOf(parser, rest);
        if (prAsciiEqualsFold(word, "activate"))
            return activate(parser, actor);
        deactivate(parser, actor);
        return true;
    }
    if (prAsciiEqualsFold(word, "autonumber")) {
        MermaidStep step{.kind = MermaidStepKind_Autonumber, .visible = true};
        if (prAsciiEqualsFold(rest, "off")) {
            step.visible = false;
        } else if (rest.size > 0) {
            PrStringView second{};
            PrStringView first = firstWord(rest, &second);
            double start = 0;
            double increment = 0;
            if (mermaidNumberParse(first, &start) != first.size ||
                    (second.size > 0 && mermaidNumberParse(second, &increment) != second.size))
                return mermaidParseFail(parser, "autonumber's start and step are not numbers");
            step.start = float(start);
            step.step = float(increment);
        }
        diagram.steps.push(step);
        return true;
    }
    if (prAsciiEqualsFold(word, "end")) {
        if (parser.blocks.count > (parser.box != kMermaidNone ? parser.boxBlocks : 0)) {
            diagram.steps.push(MermaidStep{.kind = MermaidStepKind_BlockEnd, .block = parser.blocks.last()});
            parser.blocks.pop();
            parser.blockLines.pop();
            return true;
        }
        if (parser.box != kMermaidNone) {
            parser.box = kMermaidNone;
            return true;
        }
        return mermaidParseFail(parser, "end closes nothing");
    }
    for (const BlockKeyword& keyword : kBlockKeywords) {
        if (!prAsciiEqualsFold(word, keyword.word))
            continue;
        MermaidStep step{
            .kind = MermaidStepKind(keyword.starts ? MermaidStepKind_BlockStart : MermaidStepKind_BlockSection),
        };
        step.block = keyword.block;
        step.text = addMessageText(parser, rest, &step.wrap);
        if (keyword.starts) {
            if (parser.blocks.count >= kMaxBlockDepth)
                return mermaidParseFail(parser, "a block nests too deeply");
            parser.blocks.push(keyword.block);
            parser.blockLines.push(parser.line);
        } else if (parser.blocks.count == 0 || parser.blocks.last() != keyword.block) {
            return mermaidParseFail(parser, "a section is outside its block");
        }
        diagram.steps.push(step);
        return true;
    }
    return parseMessage(parser, statement);
}

// Reads the statements of a line. Semicolons end statements, except in a title, which runs to the end of its line.
bool parseLine(Parser& parser, const MermaidLine& line) {
    PrStringView content = line.content;
    for (size_t at = 0; at <= content.size;) {
        size_t stop = at;
        bool whole = prAsciiStartsWithFold(mermaidAfter(content, at), "title");
        while (stop < content.size && (whole || content.data[stop] != ';'))
            ++stop;
        PrStringView statement = prStringTrim(PrStringView{content.data + at, stop - at});
        parser.column = uint32_t(statement.data - line.text.data);
        if (statement.size > 0 && !parseStatement(parser, statement))
            return false;
        at = stop + 1;
    }
    return true;
}

bool parse(Parser& parser, PrStringView source) {
    MermaidPreamble preamble = mermaidPreambleRead(source);
    parser.wrap = preamble.hasWrap && preamble.wrap;
    parser.diagram->title = addText(parser, preamble.title);
    parser.line = preamble.header.number;
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Sequence, nullptr))
        return false;
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        if (!parseLine(parser, line))
            return false;
    }
    if (parser.blocks.count > 0 || parser.box != kMermaidNone) {
        // The error names the innermost block or box that is left open.
        bool box = parser.box != kMermaidNone && parser.blocks.count <= parser.boxBlocks;
        parser.line = box ? parser.boxLine : parser.blockLines.last();
        parser.column = 0;
        return mermaidParseFail(parser, "a block or box has no end");
    }
    return true;
}

} // namespace

PrStatus mermaidSequenceParse(MermaidSequence& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    diagram.title = MermaidSpan{};
    diagram.text.clear();
    diagram.actors.clear();
    diagram.boxes.clear();
    diagram.steps.clear();
    Parser parser{};
    parser.error = error;
    parser.diagram = &diagram;
    parser.box = kMermaidNone;
    parser.creating = kMermaidNone;
    parser.destroying = kMermaidNone;
    bool parsed = parse(parser, source);
    mermaidNamesReset(parser.actorIds);
    parser.activations.reset();
    parser.blocks.reset();
    parser.blockLines.reset();
    return parsed ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidSequenceText(const MermaidSequence& diagram, MermaidSpan span) {
    return mermaidTextAt(diagram.text, span);
}

void mermaidSequenceReset(MermaidSequence& diagram) {
    diagram.text.reset();
    diagram.actors.reset();
    diagram.boxes.reset();
    diagram.steps.reset();
}

} // namespace pr_impl
