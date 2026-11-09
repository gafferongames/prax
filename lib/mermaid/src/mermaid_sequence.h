// Sequence diagrams: the model of a Mermaid sequence diagram, and the parser that builds it from the source of
// `sequenceDiagram`.

#pragma once

#include "pr/color.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How a participant is drawn. The glyphs of the UML kinds stand above their labels.
enum {
    MermaidActorKind_Participant,   // a box
    MermaidActorKind_Actor,         // a stick figure
    MermaidActorKind_Boundary,      // a circle with a T on its left
    MermaidActorKind_Control,       // a circle with an arrowhead on its top
    MermaidActorKind_Entity,        // a circle on a line
    MermaidActorKind_Database,      // an upright cylinder
    MermaidActorKind_Collections,   // a box in front of another
    MermaidActorKind_Queue,         // a cylinder on its side
};
typedef uint8_t MermaidActorKind;

// A participant, in the order that the source first names it.
struct MermaidActor {
    MermaidSpan id;
    MermaidSpan label;              // the description that `as` gives, or the ID
    MermaidActorKind kind;
    uint32_t box;                   // the box that holds the participant, or kMermaidNone
    bool wrap;                      // the label wraps to the participant's width

    // The steps of the messages that create and destroy the participant, or kMermaidNone. A participant that a message
    // creates starts at that message. One that a message destroys ends at it.
    uint32_t created;
    uint32_t destroyed;
};

// A box around participants.
struct MermaidActorBox {
    MermaidSpan label;
    bool hasColor;                  // false for a transparent box
    PrColor8 color;
    bool wrap;                      // the label wraps to the width of the box
};

// The line and heads of a message.
enum {
    MermaidArrow_Solid,             // ->>
    MermaidArrow_Dotted,            // -->>
    MermaidArrow_SolidOpen,         // ->
    MermaidArrow_DottedOpen,        // -->
    MermaidArrow_SolidCross,        // -x
    MermaidArrow_DottedCross,       // --x
    MermaidArrow_SolidPoint,        // -)
    MermaidArrow_DottedPoint,       // --)
    MermaidArrow_BothSolid,         // <<->>
    MermaidArrow_BothDotted,        // <<-->>
};
typedef uint8_t MermaidArrow;

// Where a note sits.
enum {
    MermaidNotePlace_LeftOf,
    MermaidNotePlace_RightOf,
    MermaidNotePlace_Over,
};
typedef uint8_t MermaidNotePlace;

// The kinds of block, which frame the statements between their start and their end.
enum {
    MermaidBlock_Loop,
    MermaidBlock_Alt,               // with else sections
    MermaidBlock_Opt,
    MermaidBlock_Par,               // with and sections
    MermaidBlock_Critical,          // with option sections
    MermaidBlock_Break,
    MermaidBlock_Rect,              // a background of a color, with no frame
};
typedef uint8_t MermaidBlock;

// What a statement of the diagram does, in source order.
enum {
    MermaidStepKind_Message,
    MermaidStepKind_Note,
    MermaidStepKind_Activate,           // `from` becomes active
    MermaidStepKind_Deactivate,         // `from`'s last activation ends
    MermaidStepKind_BlockStart,
    MermaidStepKind_BlockSection,       // else, and, or option
    MermaidStepKind_BlockEnd,
    MermaidStepKind_Autonumber,
};
typedef uint8_t MermaidStepKind;

// A statement of the diagram.
struct MermaidStep {
    MermaidStepKind kind;
    uint32_t from;                  // Message, Note, Activate, Deactivate
    uint32_t to;                    // Message; Note: the last actor that it covers
    MermaidSpan text;               // a message's or note's text, a block's or section's title, or a rect's color
    bool wrap;                      // Message, Note: the text wraps
    MermaidArrow arrow;             // Message
    bool activates;                 // Message: its target becomes active, as + writes
    MermaidNotePlace place;         // Note
    MermaidBlock block;             // BlockStart, BlockSection, BlockEnd
    float start;                    // Autonumber: the first number, or zero to keep counting
    float step;                     // Autonumber: the increment, or zero to keep it
    bool visible;                   // Autonumber: false for `autonumber off`
};

// A sequence diagram, as its source describes it.
struct MermaidSequence {
    MermaidSpan title;              // the front matter's or `title` statement's title, or empty
    RawArray<char> text;            // the characters of every span
    RawArray<MermaidActor> actors;
    RawArray<MermaidActorBox> boxes;
    RawArray<MermaidStep> steps;
};

// Parses the source of a sequence diagram into `diagram`, emptying it first. If the source does not parse, returns
// Syntax and fills a non-null `error`.
PrStatus mermaidSequenceParse(MermaidSequence& diagram, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the diagram.
PrStringView mermaidSequenceText(const MermaidSequence& diagram, MermaidSpan span);

// Frees the diagram's storage.
void mermaidSequenceReset(MermaidSequence& diagram);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
