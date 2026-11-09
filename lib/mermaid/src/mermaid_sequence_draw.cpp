// Sequence diagrams as drawn (mermaid_sequence_draw.h). The layout is mermaid.js's sequence renderer, step by step, so
// that a diagram has the reference's proportions: participants side by side with room for the messages between
// neighbors, then the statements in source order down the page, each bumping a vertical position, and the participants
// again at the bottom. Open blocks and activations grow around what is placed inside them.

#include "mermaid/src/mermaid_sequence_draw.h"

#include "pr/color_css.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/path.h"

#include "mermaid/src/mermaid_sequence.h"

namespace pr_impl {

namespace {

// mermaid.js's sequence configuration, in px.
constexpr float kActivationWidth = 10;
constexpr float kMarginX = 50;              // diagramMarginX
constexpr float kMarginY = 10;              // diagramMarginY
constexpr float kActorMargin = 50;
constexpr float kActorWidth = 150;
constexpr float kActorHeight = 65;
constexpr float kBoxMargin = 10;
constexpr float kBoxTextMargin = 5;
constexpr float kNoteMargin = 10;
constexpr float kBottomMarginAdj = 1;
constexpr float kWrapPadding = 10;
constexpr float kLabelBoxWidth = 50;
constexpr float kLabelBoxHeight = 20;

// The size of all of a sequence diagram's text. mermaid.js's configuration gives its participants and notes 14 px, but
// the diagram's font size of 16 px replaces them.
constexpr float kFontSize = 16;

constexpr float kTitleSpace = 40;           // above the diagram, for its title
constexpr float kStickWidth = 36;           // a stick figure's arms
constexpr float kGlyphRadius = 22;          // the radius of the circle in a boundary, control or entity
constexpr float kCollectionsOffset = 6;     // the shift of the front box of collections, left and down
constexpr float kMessageWidth = 1.5f;
constexpr float kDash[2] = {3, 3};
constexpr float kLoopDash[2] = {2, 2};

// mermaid.js draws lifelines 0.5 wide, but its theme gives every line a width of 2, which wins.
constexpr float kLifelineWidth = 2;

// A sequence number's circle: 6 in the units of a line's width, which is 2 by the same rule.
constexpr float kNumberRadius = 12;

// A participant as placed.
struct ActorLayout {
    float x;                        // the left of its box
    float starty;                   // the top of its box at the top
    float width;
    float height;
    float margin;                   // the space after it, before the next participant
    float stopy;                    // the top of its box at the bottom
    MermaidLabelIndex label;
};

// A box around participants as placed.
struct BoxLayout {
    float x;
    float y;
    float width;
    float height;
    float margin;                   // the space between its side and its first or last participant
    float textHeight;
    bool placed;
    MermaidLabelIndex label;
};

// A message's model: its line's ends, and the bounds of what it covers.
struct MessageModel {
    float startx;
    float stopx;
    float starty;
    float height;
    float width;
    float fromBounds;
    float toBounds;
    float lineY;
    float number;
    bool numbered;
    MermaidLabelIndex label;
};

// A note's model.
struct NoteModel {
    float startx;
    float width;
    float starty;
    float height;
    MermaidLabelIndex label;
};

// The horizontal reach of a block, which wraps its title.
struct BlockWidth {
    float from;
    float to;
    float width;
};

// A block as placed: its frame, its sections, and its title.
struct Frame {
    MermaidBlock block;
    float startx;
    float stopx;
    float starty;
    float stopy;
    MermaidLabelIndex title;
    MermaidSpan color;              // a rect's color

    // The frame's first and last sections, in Layout::sections, or kMermaidNone.
    uint32_t firstSection;
    uint32_t lastSection;
};

// A section of a frame: else, and, or option.
struct Section {
    float y;
    MermaidLabelIndex title;
    uint32_t next;                  // the frame's next section, or kMermaidNone
};

// An activation as placed.
struct Activation {
    float startx;
    float stopx;
    float starty;
    float stopy;
    uint32_t actor;
    uint32_t drawn;                 // its place in Layout::drawn, which it takes when it starts
};

// What the main pass draws, in the order that it draws it.
enum {
    DrawnKind_Note,
    DrawnKind_Activation,
    DrawnKind_Frame,
};
typedef uint8_t DrawnKind;

struct Drawn {
    DrawnKind kind;
    uint32_t index;
};

// mermaid.js's bounds: the diagram's extent, the vertical position that each statement bumps, the open blocks, and the
// open activations.
struct Bounds {
    float startx = kPrF32Infinity;
    float stopx = -kPrF32Infinity;
    float starty = kPrF32Infinity;
    float stopy = -kPrF32Infinity;
    float verticalPos = 0;
};

struct Layout {
    MermaidRender* render;
    const MermaidSequence* diagram;
    PrVec2 offset;                          // from the layout's coordinates to the diagram's
    Bounds bounds;
    RawArray<ActorLayout> actors;
    RawArray<BoxLayout> boxes;
    RawArray<MessageModel> messages;        // by step
    RawArray<NoteModel> notes;              // by step
    RawArray<uint8_t> hasWidth;             // by step: a block start or section whose width is known
    RawArray<BlockWidth> widths;            // by step
    RawArray<Frame> frames;
    RawArray<Section> sections;
    RawArray<uint32_t> open;                // open frames, outermost first
    RawArray<Activation> activations;       // open activations
    RawArray<Activation> ended;             // activations as drawn
    RawArray<Drawn> drawn;
};

// Adds an item, whose rectangle is in the layout's coordinates.
void addItem(Layout& l, PrMermaidItemKind kind, PrRect rect, PrStringView id) {
    mermaidItemAdd(*l.render, kind, prRectOffset(rect, l.offset), id, PrStringView{});
}

PrStringView textOf(const Layout& l, MermaidSpan span) {
    return mermaidSequenceText(*l.diagram, span);
}

MermaidTextStyle styleOf(float size) {
    return mermaidTextStyle(size);
}

MermaidLabelIndex label(Layout& l, PrStringView text, float size) {
    return mermaidLabelCreate(*l.render, text, MermaidTextFormat_Plain, styleOf(size));
}

// Returns a label whose lines wrap at `width`.
MermaidLabelIndex wrappedLabel(Layout& l, PrStringView text, float size, float width) {
    MermaidTextStyle style = styleOf(size);
    style.wrapWidth = prF32Max(width, 1);
    return mermaidLabelCreate(*l.render, text, MermaidTextFormat_Plain, style);
}

// Returns a label that wraps at `width` if `wrap` is true.
MermaidLabelIndex labelWrappedIf(Layout& l, PrStringView text, float size, bool wrap, float width) {
    return wrap ? wrappedLabel(l, text, size, width) : label(l, text, size);
}

PrVec2 extentOf(const Layout& l, MermaidLabelIndex index) {
    return mermaidLabelExtent(*l.render, index);
}

// The height of one line of a label.
float lineHeightOf(const Layout& l, MermaidLabelIndex index) {
    return extentOf(l, index).y / float(mermaidLabelLineCount(*l.render, index));
}

// The center of a label whose first line has its middle at `firstMiddle`.
PrVec2 firstLineAt(const Layout& l, MermaidLabelIndex index, float x, float firstMiddle) {
    return PrVec2{x, firstMiddle - lineHeightOf(l, index) * 0.5f + extentOf(l, index).y * 0.5f};
}

float centerOf(const Layout& l, uint32_t actor) {
    return l.actors.data[actor].x + l.actors.data[actor].width * 0.5f;
}

void bump(Layout& l, float by) {
    l.bounds.verticalPos += by;
    l.bounds.stopy = prF32Max(l.bounds.stopy, l.bounds.verticalPos);
}

// Grows the open blocks and activations around a rectangle, each by the box margin for each level that it is nested in.
// As mermaid.js does, the activations count on from the blocks, so that they grow by nothing or shrink.
void updateBounds(Layout& l, float left, float top, float right, float bottom) {
    uint32_t count = l.open.count;
    uint32_t visited = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ++visited;
        float v = float(count) - float(visited) + 1;
        Frame& frame = l.frames.data[l.open.data[i]];
        frame.starty = prF32Min(frame.starty, top - v * kBoxMargin);
        frame.stopy = prF32Max(frame.stopy, bottom + v * kBoxMargin);
        frame.startx = prF32Min(frame.startx, left - v * kBoxMargin);
        frame.stopx = prF32Max(frame.stopx, right + v * kBoxMargin);
        l.bounds.startx = prF32Min(l.bounds.startx, left - v * kBoxMargin);
        l.bounds.stopx = prF32Max(l.bounds.stopx, right + v * kBoxMargin);
        l.bounds.starty = prF32Min(l.bounds.starty, top - v * kBoxMargin);
        l.bounds.stopy = prF32Max(l.bounds.stopy, bottom + v * kBoxMargin);
    }
    for (uint32_t i = 0; i < l.activations.count; ++i) {
        ++visited;
        float v = float(count) - float(visited) + 1;
        Activation& activation = l.activations.data[i];
        activation.starty = prF32Min(activation.starty, top - v * kBoxMargin);
        activation.stopy = prF32Max(activation.stopy, bottom + v * kBoxMargin);
        l.bounds.startx = prF32Min(l.bounds.startx, left - v * kBoxMargin);
        l.bounds.stopx = prF32Max(l.bounds.stopx, right + v * kBoxMargin);
    }
}

void insert(Layout& l, float x1, float y1, float x2, float y2) {
    float left = prF32Min(x1, x2);
    float right = prF32Max(x1, x2);
    float top = prF32Min(y1, y2);
    float bottom = prF32Max(y1, y2);
    l.bounds.startx = prF32Min(l.bounds.startx, left);
    l.bounds.starty = prF32Min(l.bounds.starty, top);
    l.bounds.stopx = prF32Max(l.bounds.stopx, right);
    l.bounds.stopy = prF32Max(l.bounds.stopy, bottom);
    updateBounds(l, left, top, right, bottom);
}

bool isMessageWithHead(MermaidArrow arrow) {
    return arrow != MermaidArrow_SolidOpen && arrow != MermaidArrow_DottedOpen;
}

bool isBidirectional(MermaidArrow arrow) {
    return arrow == MermaidArrow_BothSolid || arrow == MermaidArrow_BothDotted;
}

bool isDotted(MermaidArrow arrow) {
    return arrow == MermaidArrow_Dotted || arrow == MermaidArrow_DottedOpen || arrow == MermaidArrow_DottedCross ||
        arrow == MermaidArrow_DottedPoint || arrow == MermaidArrow_BothDotted;
}

// The span of an actor's activations on the cross axis, or of its lifeline if it has none (activationBounds).
void activationBounds(const Layout& l, const RawArray<Activation>& activations, uint32_t actor, float& low, float& high) {
    float center = centerOf(l, actor);
    low = center - 1;
    high = center + 1;
    for (const Activation& a : activations.span()) {
        if (a.actor != actor)
            continue;
        low = prF32Min(low, a.startx);
        high = prF32Max(high, a.stopx);
    }
}

uint32_t activationCount(const RawArray<Activation>& activations, uint32_t actor) {
    uint32_t count = 0;
    for (const Activation& a : activations.span())
        count += a.actor == actor ? 1 : 0;
    return count;
}

Activation startActivation(const Layout& l, const RawArray<Activation>& activations, uint32_t actor, float y) {
    float startx = centerOf(l, actor) + (float(activationCount(activations, actor)) - 1) * kActivationWidth * 0.5f;
    return Activation{
        .startx = startx,
        .stopx = startx + kActivationWidth,
        .starty = y,
        .stopy = -kPrF32Infinity,
        .actor = actor,
        .drawn = kMermaidNone,
    };
}

// Removes and returns the actor's last activation.
bool endActivation(RawArray<Activation>& activations, uint32_t actor, Activation* out) {
    for (uint32_t i = activations.count; i-- > 0;) {
        if (activations.data[i].actor == actor) {
            *out = activations.data[i];
            activations.erase(i);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Participants

// Gives each participant its width, and the margin after it that the widest message to or from its next neighbor needs
// (getMaxMessageWidthPerActor and calculateActorMargins). Gives each box its margin and its title's height.
void measureActors(Layout& l) {
    const MermaidSequence& d = *l.diagram;
    uint32_t count = d.actors.count;
    l.actors.resize(count);
    RawArray<float> needed{};
    RawArray<uint8_t> has{};
    needed.assignZeroed(count);
    has.assignZeroed(count);
    auto want = [&](uint32_t actor, float width) {
        needed.data[actor] = has.data[actor] != 0 ? prF32Max(needed.data[actor], width) : width;
        has.data[actor] = 1;
    };
    // A wrapped label wraps inside the default width, and the participant grows taller instead of wider.
    for (uint32_t a = 0; a < count; ++a) {
        ActorLayout& actor = l.actors.data[a];
        const MermaidActor& described = d.actors.data[a];
        PrStringView text = textOf(l, described.label);
        actor.label = labelWrappedIf(l, text, kFontSize, described.wrap, kActorWidth - 2 * kWrapPadding);
        PrVec2 extent = extentOf(l, actor.label);
        actor.width = described.wrap ? kActorWidth : prF32Max(kActorWidth, extent.x + 2 * kWrapPadding);
        actor.height = described.wrap ? prF32Max(extent.y, kActorHeight) : kActorHeight;
        actor.margin = kActorMargin;
        actor.stopy = -kPrF32Infinity;
    }
    for (const MermaidStep& step : d.steps.span()) {
        bool note = step.kind == MermaidStepKind_Note;
        if (step.kind != MermaidStepKind_Message && !note)
            continue;
        uint32_t to = step.to;
        uint32_t prev = to > 0 ? to - 1 : kMermaidNone;
        uint32_t next = to + 1 < count ? to + 1 : kMermaidNone;
        if (note && ((step.place == MermaidNotePlace_LeftOf && prev == kMermaidNone) ||
                (step.place == MermaidNotePlace_RightOf && next == kMermaidNone)))
            continue;
        MermaidLabelIndex text = labelWrappedIf(l, textOf(l, step.text), kFontSize, step.wrap,
            kActorWidth - 2 * kWrapPadding);
        float width = extentOf(l, text).x + 2 * kWrapPadding;
        if (!note) {
            if (step.from == next)
                want(to, width);
            else if (step.from == prev)
                want(step.from, width);
            else if (step.from == to)
                want(step.from, width * 0.5f);
        } else if (step.place == MermaidNotePlace_RightOf) {
            want(step.from, width);
        } else if (step.place == MermaidNotePlace_LeftOf) {
            want(prev, width);
        } else {
            if (prev != kMermaidNone)
                want(prev, width * 0.5f);
            if (next != kMermaidNone)
                want(step.from, width * 0.5f);
        }
    }
    for (uint32_t a = 0; a < count; ++a) {
        if (has.data[a] == 0)
            continue;
        ActorLayout& actor = l.actors.data[a];
        float next = a + 1 < count ? l.actors.data[a + 1].width * 0.5f : 0;
        actor.margin = prF32Max(needed.data[a] + kActorMargin - actor.width * 0.5f - next, kActorMargin);
    }

    l.boxes.resize(d.boxes.count);
    RawArray<float> widths{};                   // by box: the widths and margins of its participants
    widths.assignZeroed(d.boxes.count);
    for (uint32_t a = 0; a < count; ++a) {
        uint32_t box = d.actors.data[a].box;
        if (box != kMermaidNone)
            widths.data[box] += l.actors.data[a].width + l.actors.data[a].margin;
    }
    float textHeight = 0;
    for (uint32_t b = 0; b < d.boxes.count; ++b) {
        BoxLayout& box = l.boxes.data[b];
        box = BoxLayout{.margin = kBoxTextMargin};
        float inside = widths.data[b] + kBoxMargin * 8 - 2 * kBoxTextMargin;
        const MermaidActorBox& described = d.boxes.data[b];
        box.label = labelWrappedIf(l, textOf(l, described.label), kFontSize, described.wrap, inside - 2 * kWrapPadding);
        PrVec2 text = extentOf(l, box.label);
        textHeight = prF32Max(textHeight, text.y);
        float wanted = prF32Max(inside, text.x + 2 * kWrapPadding);
        if (inside < wanted)
            box.margin += (wanted - inside) * 0.5f;
    }
    for (BoxLayout& box : l.boxes.span())
        box.textHeight = textHeight;
    widths.reset();
    needed.reset();
    has.reset();
}

// Places the participants side by side, each box around its participants (addActorRenderingData).
void placeActors(Layout& l) {
    const MermaidSequence& d = *l.diagram;
    float right = 0;
    float margin = 0;
    float tallest = 0;
    uint32_t previousBox = kMermaidNone;
    for (uint32_t a = 0; a < d.actors.count; ++a) {
        ActorLayout& actor = l.actors.data[a];
        uint32_t box = d.actors.data[a].box;
        if (previousBox != kMermaidNone && previousBox != box)
            margin += kBoxMargin + l.boxes.data[previousBox].margin;
        if (box != kMermaidNone && box != previousBox) {
            l.boxes.data[box].x = right + margin;
            l.boxes.data[box].y = 0;
            l.boxes.data[box].placed = true;
            margin += l.boxes.data[box].margin;
        }
        tallest = prF32Max(tallest, actor.height);
        // A participant that a message creates gets room for its box beside the message's line.
        if (d.actors.data[a].created != kMermaidNone)
            margin += actor.width * 0.5f;
        actor.x = right + margin;
        actor.starty = l.bounds.verticalPos;
        insert(l, actor.x, 0, actor.x + actor.width, actor.height);
        right += actor.width + margin;
        if (box != kMermaidNone)
            l.boxes.data[box].width = right + l.boxes.data[box].margin - l.boxes.data[box].x;
        margin = actor.margin;
        previousBox = box;
    }
    bump(l, tallest);
}

// ---------------------------------------------------------------------------------------------------------------------
// Models

// A message's line, from the edge of its source's activations to the edge of its target's, and its width with its text
// (buildMessageModel).
MessageModel messageModel(Layout& l, const MermaidStep& step, const RawArray<Activation>& activations) {
    float fromLow = 0;
    float fromHigh = 0;
    float toLow = 0;
    float toHigh = 0;
    activationBounds(l, activations, step.from, fromLow, fromHigh);
    activationBounds(l, activations, step.to, toLow, toHigh);
    bool rightward = fromLow <= toLow;
    float start = rightward ? fromHigh : fromLow;
    float stop = rightward ? toLow : toHigh;
    bool targetActive = prF32Abs(toLow - toHigh) > 2;
    auto toward = [&](float by) {
        return rightward ? -by : by;
    };
    if (step.from == step.to) {
        stop = start;
    } else {
        if (step.activates && !targetActive)
            stop += toward(kActivationWidth * 0.5f - 1);
        if (isMessageWithHead(step.arrow))
            stop += toward(3);
        if (isBidirectional(step.arrow))
            start -= toward(3);
    }
    // A wrapped message wraps at the width between its ends, or at least at a participant's width, and its text does
    // not widen it.
    MessageModel m{};
    float span = prF32Abs(start - stop);
    float wrapped = prF32Max(span + 2 * kWrapPadding, kActorWidth);
    m.label = labelWrappedIf(l, textOf(l, step.text), kFontSize, step.wrap, wrapped);
    float text = step.wrap ? 0 : extentOf(l, m.label).x + 2 * kWrapPadding;
    m.width = prF32Max(text, wrapped);
    m.startx = start;
    m.stopx = stop;
    m.fromBounds = prF32Min(prF32Min(fromLow, fromHigh), prF32Min(toLow, toHigh));
    m.toBounds = prF32Max(prF32Max(fromLow, fromHigh), prF32Max(toLow, toHigh));
    return m;
}

// A note's box beside or over its participants (buildNoteModel). A wrapped note is measured with its text wrapped at a
// participant's width, and then wraps inside its own width.
NoteModel noteModel(Layout& l, const MermaidStep& step) {
    const ActorLayout& from = l.actors.data[step.from];
    const ActorLayout& to = l.actors.data[step.to];
    PrStringView source = textOf(l, step.text);
    bool wrap = step.wrap && source.size > 0;
    NoteModel n{};
    n.label = labelWrappedIf(l, source, kFontSize, wrap, kActorWidth);
    float text = extentOf(l, n.label).x;
    n.startx = from.x;
    if (step.place == MermaidNotePlace_RightOf) {
        if (wrap)
            n.width = prF32Max(kActorWidth, text);
        else
            n.width = prF32Max(from.width * 0.5f + to.width * 0.5f, text + 2 * kNoteMargin);
        n.startx = from.x + (from.width + kActorMargin) * 0.5f;
    } else if (step.place == MermaidNotePlace_LeftOf) {
        if (wrap)
            n.width = prF32Max(kActorWidth, text + 2 * kNoteMargin);
        else
            n.width = prF32Max(from.width * 0.5f + to.width * 0.5f, text + 2 * kNoteMargin);
        n.startx = from.x - n.width + (from.width - kActorMargin) * 0.5f;
    } else if (step.from == step.to) {
        float least = prF32Max(from.width, kActorWidth);
        if (wrap)
            n.width = least;
        else
            n.width = prF32Max(least, text + 2 * kNoteMargin);
        n.startx = from.x + (from.width - n.width) * 0.5f;
    } else {
        n.width = prF32Abs(from.x + from.width * 0.5f - (to.x + to.width * 0.5f)) + kActorMargin;
        const ActorLayout& left = from.x < to.x ? from : to;
        n.startx = left.x + left.width * 0.5f - kActorMargin * 0.5f;
    }
    if (wrap)
        n.label = wrappedLabel(l, source, kFontSize, n.width - 2 * kWrapPadding);
    return n;
}

// Builds every message's and note's model, and each block's horizontal reach, in a pass that tracks the activations as
// the main pass will (calculateLoopBounds).
void buildModels(Layout& l) {
    const MermaidSequence& d = *l.diagram;
    uint32_t count = d.steps.count;
    l.messages.resize(count);
    l.notes.resize(count);
    l.widths.resize(count);
    l.hasWidth.assignZeroed(count);
    RawArray<Activation> activations{};
    RawArray<uint32_t> stack{};             // the steps that started the open blocks
    RawArray<BlockWidth> reach{};           // by open block
    for (uint32_t s = 0; s < count; ++s) {
        const MermaidStep& step = d.steps.data[s];
        switch (step.kind) {
            case MermaidStepKind_BlockStart:
                stack.push(s);
                reach.push(BlockWidth{.from = kPrF32Infinity, .to = -kPrF32Infinity, .width = 0});
                break;
            case MermaidStepKind_BlockSection:
                if (step.text.size > 0 && stack.count > 0) {
                    l.widths.data[stack.last()] = reach.last();
                    l.hasWidth.data[stack.last()] = 1;
                    l.widths.data[s] = reach.last();
                    l.hasWidth.data[s] = 1;
                }
                break;
            case MermaidStepKind_BlockEnd:
                if (stack.count > 0) {
                    l.widths.data[stack.last()] = reach.last();
                    l.hasWidth.data[stack.last()] = 1;
                    stack.pop();
                    reach.pop();
                }
                break;
            case MermaidStepKind_Activate:
                activations.push(startActivation(l, activations, step.from, 0));
                break;
            case MermaidStepKind_Deactivate: {
                Activation ended{};
                endActivation(activations, step.from, &ended);
                break;
            }
            default:
                break;
        }
        if (step.kind == MermaidStepKind_Note) {
            NoteModel n = noteModel(l, step);
            l.notes.data[s] = n;
            for (BlockWidth& r : reach.span()) {
                r.from = prF32Min(r.from, n.startx);
                r.to = prF32Max(r.to, n.startx + n.width);
                r.width = prF32Max(r.width, prF32Abs(r.from - r.to)) - kLabelBoxWidth;
            }
        } else if (step.kind == MermaidStepKind_Message) {
            MessageModel m = messageModel(l, step, activations);
            l.messages.data[s] = m;
            for (BlockWidth& r : reach.span()) {
                if (m.startx == m.stopx) {
                    const ActorLayout& from = l.actors.data[step.from];
                    const ActorLayout& to = l.actors.data[step.to];
                    r.from = prF32Min(prF32Min(from.x - m.width * 0.5f, from.x - from.width * 0.5f), r.from);
                    r.to = prF32Max(prF32Max(to.x + m.width * 0.5f, to.x + from.width * 0.5f), r.to);
                    r.width = prF32Max(r.width, prF32Abs(r.to - r.from)) - kLabelBoxWidth;
                } else {
                    r.from = prF32Min(m.startx, r.from);
                    r.to = prF32Max(m.stopx, r.to);
                    r.width = prF32Max(r.width, m.width) - kLabelBoxWidth;
                }
            }
        }
    }
    activations.reset();
    stack.reset();
    reach.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// The main pass

// Opens a block or a section: bumps the position, then makes room for the title, which reads [title] and wraps to the
// block's width (adjustLoopHeightForWrap).
MermaidLabelIndex blockTitle(Layout& l, uint32_t s, float before, float after, float* height) {
    const MermaidStep& step = l.diagram->steps.data[s];
    bump(l, before);
    *height = after;
    MermaidLabelIndex title = kMermaidNone;
    if (step.text.size > 0 && l.hasWidth.data[s] != 0) {
        RawArray<char> text{};
        text.push('[');
        PrStringView source = textOf(l, step.text);
        text.append(source.data, uint32_t(source.size));
        text.push(']');
        MermaidTextStyle style = styleOf(kFontSize);
        style.wrapWidth = prF32Max(l.widths.data[s].width - 2 * kWrapPadding, 1);
        title = mermaidLabelCreate(*l.render, PrStringView{text.data, text.count}, MermaidTextFormat_Plain, style);
        *height = after + prF32Max(extentOf(l, title).y, kLabelBoxHeight);
        text.reset();
    }
    return title;
}

// Places a message's text and line (boundMessage), and returns the y of its line.
float boundMessage(Layout& l, MessageModel& m) {
    bump(l, 10);
    PrVec2 extent = extentOf(l, m.label);
    float lineHeight = lineHeightOf(l, m.label);
    m.height += lineHeight;
    bump(l, lineHeight);
    float below = extent.y - 10;
    float lineY = 0;
    if (m.startx == m.stopx) {
        below += kBoxMargin;
        lineY = l.bounds.verticalPos + below;
        below += 30;
        float half = prF32Max(extent.x * 0.5f, kActorWidth * 0.5f);
        insert(l, m.startx - half, l.bounds.verticalPos - 10 + below, m.stopx + half, l.bounds.verticalPos + 30 + below);
    } else {
        below += kBoxMargin;
        lineY = l.bounds.verticalPos + below;
        insert(l, m.startx, lineY - 10, m.stopx, lineY);
    }
    bump(l, below);
    m.height += below;
    insert(l, m.fromBounds, m.starty, m.toBounds, m.starty + m.height);
    return lineY;
}

// Returns the distance from a participant's center to where a message that creates or destroys it meets it. The line
// meets the box of a participant, or the glyph of a stick figure, control, entity or database. A boundary meets the
// line at its box, as in mermaid.js.
float createdReach(const Layout& l, uint32_t a) {
    switch (l.diagram->actors.data[a].kind) {
        case MermaidActorKind_Actor:
        case MermaidActorKind_Control:
        case MermaidActorKind_Entity:
        case MermaidActorKind_Database:
            return kStickWidth * 0.5f;
        default:
            return l.actors.data[a].width * 0.5f;
    }
}

// Ends a message's line at the participant that it creates or destroys, and grows the bounds to the participant's box.
// `receiver` is true if the participant is the message's target.
void meetActor(Layout& l, MessageModel& m, uint32_t a, uint32_t other, bool receiver, float reach) {
    const ActorLayout& actor = l.actors.data[a];
    bool left = actor.x < l.actors.data[other].x;
    float bottom = m.starty + m.height + actor.height * 0.5f + kNoteMargin;
    float& end = receiver ? m.stopx : m.startx;
    float& start = receiver ? m.startx : m.stopx;
    if (left) {
        insert(l, end - reach, m.starty, start, bottom);
        end += reach;
    } else {
        insert(l, start, m.starty, end + reach, bottom);
        end -= reach;
    }
}

// Places a participant that the message at step `s` creates or destroys, centered on the message's line
// (adjustCreatedDestroyedData). A created participant starts there, and a destroyed one ends there.
void placeCreatedDestroyed(Layout& l, uint32_t s, MessageModel& m) {
    const MermaidStep& step = l.diagram->steps.data[s];
    const MermaidActor* actors = l.diagram->actors.data;
    uint32_t actor = kMermaidNone;
    if (actors[step.to].created == s) {
        actor = step.to;
        meetActor(l, m, actor, step.from, true, createdReach(l, actor) + 3);
        l.actors.data[actor].starty = m.lineY - l.actors.data[actor].height * 0.5f;
    } else if (actors[step.from].destroyed == s) {
        actor = step.from;
        meetActor(l, m, actor, step.to, false, createdReach(l, actor));
    } else if (actors[step.to].destroyed == s) {
        actor = step.to;
        meetActor(l, m, actor, step.from, true, createdReach(l, actor) + 3);
    }
    if (actor == kMermaidNone)
        return;
    ActorLayout& placed = l.actors.data[actor];
    if (actors[actor].destroyed == s)
        placed.stopy = m.lineY - placed.height * 0.5f;
    bump(l, placed.height * 0.5f);
}

void placeNote(Layout& l, NoteModel& n) {
    bump(l, kBoxMargin);
    n.starty = l.bounds.verticalPos;
    float text = extentOf(l, n.label).y;
    n.height = text + 2 * kNoteMargin;
    bump(l, n.height);
    insert(l, n.startx, n.starty, n.startx + n.width, n.starty + n.height);
}

void endFrame(Layout& l) {
    uint32_t index = l.open.last();
    l.open.pop();
    Frame& frame = l.frames.data[index];
    // A block that holds nothing that grows it spans the participants, down to where it ends.
    if (frame.startx > frame.stopx) {
        frame.startx = 0;
        frame.stopx = 0;
        if (l.actors.count > 0) {
            const ActorLayout& last = l.actors.data[l.actors.count - 1];
            frame.startx = l.actors.data[0].x;
            frame.stopx = last.x + last.width;
        }
    }
    if (!(frame.stopy >= frame.starty))
        frame.stopy = l.bounds.verticalPos;
    if (frame.block != MermaidBlock_Rect)
        l.drawn.push(Drawn{DrawnKind_Frame, index});
    bump(l, frame.stopy - l.bounds.verticalPos);
}

void mainPass(Layout& l) {
    const MermaidSequence& d = *l.diagram;
    float number = 1;
    float step = 1;
    bool numbered = false;
    for (uint32_t s = 0; s < d.steps.count; ++s) {
        const MermaidStep& st = d.steps.data[s];
        switch (st.kind) {
            case MermaidStepKind_Note:
                placeNote(l, l.notes.data[s]);
                l.drawn.push(Drawn{DrawnKind_Note, s});
                break;
            case MermaidStepKind_Activate:
                // An activation draws below what comes after its start, as mermaid.js's group for it does.
                l.activations.push(startActivation(l, l.activations, st.from, l.bounds.verticalPos + 2));
                l.activations.last().drawn = l.drawn.count;
                l.drawn.push(Drawn{DrawnKind_Activation, kMermaidNone});
                break;
            case MermaidStepKind_Deactivate: {
                Activation a{};
                if (!endActivation(l.activations, st.from, &a))
                    break;
                float y = l.bounds.verticalPos;
                if (a.starty + 18 > y) {
                    a.starty = y - 6;
                    y += 12;
                }
                a.stopy = y;
                l.ended.push(a);
                l.drawn.data[a.drawn].index = l.ended.count - 1;
                insert(l, a.startx, y - 10, a.stopx, y);
                break;
            }
            case MermaidStepKind_BlockStart: {
                bool rect = st.block == MermaidBlock_Rect;
                float height = 0;
                MermaidLabelIndex title = kMermaidNone;
                if (rect) {
                    bump(l, kBoxMargin);
                    height = kBoxMargin;
                } else {
                    title = blockTitle(l, s, kBoxMargin, kBoxMargin + kBoxTextMargin, &height);
                }
                l.frames.push(Frame{
                    .block = st.block,
                    .startx = kPrF32Infinity,
                    .stopx = -kPrF32Infinity,
                    .starty = l.bounds.verticalPos,
                    .stopy = -kPrF32Infinity,
                    .title = title,
                    .color = st.text,
                    .firstSection = kMermaidNone,
                    .lastSection = kMermaidNone,
                });
                l.open.push(l.frames.count - 1);
                bump(l, height);
                break;
            }
            case MermaidStepKind_BlockSection: {
                if (l.open.count == 0)
                    break;
                float height = 0;
                MermaidLabelIndex title = blockTitle(l, s, kBoxMargin + kBoxTextMargin, kBoxMargin, &height);
                uint32_t index = l.sections.count;
                l.sections.push(Section{.y = l.bounds.verticalPos, .title = title, .next = kMermaidNone});
                Frame& frame = l.frames.data[l.open.last()];
                if (frame.lastSection != kMermaidNone)
                    l.sections.data[frame.lastSection].next = index;
                else
                    frame.firstSection = index;
                frame.lastSection = index;
                bump(l, height);
                break;
            }
            case MermaidStepKind_BlockEnd:
                if (l.open.count > 0)
                    endFrame(l);
                break;
            case MermaidStepKind_Autonumber:
                if (st.start > 0)
                    number = st.start;
                if (st.step > 0)
                    step = st.step;
                numbered = st.visible;
                break;
            case MermaidStepKind_Message: {
                MessageModel& m = l.messages.data[s];
                m.starty = l.bounds.verticalPos;
                m.number = number;
                m.numbered = numbered;
                m.lineY = boundMessage(l, m);
                placeCreatedDestroyed(l, s, m);
                number = prF32Round((number + step) * 100) / 100;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

void strokeLine(MermaidRender& render, PrVec2 a, PrVec2 b, MermaidInk ink, float width, const float* PR_NULLABLE dash) {
    mermaidDrawLine(render, a, b, ink, width, dash);
}

// Records a path filled and stroked.
void paintPath(MermaidRender& render, const PrPath* path, MermaidInk fill, MermaidInk stroke, float width) {
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .fill = fill,
        .stroke = stroke,
        .strokeWidth = width,
    };
    mermaidDrawPath(render, path, paint);
}

void paintRect(MermaidRender& render, PrRect rect, float radius, MermaidInk fill, MermaidInk stroke, float width) {
    PrPath* path = prPathCreate();
    if (radius > 0) {
        PrCornerRect rounded{.rect = rect};
        for (PrCorner& corner : rounded.corners)
            corner = PrCorner{.radii = PrVec2{radius, radius}, .shape = 1};
        prPathAddCornerRect(path, rounded);
    } else {
        prPathAddRect(path, rect);
    }
    paintPath(render, path, fill, stroke, width);
    prPathDestroy(path);
}

// Records a polygon in the frame of a marker: `points` along `direction` from `tip` and across it, scaled.
void markerPath(PrPath* path, PrVec2 tip, PrVec2 direction, const PrVec2* points, uint32_t count, float scale) {
    PrVec2 across{-direction.y, direction.x};
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 p = tip + direction * (points[i].x * scale) + across * (points[i].y * scale);
        if (i == 0)
            prPathMoveTo(path, p);
        else
            prPathLineTo(path, p);
    }
}

// Records the head of a message at the end of its line, which runs along `direction` to `end`: mermaid.js's markers, in
// their frames relative to their reference points.
void drawHead(MermaidRender& render, MermaidArrow arrow, PrVec2 end, PrVec2 direction) {
    MermaidInk ink = mermaidInk(MermaidRole_Line);
    PrPath* path = prPathCreate();
    switch (arrow) {
        case MermaidArrow_Solid:
        case MermaidArrow_Dotted:
        case MermaidArrow_BothSolid:
        case MermaidArrow_BothDotted: {
            // M -1 0 L 10 5 L 0 10 z, from (7.9, 5).
            const PrVec2 points[] = {{-8.9f, -5}, {2.1f, 0}, {-7.9f, 5}};
            markerPath(path, end, direction, points, 3, 1);
            prPathClose(path);
            paintPath(render, path, ink, ink, 1);
            break;
        }
        case MermaidArrow_SolidPoint:
        case MermaidArrow_DottedPoint: {
            // M 18,7 L9,13 L14,7 L9,1 Z, from (15.5, 7), in units of the line's width.
            const PrVec2 points[] = {{2.5f, 0}, {-6.5f, 6}, {-1.5f, 0}, {-6.5f, -6}};
            markerPath(path, end, direction, points, 4, kMessageWidth);
            prPathClose(path);
            mermaidFillPath(render, path, ink);
            break;
        }
        case MermaidArrow_SolidCross:
        case MermaidArrow_DottedCross: {
            // M 1,2 L 6,7 M 6,2 L 1,7, from (4, 4.5), in units of the line's width.
            const PrVec2 first[] = {{-3, -2.5f}, {2, 2.5f}};
            const PrVec2 second[] = {{2, -2.5f}, {-3, 2.5f}};
            markerPath(path, end, direction, first, 2, kMessageWidth);
            markerPath(path, end, direction, second, 2, kMessageWidth);
            mermaidStrokePath(render, path, ink, 1.333f * kMessageWidth, nullptr);
            break;
        }
        default:
            break;
    }
    prPathDestroy(path);
}

PrVec2 unit(PrVec2 v) {
    float length = prF32Sqrt(v.x * v.x + v.y * v.y);
    return length > 0 ? v * (1 / length) : PrVec2{1, 0};
}

void drawMessage(Layout& l, uint32_t s) {
    MermaidRender& render = *l.render;
    const MermaidStep& step = l.diagram->steps.data[s];
    const MessageModel& m = l.messages.data[s];
    MermaidInk ink = mermaidInk(MermaidRole_Line);
    const float* dash = isDotted(step.arrow) ? kDash : nullptr;
    float y = m.lineY;
    float start = m.startx;
    // The text's first line has its middle 31 below where its message starts, as mermaid.js's offsets put it.
    float middle = prF32Min(m.startx, m.stopx) + prF32Abs(m.stopx - m.startx) * 0.5f;
    mermaidLabelDrawCentered(render, m.label, firstLineAt(l, m.label, middle, m.starty + 31),
        mermaidInk(MermaidRole_Text));
    if (m.numbered && !isBidirectional(step.arrow))
        start += 6;
    PrVec2 endTangent{};
    PrVec2 end{};
    if (m.startx == m.stopx) {
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{start, y});
        prPathCubicTo(path, PrVec2{start + 60, y - 10}, PrVec2{m.stopx + 60, y + 30}, PrVec2{m.stopx, y + 20});
        mermaidStrokePath(render, path, ink, kMessageWidth, dash);
        prPathDestroy(path);
        end = PrVec2{m.stopx, y + 20};
        endTangent = unit(PrVec2{-60, -10});
    } else {
        strokeLine(render, PrVec2{start, y}, PrVec2{m.stopx, y}, ink, kMessageWidth, dash);
        end = PrVec2{m.stopx, y};
        endTangent = PrVec2{m.stopx >= start ? 1.0f : -1.0f, 0};
    }
    drawHead(render, step.arrow, end, endTangent);
    if (isBidirectional(step.arrow))
        drawHead(render, step.arrow, PrVec2{start, y}, PrVec2{-endTangent.x, 0});
    addItem(l, PrMermaidItemKind_Edge, PrRect{prF32Min(start, m.stopx), y - 1, prF32Max(start, m.stopx), y + 1},
        textOf(l, step.text));
    if (m.numbered) {
        bool rightward = m.startx <= m.stopx;
        float x = m.startx == m.stopx || rightward ? m.fromBounds + 1 : m.toBounds - 1;
        PrPath* circle = prPathCreate();
        prPathAddEllipse(circle, PrVec2{x, y}, PrVec2{kNumberRadius, kNumberRadius}, 0);
        mermaidFillPath(render, circle, ink);
        prPathDestroy(circle);
        char digits[kPrF64TextMaxBytes];
        size_t written = prF64Format(double(m.number), digits);
        float size = written > 5 ? 7 : written > 3 ? 9 : 12;
        MermaidLabelIndex number = label(l, PrStringView{digits, written}, size);
        mermaidLabelDrawCentered(render, number, PrVec2{x, y}, mermaidInk(MermaidRole_SequenceNumberText));
    }
}

// Returns the height of a participant at the bottom. mermaid.js measures each shape after it draws the top row, and the
// bottom row uses that height. For a glyph, the height includes room for the label below it.
float bottomHeight(const Layout& l, uint32_t a) {
    const ActorLayout& actor = l.actors.data[a];
    switch (l.diagram->actors.data[a].kind) {
        case MermaidActorKind_Boundary:
        case MermaidActorKind_Entity:
            return 2 * kGlyphRadius + kLabelBoxHeight;
        case MermaidActorKind_Control:
            return 2 * kGlyphRadius + 2 * kLabelBoxHeight;
        case MermaidActorKind_Database:
            return actor.width / 3 + kLabelBoxHeight;
        default:
            return actor.height;
    }
}

// Returns the distance from the top of a participant to the start of its lifeline. A glyph's lifeline starts below its
// label.
float lifelineStart(const Layout& l, uint32_t a) {
    const ActorLayout& actor = l.actors.data[a];
    switch (l.diagram->actors.data[a].kind) {
        case MermaidActorKind_Actor:
        case MermaidActorKind_Boundary:
            return 80;
        case MermaidActorKind_Control:
        case MermaidActorKind_Entity:
            return 75;
        case MermaidActorKind_Database:
            return actor.height + 2 * kBoxTextMargin;
        default:
            return actor.height;
    }
}

void addCircle(PrPath* path, PrVec2 center, float radius) {
    prPathAddEllipse(path, center, PrVec2{radius, radius}, 0);
}

// Records a stick figure.
void drawStickFigure(MermaidRender& render, float center, float top, MermaidInk fill, MermaidInk stroke) {
    PrPath* figure = prPathCreate();
    prPathMoveTo(figure, PrVec2{center, top + 25});
    prPathLineTo(figure, PrVec2{center, top + 45});
    prPathMoveTo(figure, PrVec2{center - kStickWidth * 0.5f, top + 33});
    prPathLineTo(figure, PrVec2{center + kStickWidth * 0.5f, top + 33});
    prPathMoveTo(figure, PrVec2{center - kStickWidth * 0.5f, top + 60});
    prPathLineTo(figure, PrVec2{center, top + 45});
    prPathLineTo(figure, PrVec2{center + kStickWidth * 0.5f - 2, top + 60});
    mermaidStrokePath(render, figure, stroke, 2, nullptr);
    prPathDestroy(figure);
    PrPath* head = prPathCreate();
    addCircle(head, PrVec2{center, top + 10}, 15);
    paintPath(render, head, fill, stroke, 2);
    prPathDestroy(head);
}

// Records a boundary: a circle with a sideways T on its left.
void drawBoundary(MermaidRender& render, PrVec2 circle, MermaidInk fill, MermaidInk stroke) {
    PrPath* tee = prPathCreate();
    float bar = circle.x - kGlyphRadius * 2.5f;
    prPathMoveTo(tee, PrVec2{bar, circle.y});
    prPathLineTo(tee, PrVec2{circle.x - 15, circle.y});
    prPathMoveTo(tee, PrVec2{bar, circle.y - 10});
    prPathLineTo(tee, PrVec2{bar, circle.y + 10});
    mermaidStrokePath(render, tee, stroke, 2, nullptr);
    prPathDestroy(tee);
    PrPath* path = prPathCreate();
    addCircle(path, circle, kGlyphRadius);
    paintPath(render, path, fill, stroke, 2);
    prPathDestroy(path);
}

// Records a control: a circle with an arrowhead on its top that points counterclockwise.
void drawControl(MermaidRender& render, PrVec2 circle, MermaidInk fill, MermaidInk stroke) {
    PrPath* path = prPathCreate();
    addCircle(path, circle, kGlyphRadius);
    paintPath(render, path, fill, stroke, 2);
    prPathClear(path);
    // The arrowhead is mermaid.js's marker M 14.4 5.6 L 7.2 10.4 L 8.8 5.6 L 7.2 0.8 Z, with its reference point at
    // (11, 5.8). It is scaled by the line's width of 2 and turned 172.5 degrees.
    const PrVec2 points[] = {{3.4f, -0.2f}, {-3.8f, 4.6f}, {-2.2f, -0.2f}, {-3.8f, -5}};
    float angle = prF32DegreesToRadians(172.5f);
    PrVec2 along{prF32Cos(angle), prF32Sin(angle)};
    markerPath(path, PrVec2{circle.x, circle.y - kGlyphRadius}, along, points, 4, 2);
    prPathClose(path);
    paintPath(render, path, fill, stroke, 2.4f);
    prPathDestroy(path);
}

// Records an entity: a circle on a line.
void drawEntity(MermaidRender& render, PrVec2 circle, MermaidInk fill, MermaidInk stroke) {
    PrPath* path = prPathCreate();
    addCircle(path, circle, kGlyphRadius);
    paintPath(render, path, fill, stroke, 1);
    prPathDestroy(path);
    float y = circle.y + kGlyphRadius;
    strokeLine(render, PrVec2{circle.x - kGlyphRadius, y}, PrVec2{circle.x + kGlyphRadius, y}, stroke, 2, nullptr);
}

// Records a database: an upright cylinder in the middle third of the participant's width, whose top is at `top`.
void drawDatabase(MermaidRender& render, const ActorLayout& actor, float top, MermaidInk fill, MermaidInk stroke) {
    float width = actor.width / 3;
    float rx = width * 0.5f;
    float ry = rx / (2.5f + width / 50);
    float left = actor.x + width;
    float right = left + width;
    float cx = left + rx;
    float upper = top + 2 * ry;
    float lower = top + width;
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{left, upper});
    prPathLineTo(path, PrVec2{left, lower});
    prPathEllipticArcTo(path, PrVec2{cx, lower}, PrVec2{rx, ry}, 0, kPrF32Pi, -kPrF32Pi);
    prPathLineTo(path, PrVec2{right, upper});
    prPathEllipticArcTo(path, PrVec2{cx, upper}, PrVec2{rx, ry}, 0, 0, -kPrF32Pi);
    prPathClose(path);
    paintPath(render, path, fill, stroke, 1);
    prPathClear(path);
    prPathEllipticArcTo(path, PrVec2{cx, upper}, PrVec2{rx, ry}, 0, kPrF32Pi, -kPrF32Pi);
    mermaidStrokePath(render, path, stroke, 1, nullptr);
    prPathDestroy(path);
}

// Records a queue: a cylinder on its side, which fills the participant's box, with its open end on the right.
void drawQueue(MermaidRender& render, PrRect box, MermaidInk fill, MermaidInk stroke) {
    float height = box.bottom - box.top;
    float ry = height * 0.5f;
    float rx = ry / (2.5f + height / 50);
    float middle = box.top + ry;
    PrPath* path = prPathCreate();
    prPathEllipticArcTo(path, PrVec2{box.left + rx, middle}, PrVec2{rx, ry}, 0, -kPrF32Pi * 0.5f, -kPrF32Pi);
    prPathEllipticArcTo(path, PrVec2{box.right - rx, middle}, PrVec2{rx, ry}, 0, kPrF32Pi * 0.5f, -kPrF32Pi);
    prPathClose(path);
    paintPath(render, path, fill, stroke, 1);
    prPathClear(path);
    prPathEllipticArcTo(path, PrVec2{box.right - rx, middle}, PrVec2{rx, ry}, 0, -kPrF32Pi * 0.5f, -kPrF32Pi);
    mermaidStrokePath(render, path, stroke, 1, nullptr);
    prPathDestroy(path);
}

// Records a participant at the top or the bottom, and its lifeline if at the top. mermaid.js places some glyphs and
// labels differently in the two rows, and the engine follows it.
void drawActor(Layout& l, uint32_t a, bool bottom) {
    MermaidRender& render = *l.render;
    const ActorLayout& actor = l.actors.data[a];
    MermaidActorKind kind = l.diagram->actors.data[a].kind;
    float top = bottom ? actor.stopy : actor.starty;
    float height = bottom ? bottomHeight(l, a) : actor.height;
    float center = actor.x + actor.width * 0.5f;
    MermaidInk fill = mermaidInk(MermaidRole_NodeFill);
    MermaidInk stroke = mermaidInk(MermaidRole_NodeStroke);
    if (!bottom) {
        strokeLine(render, PrVec2{center, top + lifelineStart(l, a)}, PrVec2{center, actor.stopy}, stroke,
            kLifelineWidth, nullptr);
    }
    PrRect box{actor.x, top, actor.x + actor.width, top + height};
    float labelY = top + height * 0.5f;
    switch (kind) {
        case MermaidActorKind_Actor:
            drawStickFigure(render, center, top, fill, stroke);
            labelY = top + 35 + height * 0.5f;
            break;
        case MermaidActorKind_Boundary:
            drawBoundary(render, PrVec2{center, top + 33}, fill, stroke);
            labelY = top + 36 + height * 0.5f;
            break;
        case MermaidActorKind_Control:
            drawControl(render, PrVec2{center, top + 32}, fill, stroke);
            labelY = top + kGlyphRadius + (bottom ? 5 : 12) + height * 0.5f;
            break;
        case MermaidActorKind_Entity:
            drawEntity(render, PrVec2{center, top + (bottom ? 32 : 31)}, fill, stroke);
            labelY = top + (bottom ? 37 : 36) + height * 0.5f;
            break;
        case MermaidActorKind_Database:
            drawDatabase(render, actor, top, fill, stroke);
            labelY = top + 35 + height * 0.5f;
            break;
        case MermaidActorKind_Collections: {
            paintRect(render, box, 0, fill, stroke, 1);
            box = prRectOffset(box, PrVec2{-kCollectionsOffset, kCollectionsOffset});
            paintRect(render, box, 0, fill, stroke, 1);
            center -= kCollectionsOffset;
            labelY += kCollectionsOffset;
            break;
        }
        case MermaidActorKind_Queue:
            drawQueue(render, box, fill, stroke);
            break;
        default:
            paintRect(render, box, 3, fill, stroke, 1);
            break;
    }
    mermaidLabelDrawCentered(render, actor.label, PrVec2{center, labelY}, mermaidInk(MermaidRole_StrongText));
    if (!bottom) {
        addItem(l, PrMermaidItemKind_Node, PrRect{actor.x, top, actor.x + actor.width, top + actor.height},
            textOf(l, l.diagram->actors.data[a].id));
    }
}

void drawNote(Layout& l, uint32_t s) {
    const NoteModel& n = l.notes.data[s];
    PrRect rect{n.startx, n.starty, n.startx + n.width, n.starty + n.height};
    paintRect(*l.render, rect, 0, mermaidInk(MermaidRole_NoteFill), mermaidInk(MermaidRole_NoteStroke), 1);
    PrVec2 center{(rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f};
    mermaidLabelDrawCentered(*l.render, n.label, center, mermaidInk(MermaidRole_StrongText));
    addItem(l, PrMermaidItemKind_Label, rect, PrStringView{});
}

void drawActivation(Layout& l, uint32_t index) {
    const Activation& a = l.ended.data[index];
    PrRect rect{a.startx, a.starty, a.stopx, a.stopy};
    paintRect(*l.render, rect, 0, mermaidInk(MermaidRole_ActivationFill), mermaidInk(MermaidRole_ActivationStroke), 1);
}

// The name of a block's kind, as its label box shows it.
PrStringView blockName(MermaidBlock block) {
    switch (block) {
        case MermaidBlock_Loop:
            return "loop";
        case MermaidBlock_Alt:
            return "alt";
        case MermaidBlock_Opt:
            return "opt";
        case MermaidBlock_Par:
            return "par";
        case MermaidBlock_Critical:
            return "critical";
        case MermaidBlock_Break:
            return "break";
        default:
            return "";
    }
}

// Records a block's frame, its label box with a clipped corner, its title, and its sections' dashed lines and titles.
void drawFrame(Layout& l, uint32_t index) {
    MermaidRender& render = *l.render;
    const Frame& f = l.frames.data[index];
    MermaidInk border = mermaidInk(MermaidRole_NodeStroke);
    MermaidInk text = mermaidInk(MermaidRole_StrongText);
    PrPath* frame = prPathCreate();
    prPathAddRect(frame, PrRect{f.startx, f.starty, f.stopx, f.stopy});
    mermaidStrokePath(render, frame, border, 2, kLoopDash);
    prPathDestroy(frame);
    for (uint32_t i = f.firstSection; i != kMermaidNone; i = l.sections.data[i].next) {
        const Section& section = l.sections.data[i];
        strokeLine(render, PrVec2{f.startx, section.y}, PrVec2{f.stopx, section.y}, border, 2, kDash);
        if (section.title != kMermaidNone) {
            float middle = section.y + kBoxMargin + kBoxTextMargin + 2.5f;
            mermaidLabelDrawCentered(render, section.title, firstLineAt(l, section.title, (f.startx + f.stopx) * 0.5f,
                middle), text);
        }
    }
    // The label box: 50 by 20, with its bottom right corner cut off.
    PrPath* box = prPathCreate();
    float x = f.startx;
    float y = f.starty;
    prPathMoveTo(box, PrVec2{x, y});
    prPathLineTo(box, PrVec2{x + kLabelBoxWidth, y});
    prPathLineTo(box, PrVec2{x + kLabelBoxWidth, y + kLabelBoxHeight - 7});
    prPathLineTo(box, PrVec2{x + kLabelBoxWidth - 7 * 1.2f, y + kLabelBoxHeight});
    prPathLineTo(box, PrVec2{x, y + kLabelBoxHeight});
    prPathClose(box);
    paintPath(render, box, mermaidInk(MermaidRole_NodeFill), border, 1);
    prPathDestroy(box);
    MermaidLabelIndex name = label(l, blockName(f.block), kFontSize);
    mermaidLabelDrawCentered(render, name, PrVec2{x + kLabelBoxWidth * 0.5f, y + kLabelBoxHeight * 0.5f + 2.5f}, text);
    if (f.title != kMermaidNone) {
        // mermaid.js centers the title between the label box's middle and the frame's right, a little below its top.
        float titleX = f.startx + kLabelBoxWidth * 0.5f + (f.stopx - f.startx) * 0.5f;
        float firstMiddle = f.starty + kBoxMargin + kBoxTextMargin + 2.5f;
        mermaidLabelDrawCentered(render, f.title, firstLineAt(l, f.title, titleX, firstMiddle), text);
    }
    addItem(l, PrMermaidItemKind_Group, PrRect{f.startx, f.starty, f.stopx, f.stopy}, blockName(f.block));
}

// A rect block's background, in its color.
void drawBackground(Layout& l, const Frame& f) {
    PrColor8 color{128, 128, 128, 128};
    prColor8FromCss(textOf(l, f.color), &color);
    PrPath* path = prPathCreate();
    prPathAddRect(path, PrRect{f.startx, f.starty, f.stopx, f.stopy});
    mermaidFillPath(*l.render, path, mermaidInkOfColor(color));
    prPathDestroy(path);
}

void drawBox(Layout& l, uint32_t b) {
    const BoxLayout& box = l.boxes.data[b];
    const MermaidActorBox& described = l.diagram->boxes.data[b];
    float spread = kBoxMargin * 2;
    PrRect rect{box.x - spread, box.y - spread * 0.25f, 0, 0};
    rect.right = rect.left + box.width + 2 * spread;
    rect.bottom = rect.top + box.height + spread * 0.75f;
    MermaidInk fill = described.hasColor ? mermaidInkOfColor(described.color) : mermaidInkOfColor(PrColor8{0, 0, 0, 0});
    paintRect(*l.render, rect, 0, fill, mermaidInkOfColor(PrColor8{0, 0, 0, 128}), 1);
    PrVec2 center{box.x + box.width * 0.5f, box.y + kBoxTextMargin + box.textHeight * 0.5f};
    mermaidLabelDrawCentered(*l.render, box.label, center, mermaidInk(MermaidRole_StrongText));
    addItem(l, PrMermaidItemKind_Group, rect, PrStringView{});
}

} // namespace

PrStatus mermaidSequenceDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidSequence diagram{};
    PrStatus status = mermaidSequenceParse(diagram, source, error);
    if (status != PrStatus_Ok) {
        mermaidSequenceReset(diagram);
        return status;
    }
    Layout l{.render = &render, .diagram = &diagram};
    measureActors(l);
    bool hasBox = diagram.boxes.count > 0;
    if (hasBox) {
        bump(l, kBoxMargin);
        bool titled = false;
        for (const MermaidActorBox& box : diagram.boxes.span())
            titled = titled || box.label.size > 0;
        if (titled)
            bump(l, l.boxes.data[0].textHeight);
    }
    placeActors(l);
    buildModels(l);
    mainPass(l);

    // The participants again at the bottom, with their lifelines down to them.
    bump(l, kBoxMargin * 2);
    float tallest = 0;
    for (uint32_t a = 0; a < l.actors.count; ++a) {
        ActorLayout& actor = l.actors.data[a];
        if (actor.stopy == -kPrF32Infinity)
            actor.stopy = l.bounds.verticalPos;
        float height = bottomHeight(l, a);
        insert(l, actor.x, actor.stopy, actor.x + actor.width, actor.stopy + height);
        tallest = prF32Max(tallest, height);
    }
    bump(l, tallest + kBoxMargin);
    for (uint32_t b = 0; b < l.boxes.count; ++b) {
        BoxLayout& box = l.boxes.data[b];
        box.height = l.bounds.verticalPos - box.y;
        if (box.placed)
            insert(l, box.x, box.y, box.x + box.width, box.height);
    }
    if (hasBox)
        bump(l, kBoxMargin);

    Bounds& b = l.bounds;
    if (b.startx == kPrF32Infinity) {
        b.startx = 0;
        b.stopx = 0;
        b.starty = 0;
        b.stopy = 0;
    }
    float height = b.stopy - b.starty + 2 * kMarginY - kBoxMargin + kBottomMarginAdj;
    float width = b.stopx - b.startx + 2 * kMarginX;
    bool titled = diagram.title.size > 0;
    float titleSpace = titled ? kTitleSpace : 0;
    render.extent = PrVec2{width, height + titleSpace};
    l.offset = PrVec2{kMarginX - b.startx, kMarginY + titleSpace};

    mermaidPushTransform(render, prAffine2Translate(l.offset));
    for (uint32_t i = 0; i < l.boxes.count; ++i) {
        if (l.boxes.data[i].placed)
            drawBox(l, i);
    }
    for (const Frame& f : l.frames.span()) {
        if (f.block == MermaidBlock_Rect)
            drawBackground(l, f);
    }
    for (uint32_t a = 0; a < l.actors.count; ++a) {
        drawActor(l, a, false);
        drawActor(l, a, true);
    }
    for (const Drawn& drawn : l.drawn.span()) {
        if (drawn.kind == DrawnKind_Note)
            drawNote(l, drawn.index);
        else if (drawn.kind != DrawnKind_Activation)
            drawFrame(l, drawn.index);
        else if (drawn.index != kMermaidNone) // as in mermaid.js, an activation that never ends is not drawn
            drawActivation(l, drawn.index);
    }
    for (uint32_t s = 0; s < diagram.steps.count; ++s) {
        if (diagram.steps.data[s].kind == MermaidStepKind_Message)
            drawMessage(l, s);
    }
    if (titled) {
        // mermaid.js puts its title's start a little left of the middle, above the diagram.
        MermaidLabelIndex title = label(l, textOf(l, diagram.title), kFontSize);
        PrVec2 extent = extentOf(l, title);
        PrVec2 origin{(b.stopx - b.startx) * 0.5f - 2 * kMarginX, -25 - extent.y * 0.75f};
        mermaidLabelDraw(render, title, origin, mermaidInk(MermaidRole_Title));
    }
    mermaidPopTransform(render);

    l.actors.reset();
    l.boxes.reset();
    l.messages.reset();
    l.notes.reset();
    l.hasWidth.reset();
    l.widths.reset();
    l.frames.reset();
    l.sections.reset();
    l.open.reset();
    l.activations.reset();
    l.ended.reset();
    l.drawn.reset();
    mermaidSequenceReset(diagram);
    return PrStatus_Ok;
}

} // namespace pr_impl
