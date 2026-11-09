// Parsing: what every kind of diagram's parser reads with. A source is a preamble, of front matter, directives and
// comments up to the header line that names the diagram's kind, then a body of statements. The preamble is read once,
// and a parser walks the body's lines, which come without the comments, directives and accessibility statements that
// mermaid.js removes before any diagram's grammar sees them.
//
// A parser's state begins with MermaidParseState, which records the first error. Names, text and style statements read
// with the helpers here, so that each kind's parser holds only its own grammar.

#pragma once

#include "pr/color.h"
#include "pr/graph.h"
#include "pr/hash_index.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Errors

// Where a parser is in its source, and the first error that it met. A parser's state begins with it.
struct MermaidParseState {
    PrMermaidError* PR_NULLABLE error;
    bool failed;
    uint32_t line;                      // from one
    uint32_t column;                    // of the statement's first byte, from zero
};

// Records `message` at the state's line and column, unless an error is already recorded. Returns false, so that a
// parser can return its result.
bool mermaidParseFail(MermaidParseState& state, const char* message);

// ---------------------------------------------------------------------------------------------------------------------
// Lines

// A line of a source, without its line ending.
struct MermaidLine {
    uint32_t number;                    // from one, or zero for no line
    PrStringView text;
    PrStringView content;               // the text without white space at either end
    uint32_t column;                    // of the content's first byte in the text
};

// What a source says before its diagram's statements: its front matter, its directives, and the line of its header.
struct MermaidPreamble {
    PrStringView frontMatter;           // the text between the front matter's lines of dashes
    PrStringView title;                 // the front matter's title, without its quotes, or empty
    MermaidLine header;                 // the header line, whose number is zero if the source has none
    size_t body;                        // the offset in the source of the line after the header

    // The color scheme that the theme of the front matter or a directive names, and the sequence diagram's wrap
    // setting. A theme other than dark names the light scheme.
    bool hasScheme;
    PrColorScheme scheme;
    bool hasWrap;
    bool wrap;
};

// Reads the preamble of `source`. A directive, %%{ ... }%%, may span lines.
MermaidPreamble mermaidPreambleRead(PrStringView source);

// The lines of a source from an offset, read in turn.
struct MermaidLines {
    PrStringView source;
    size_t at;                          // the offset of the next line, past the end once every line is read
    uint32_t number;                    // the number of the last line read
};

// Returns the lines of the body of `source`, after its header line.
MermaidLines mermaidLinesOfBody(PrStringView source, const MermaidPreamble& preamble);

// Reads the next statement line into `out`, past blank lines, comments, directives and the accessibility statements
// (accTitle and accDescr, with a block's lines), which prMermaidAccessibleTitle reports instead. Returns false when no
// line is left.
bool mermaidLinesNext(MermaidLines& lines, MermaidLine* out);

// Reads the next line into `out`, whatever it holds. Returns false when no line is left.
bool mermaidLinesNextRaw(MermaidLines& lines, MermaidLine* out);

// Extends `line` over the next line of the source, with the line ending between them, as a statement does that runs on
// past its line. Returns false when no line is left.
bool mermaidLinesExtend(MermaidLines& lines, MermaidLine* line);

// Returns true if `header` starts with a keyword that names a kind of diagram, ignoring case, and writes the kind, and
// to a non-null `rest` the header after the keyword, without white space at either end. A keyword ends at white space,
// a semicolon or a colon.
bool mermaidKindOfHeader(PrStringView header, PrMermaidKind* kind, PrStringView* PR_NULLABLE rest);

// Places the parser at the header of `preamble`, and returns true and writes the header after its keyword to `rest` if
// the header names `kind`, if `rest` is not null. Otherwise fails, at the first line if the source has no header.
bool mermaidParseHeader(MermaidParseState& parser, const MermaidPreamble& preamble, PrMermaidKind kind,
    PrStringView* PR_NULLABLE rest);

// ---------------------------------------------------------------------------------------------------------------------
// Text

// The text of `text` from `at`, which is empty if `at` is past its end.
PrStringView mermaidAfter(PrStringView text, size_t at);

// The text of `text` before `at`, which is all of it if `at` is past its end.
PrStringView mermaidBefore(PrStringView text, size_t at);

// Moves `at` past the white space in `text` from it.
void mermaidSkipSpace(PrStringView text, size_t* at);

// Returns true if `text` starts with `word`, followed by its end or white space.
bool mermaidStartsWithWord(PrStringView text, PrStringView word);

// Splits `text` at its first run of white space into a word and the rest, without white space at either end.
void mermaidSplitWord(PrStringView text, PrStringView* word, PrStringView* rest);

// Reads the double-quoted text that `text` starts with, without its quotes, and writes what follows it, without white
// space at either end. Returns false if `text` starts with no quote, or its quote is not closed.
bool mermaidQuotedRead(PrStringView text, PrStringView* quoted, PrStringView* rest);

// `text` without white space at either end, and then without the double quotes around it, if it has them.
PrStringView mermaidUnquoted(PrStringView text);

// Calls `fn` with each name in a comma-separated list, without white space at either end. Empty names are skipped.
template <typename Fn>
void mermaidEachName(PrStringView list, Fn fn) {
    size_t start = 0;
    for (size_t i = 0; i <= list.size; ++i) {
        if (i < list.size && list.data[i] != ',')
            continue;
        PrStringView name = prStringTrim(PrStringView{list.data + start, i - start});
        if (name.size > 0)
            fn(name);
        start = i + 1;
    }
}

// Appends `text` to a diagram's text, and returns where it is.
MermaidSpan mermaidTextAdd(RawArray<char>& text, PrStringView added);

// The text of a diagram that `span` names.
PrStringView mermaidTextAt(const RawArray<char>& text, MermaidSpan span);

// Reads a direction, TB, TD, BT, LR or RL, as direction statements and headers write it.
bool mermaidDirectionParse(PrStringView text, PrGraphDirection* out);

// Parses the number at the start of `text`, as prF64ParsePrefix does, and returns the bytes that it takes. A number
// whose magnitude passes 1e30 takes none, so that what a diagram computes from its numbers stays finite in floats.
size_t mermaidNumberParse(PrStringView text, double* out);

// Returns true if a click or link statement's URL may be given to a host as an item's link. A URL whose scheme is
// javascript, vbscript or data may not, as mermaid.js's sanitizer refuses them. ASCII control characters and white
// space, which a browser ignores in a scheme, do not hide one.
bool mermaidLinkIsSafe(PrStringView url);

// Finds the setting `key` in a configuration's text, in YAML (`key: value`) or in a directive's JSON (`"key":
// "value"`), and returns true and sets `value` to its word if it is set. A key that only starts with `key`, such as
// themeVariables for theme, does not match.
bool mermaidConfigValue(PrStringView text, PrStringView key, PrStringView* value);

// ---------------------------------------------------------------------------------------------------------------------
// Names

// A lookup from names to the numbers of what they name, such as nodes by their IDs. The names view text that outlives
// the lookup, usually the source.
struct MermaidNames {
    RawArray<PrStringView> names;       // by entry
    RawArray<uint32_t> values;          // by entry
    HashIndex slots;                    // the entries, by the hashes of their names
};

// The number that `name` names, or kMermaidNone.
uint32_t mermaidNamesFind(const MermaidNames& names, PrStringView name);

// Makes `name` name `value`, in place of any number that it named.
void mermaidNamesSet(MermaidNames& names, PrStringView name, uint32_t value);

// Frees the lookup's memory.
void mermaidNamesReset(MermaidNames& names);

// ---------------------------------------------------------------------------------------------------------------------
// Styles

// A class that a statement gives a node, by the class's name, which views the source.
struct MermaidAssignedClass {
    uint32_t node;
    PrStringView name;
};

// The styles that a diagram's statements give its nodes, by their indices: the classes that classDef defines, the
// classes that class, cssClass and ::: statements assign, and the declarations of each node's style statements.
// Resolving a node's style merges the default class, then its classes in the order they were assigned, then its own
// declarations. Names view the source, which must outlive the sheet.
struct MermaidStyleSheet {
    MermaidNames classNames;            // the index in `classes` of each class's name
    RawArray<MermaidPaint> classes;
    RawArray<MermaidAssignedClass> assigned;
    RawArray<MermaidPaint> own;         // by node, as far as a node has declarations

    // The assigned classes ordered by node, with the start of each node's in `assignedStart`, which resolving builds.
    RawArray<uint32_t> byNode;
    RawArray<uint32_t> assignedStart;
};

// The style of the class `name`, or null if no classDef statement defines it.
const MermaidPaint* PR_NULLABLE mermaidStyleSheetClass(const MermaidStyleSheet& sheet, PrStringView name);

// Merges the declarations of a classDef statement into the class `name`.
void mermaidStyleSheetDefine(MermaidStyleSheet& sheet, PrStringView name, PrStringView declarations);

// Assigns the class `name` to `node`.
void mermaidStyleSheetAssign(MermaidStyleSheet& sheet, uint32_t node, PrStringView name);

// Merges the declarations of a style statement into the node's own style.
void mermaidStyleSheetSetOwn(MermaidStyleSheet& sheet, uint32_t node, PrStringView declarations);

// Orders the assigned classes by node, for nodes below `nodeCount`. Call it once the statements are read, before
// mermaidStyleSheetResolve.
void mermaidStyleSheetFinish(MermaidStyleSheet& sheet, uint32_t nodeCount);

// Merges the style of `node` into `style`: the default class if `withDefault`, its assigned classes, then its own
// declarations.
void mermaidStyleSheetResolve(const MermaidStyleSheet& sheet, uint32_t node, bool withDefault, MermaidPaint& style);

// Frees the sheet's memory.
void mermaidStyleSheetReset(MermaidStyleSheet& sheet);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
