// Git graphs (mermaid_gitgraph.h). The parser runs the source's commands on a model of a repository: commits, branches,
// checkouts, merges and cherry-picks. The drawing gives each branch a lane, and each commit its own column in the order
// of the commands. Lines join each commit to its parents, turning with rounded corners between lanes.

#include "mermaid/src/mermaid_gitgraph.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/geom.h"
#include "pr/hash_index.h"
#include "pr/hash_key.h"
#include "pr/path.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's git graph measurements, in px.
constexpr float kCommitSpacing = 50;        // between commits along the lanes
constexpr float kFirstCommit = 10;          // from the lanes' start to the first commit
constexpr float kLaneSpacing = 90;
constexpr float kLaneEnd = 40;              // past the last commit
constexpr float kRadius = 10;               // a commit
constexpr float kMergeRadius = 6;           // the hole in a merge
constexpr float kCorner = 20;               // a line's corner between lanes
constexpr float kRerouteCorner = 10;        // a corner of a line that detours between lanes
constexpr float kLineWidth = 8;
constexpr float kBranchFontSize = 16;
constexpr float kLabelFontSize = 10;
constexpr float kBranchLabelGap = 35;       // between a branch's label and its lane's start
constexpr float kBranchLabelPadding = 9;
constexpr float kBranchLabelHeight = 23;
constexpr float kDiagramPadding = 8;
constexpr float kTitleFontSize = 18;
constexpr float kDash[2] = {2, 2};
constexpr float kDetourGap = 10;            // the least distance between two detours between lanes

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

// The attributes of a command, such as id: "x" and type: HIGHLIGHT.
struct Attributes {
    PrStringView id;
    PrStringView type;
    PrStringView order;
    PrStringView parent;
    PrStringView tags[8];
    uint32_t tagCount;
};

// Reads `key: value` pairs, whose values are quoted or run to white space.
bool readAttributes(PrStringView text, Attributes* out) {
    *out = Attributes{};
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at >= text.size)
            break;
        size_t keyStart = at;
        while (at < text.size && text.data[at] != ':' && !prAsciiIsSpace(text.data[at]))
            ++at;
        PrStringView key{text.data + keyStart, at - keyStart};
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at >= text.size || text.data[at] != ':')
            return false;
        ++at;
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        PrStringView value{};
        if (at < text.size && text.data[at] == '"') {
            size_t close = at + 1;
            while (close < text.size && text.data[close] != '"')
                ++close;
            if (close >= text.size)
                return false;
            value = PrStringView{text.data + at + 1, close - at - 1};
            at = close + 1;
        } else {
            size_t start = at;
            while (at < text.size && !prAsciiIsSpace(text.data[at]))
                ++at;
            value = PrStringView{text.data + start, at - start};
        }
        if (key == "id")
            out->id = value;
        else if (key == "type")
            out->type = value;
        else if (key == "order")
            out->order = value;
        else if (key == "parent")
            out->parent = value;
        else if (key == "tag" && out->tagCount < 8)
            out->tags[out->tagCount++] = value;
        else if (!(key == "msg"))
            return false;
    }
    return true;
}

struct Parser : MermaidParseState {
    MermaidGitGraph* graph;
    uint32_t current;                   // the branch that is checked out
    RawArray<char> scratch;

    // The branches by name and the commits by ID, as positions in the graph's arrays, compared by their text.
    HashIndex branchNames;
    HashIndex commitIds;
};

// Returns the branch named `name`, or kMermaidNone.
uint32_t findBranch(const Parser& parser, PrStringView name) {
    const MermaidGitGraph& graph = *parser.graph;
    uint32_t at = parser.branchNames.find(hashKey(name), [&](uint32_t b) {
        return mermaidGitGraphText(graph, graph.branches.data[b].name) == name;
    });
    return at == kHashMissing ? kMermaidNone : at;
}

// Returns the first commit whose ID is `id`, or kMermaidNone.
uint32_t findCommit(const Parser& parser, PrStringView id) {
    const MermaidGitGraph& graph = *parser.graph;
    uint32_t at = parser.commitIds.find(hashKey(id), [&](uint32_t c) {
        return mermaidGitGraphText(graph, graph.commits.data[c].id) == id;
    });
    return at == kHashMissing ? kMermaidNone : at;
}

// Adds a branch named `name`, which no branch has, with `head` as its latest commit.
void addBranch(Parser& parser, PrStringView name, uint32_t head, double order) {
    MermaidGitGraph& graph = *parser.graph;
    parser.branchNames.insert(hashKey(name), graph.branches.count);
    graph.branches.push(MermaidGitBranch{.name = mermaidTextAdd(graph.text, name), .head = head, .order = order});
}

bool readType(PrStringView name, MermaidCommitType* out) {
    if (name.size == 0)
        return true;
    if (name == "NORMAL")
        *out = MermaidCommitType_Normal;
    else if (name == "REVERSE")
        *out = MermaidCommitType_Reverse;
    else if (name == "HIGHLIGHT")
        *out = MermaidCommitType_Highlight;
    else
        return false;
    return true;
}

// Adds a commit on the current branch. A commit without an ID of its own gets mermaid.js's form of one: its number and
// seven hexadecimal digits, which here come from the number, so that the graph draws alike each time.
bool addCommit(Parser& parser, const Attributes& attributes, MermaidCommitType type, uint32_t other, bool merge) {
    MermaidGitGraph& graph = *parser.graph;
    MermaidGitBranch& branch = graph.branches.data[parser.current];
    MermaidCommit commit{
        .branch = parser.current,
        .parents = {branch.head, other},
        .type = type,
        .merge = merge,
        .customId = attributes.id.size > 0,
        .firstTag = graph.tags.count,
    };
    if (!readType(attributes.type, &commit.type))
        return mermaidParseFail(parser, "a commit's type is not NORMAL, REVERSE or HIGHLIGHT");
    if (commit.customId) {
        if (findCommit(parser, attributes.id) != kMermaidNone)
            return mermaidParseFail(parser, "a commit's ID is already used");
        commit.id = mermaidTextAdd(graph.text, attributes.id);
    } else {
        uint32_t seq = graph.commits.count;
        uint32_t hash = 2166136261u;
        for (uint32_t k = 0; k < 4; ++k)
            hash = (hash ^ ((seq >> (8 * k)) & 0xff)) * 16777619u;
        parser.scratch.clear();
        char digits[kPrF64TextMaxBytes];
        size_t written = prF64Format(double(seq), digits);
        parser.scratch.append(digits, uint32_t(written));
        parser.scratch.push('-');
        for (int32_t k = 6; k >= 0; --k)
            parser.scratch.push("0123456789abcdef"[(hash >> (4 * k)) & 0xf]);
        commit.id = mermaidTextAdd(graph.text, PrStringView{parser.scratch.data, parser.scratch.count});
    }
    for (uint32_t t = 0; t < attributes.tagCount; ++t)
        graph.tags.push(mermaidTextAdd(graph.text, attributes.tags[t]));
    commit.tagCount = graph.tags.count - commit.firstTag;
    // An ID of mermaid.js's form may repeat one that the source gave an earlier commit, which keeps it.
    PrStringView id = mermaidGitGraphText(graph, commit.id);
    if (findCommit(parser, id) == kMermaidNone)
        parser.commitIds.insert(hashKey(id), graph.commits.count);
    graph.commits.push(commit);
    branch.head = graph.commits.count - 1;
    return true;
}

// Reads a branch's name, quoted or a word, and the attributes after it.
PrStringView branchName(PrStringView rest, PrStringView* attributes) {
    rest = prStringTrim(rest);
    size_t end = 0;
    if (rest.size > 0 && rest.data[0] == '"') {
        end = 1;
        while (end < rest.size && rest.data[end] != '"')
            ++end;
        *attributes = mermaidAfter(rest, end + 1);
        return PrStringView{rest.data + 1, end > 1 ? end - 1 : 0};
    }
    while (end < rest.size && !prAsciiIsSpace(rest.data[end]))
        ++end;
    *attributes = mermaidAfter(rest, end);
    return PrStringView{rest.data, end};
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidGitGraph& graph = *parser.graph;
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    Attributes attributes{};
    if (word == "commit") {
        if (!readAttributes(rest, &attributes))
            return mermaidParseFail(parser, "a commit's attributes do not read");
        return addCommit(parser, attributes, MermaidCommitType_Normal, kMermaidNone, false);
    }
    if (word == "branch") {
        PrStringView tail{};
        PrStringView name = branchName(rest, &tail);
        if (name.size == 0 || !readAttributes(tail, &attributes))
            return mermaidParseFail(parser, "a branch has no name");
        if (findBranch(parser, name) != kMermaidNone)
            return mermaidParseFail(parser, "a branch of that name already exists");
        double order = double(graph.branches.count);
        if (attributes.order.size > 0 && mermaidNumberParse(attributes.order, &order) != attributes.order.size)
            return mermaidParseFail(parser, "a branch's order is not a number");
        addBranch(parser, name, graph.branches.data[parser.current].head, order);
        parser.current = graph.branches.count - 1;
        return true;
    }
    if (word == "checkout" || word == "switch") {
        PrStringView tail{};
        uint32_t branch = findBranch(parser, branchName(rest, &tail));
        if (branch == kMermaidNone)
            return mermaidParseFail(parser, "checkout names no branch that the graph has");
        parser.current = branch;
        return true;
    }
    if (word == "merge") {
        PrStringView tail{};
        uint32_t branch = findBranch(parser, branchName(rest, &tail));
        if (branch == kMermaidNone)
            return mermaidParseFail(parser, "merge names no branch that the graph has");
        if (branch == parser.current)
            return mermaidParseFail(parser, "a branch cannot merge itself");
        uint32_t head = graph.branches.data[branch].head;
        uint32_t current = graph.branches.data[parser.current].head;
        if (head == kMermaidNone || current == kMermaidNone)
            return mermaidParseFail(parser, "a merge needs commits on both branches");
        if (head == current)
            return mermaidParseFail(parser, "a merge's branches have the same latest commit");
        if (!readAttributes(tail, &attributes))
            return mermaidParseFail(parser, "a merge's attributes do not read");
        MermaidCommitType type = MermaidCommitType_Merge;
        if (!readType(attributes.type, &type))
            return mermaidParseFail(parser, "a commit's type is not NORMAL, REVERSE or HIGHLIGHT");
        attributes.type = PrStringView{};
        return addCommit(parser, attributes, type, head, true);
    }
    if (word == "cherry-pick") {
        if (!readAttributes(rest, &attributes))
            return mermaidParseFail(parser, "a cherry-pick's attributes do not read");
        uint32_t source = findCommit(parser, attributes.id);
        if (source == kMermaidNone)
            return mermaidParseFail(parser, "a cherry-pick names no commit that the graph has");
        if (graph.commits.data[source].branch == parser.current)
            return mermaidParseFail(parser, "a cherry-pick's commit is already on the current branch");
        Attributes picked{.tagCount = attributes.tagCount};
        for (uint32_t t = 0; t < attributes.tagCount; ++t)
            picked.tags[t] = attributes.tags[t];
        RawArray<char> tag{};
        if (picked.tagCount == 0) {
            tag.append("cherry-pick:", 12);
            tag.append(attributes.id.data, uint32_t(attributes.id.size));
            if (graph.commits.data[source].merge && attributes.parent.size > 0) {
                tag.append("|parent:", 8);
                tag.append(attributes.parent.data, uint32_t(attributes.parent.size));
            }
            picked.tags[picked.tagCount++] = PrStringView{tag.data, tag.count};
        }
        bool ok = addCommit(parser, picked, MermaidCommitType_CherryPick, source, false);
        tag.reset();
        return ok;
    }
    return mermaidParseFail(parser, "unknown statement");
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

// The colors of a branch, by its lane's index: its commits and lines, a highlighted commit's square, and the text of
// its label, which is drawn on its color.
struct BranchColors {
    PrColor8 light;
    PrColor8 dark;
    PrColor8 highlightLight;
    PrColor8 highlightDark;
    bool whiteText;
};

constexpr BranchColors kBranchColors[8] = {
    {{0x00, 0x00, 0xec, 0xff}, {0x3d, 0x3d, 0xff, 0xff}, {0x13, 0x13, 0x00, 0xff}, {0xff, 0xff, 0x80, 0xff}, true},
    {{0xde, 0xde, 0x00, 0xff}, {0xff, 0xff, 0x3d, 0xff}, {0x00, 0x00, 0xa0, 0xff}, {0x80, 0x80, 0xff, 0xff}, false},
    {{0x9d, 0xec, 0x00, 0xff}, {0xbe, 0xff, 0x3d, 0xff}, {0x31, 0x00, 0x92, 0xff}, {0xaa, 0x80, 0xff, 0xff}, false},
    {{0x00, 0x76, 0xec, 0xff}, {0x3d, 0x9e, 0xff, 0xff}, {0x92, 0x49, 0x00, 0xff}, {0xff, 0xbf, 0x80, 0xff}, true},
    {{0x00, 0xec, 0xec, 0xff}, {0x3d, 0xff, 0xff, 0xff}, {0x92, 0x00, 0x00, 0xff}, {0xff, 0x80, 0x80, 0xff}, false},
    {{0x00, 0xec, 0x76, 0xff}, {0x3d, 0xff, 0x9e, 0xff}, {0x92, 0x00, 0x49, 0xff}, {0xff, 0x80, 0xbf, 0xff}, false},
    {{0xec, 0x00, 0xec, 0xff}, {0xff, 0x3d, 0xff, 0xff}, {0x00, 0x92, 0x00, 0xff}, {0x80, 0xff, 0x80, 0xff}, false},
    {{0xec, 0x00, 0x00, 0xff}, {0xff, 0x3d, 0x3d, 0xff}, {0x00, 0x92, 0x92, 0xff}, {0x80, 0xff, 0xff, 0xff}, false},
};

MermaidInk branchInk(uint32_t lane) {
    const BranchColors& colors = kBranchColors[lane % 8];
    return mermaidInkOfColors(colors.light, colors.dark);
}

MermaidInk paired(PrColor8 light, PrColor8 dark) {
    return mermaidInkOfColors(light, dark);
}

// The extent of what the graph draws, which places it in the diagram.
struct Bounds {
    float left = kPrF32Infinity;
    float top = kPrF32Infinity;
    float right = -kPrF32Infinity;
    float bottom = -kPrF32Infinity;

    void add(PrVec2 p) {
        left = prF32Min(left, p.x);
        right = prF32Max(right, p.x);
        top = prF32Min(top, p.y);
        bottom = prF32Max(bottom, p.y);
    }
    void add(PrRect r) {
        add(PrVec2{r.left, r.top});
        add(PrVec2{r.right, r.bottom});
    }
};

struct Layout {
    MermaidRender* render;
    const MermaidGitGraph* graph;
    RawArray<uint32_t> lanes;           // by branch: its lane, in the order of the branches' `order`
    RawArray<float> laneUsed;           // the y of each detour between lanes, which later detours avoid

    // The detours by the band of kDetourGap that holds each, as positions in `laneUsed`. Detours are at least
    // kDetourGap apart, so a band holds at most one.
    HashIndex laneBands;

    // The commits before and after each commit on its branch, or kMermaidNone.
    RawArray<uint32_t> previousOnBranch;  // by commit
    RawArray<uint32_t> nextOnBranch;      // by commit

    bool measuring;                     // the pass that only finds the bounds
    Bounds bounds;
    PrVec2 offset;                      // from the graph's coordinates to the diagram's, which items are in
};

PrRect offsetRect(const Layout& l, PrRect r) {
    return PrRect{r.left + l.offset.x, r.top + l.offset.y, r.right + l.offset.x, r.bottom + l.offset.y};
}

PrVec2 commitAt(const Layout& l, uint32_t c) {
    const MermaidCommit& commit = l.graph->commits.data[c];
    return PrVec2{kFirstCommit + kCommitSpacing * float(c), kLaneSpacing * float(l.lanes.data[commit.branch])};
}

// Returns a y between two lanes for a detour, away from the detours already taken, as mermaid.js's findLane does.
float findLane(Layout& l, float from, float to, uint32_t depth) {
    float middle = from + prF32Abs(from - to) * 0.5f;
    if (depth > 5)
        return middle;
    // A detour within kDetourGap of `middle` is in its band or in a band beside it.
    int32_t band = int32_t(prF32Floor(middle / kDetourGap));
    bool clear = true;
    for (int32_t near = band - 1; near <= band + 1 && clear; ++near) {
        uint32_t at = l.laneBands.find(hashKey(near), [&](uint32_t used) {
            return int32_t(prF32Floor(l.laneUsed.data[used] / kDetourGap)) == near;
        });
        clear = at == kHashMissing || prF32Abs(l.laneUsed.data[at] - middle) >= kDetourGap;
    }
    if (clear) {
        l.laneBands.insert(hashKey(band), l.laneUsed.count);
        l.laneUsed.push(middle);
        return middle;
    }
    return findLane(l, from, to - prF32Abs(from - to) / 5, depth + 1);
}

// Appends a corner of radius `r` at `corner`, coming from the path's current point and leaving toward `next`.
void addCorner(PrPath* path, PrVec2 from, PrVec2 corner, PrVec2 next, float r) {
    auto toward = [](PrVec2 a, PrVec2 b, float distance) {
        PrVec2 d = b - a;
        float length = prF32Sqrt(d.x * d.x + d.y * d.y);
        return length > 0 ? a + d * (distance / length) : a;
    };
    PrVec2 in = toward(corner, from, r);
    PrVec2 out = toward(corner, next, r);
    constexpr float kKappa = 0.5522847f;
    prPathLineTo(path, in);
    prPathCubicTo(path, in + (corner - in) * kKappa, out + (corner - out) * kKappa, out);
}

// Records the line from a parent to its child, as mermaid.js routes it. A line that turns from one lane to another
// turns at the parent's column, or at the child's for a merge's second parent. If a commit on the lane that the line
// would run along lies between them, the line detours along a free y between the lanes.
void drawArrow(Layout& l, uint32_t parent, uint32_t child) {
    const MermaidGitGraph& g = *l.graph;
    const MermaidCommit& from = g.commits.data[parent];
    const MermaidCommit& to = g.commits.data[child];
    PrVec2 p = commitAt(l, parent);
    PrVec2 c = commitAt(l, child);
    bool secondParent = to.merge && to.parents[0] != parent;
    uint32_t color = l.lanes.data[secondParent ? from.branch : to.branch];
    // A line down to its child runs along the child's lane, and one up along the parent's. It detours if a commit on
    // that lane lies between the two.
    bool reroute = false;
    if (p.y < c.y) {
        uint32_t before = l.previousOnBranch.data[child];
        reroute = before != kMermaidNone && before > parent;
    } else {
        reroute = l.nextOnBranch.data[parent] < child;
    }
    PrPath* path = prPathCreate();
    prPathMoveTo(path, p);
    if (p.y == c.y) {
        prPathLineTo(path, c);
    } else if (reroute) {
        float y = p.y < c.y ? findLane(l, p.y, c.y, 0) : findLane(l, c.y, p.y, 0);
        if (p.y > c.y)
            color = l.lanes.data[from.branch];
        addCorner(path, p, PrVec2{p.x, y}, PrVec2{c.x, y}, kRerouteCorner);
        addCorner(path, PrVec2{p.x, y}, PrVec2{c.x, y}, c, kRerouteCorner);
        prPathLineTo(path, c);
    } else {
        PrVec2 corner = secondParent ? PrVec2{c.x, p.y} : PrVec2{p.x, c.y};
        addCorner(path, p, corner, c, kCorner);
        prPathLineTo(path, c);
    }
    if (l.measuring) {
        l.bounds.add(p);
        l.bounds.add(c);
    } else {
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .stroke = branchInk(color),
            .strokeWidth = kLineWidth,
        };
        mermaidDrawPath(*l.render, path, paint);
    }
    prPathDestroy(path);
}

void drawCommit(Layout& l, uint32_t c) {
    MermaidRender& render = *l.render;
    const MermaidCommit& commit = l.graph->commits.data[c];
    PrVec2 at = commitAt(l, c);
    uint32_t lane = l.lanes.data[commit.branch];
    MermaidInk ink = branchInk(lane);
    MermaidInk light = mermaidInk(MermaidRole_NodeFill);
    PrPath* path = prPathCreate();
    switch (commit.type) {
        case MermaidCommitType_Highlight: {
            const BranchColors& colors = kBranchColors[lane % 8];
            prPathAddRect(path, PrRect{at.x - 10, at.y - 10, at.x + 10, at.y + 10});
            mermaidFillPath(render, path, paired(colors.highlightLight, colors.highlightDark));
            prPathClear(path);
            prPathAddRect(path, PrRect{at.x - 6, at.y - 6, at.x + 6, at.y + 6});
            mermaidFillPath(render, path, light);
            break;
        }
        case MermaidCommitType_CherryPick: {
            MermaidInk white = paired(PrColor8{0xff, 0xff, 0xff, 0xff}, PrColor8{0x1e, 0x1f, 0x26, 0xff});
            prPathAddEllipse(path, at, PrVec2{kRadius, kRadius}, 0);
            mermaidFillPath(render, path, mermaidInk(MermaidRole_Line));
            prPathClear(path);
            prPathAddEllipse(path, at + PrVec2{-3, 2}, PrVec2{2.75f, 2.75f}, 0);
            prPathAddEllipse(path, at + PrVec2{3, 2}, PrVec2{2.75f, 2.75f}, 0);
            mermaidFillPath(render, path, white);
            prPathClear(path);
            prPathMoveTo(path, at + PrVec2{3, 1});
            prPathLineTo(path, at + PrVec2{0, -5});
            prPathLineTo(path, at + PrVec2{-3, 1});
            mermaidStrokePath(render, path, white, 1, nullptr);
            break;
        }
        default:
            prPathAddEllipse(path, at, PrVec2{kRadius, kRadius}, 0);
            mermaidFillPath(render, path, ink);
            if (commit.type == MermaidCommitType_Merge) {
                prPathClear(path);
                prPathAddEllipse(path, at, PrVec2{kMergeRadius, kMergeRadius}, 0);
                mermaidFillPath(render, path, light);
            } else if (commit.type == MermaidCommitType_Reverse) {
                prPathClear(path);
                prPathMoveTo(path, at + PrVec2{-5, -5});
                prPathLineTo(path, at + PrVec2{5, 5});
                prPathMoveTo(path, at + PrVec2{-5, 5});
                prPathLineTo(path, at + PrVec2{5, -5});
                mermaidStrokePath(render, path, light, 3, nullptr);
            }
            break;
    }
    prPathDestroy(path);
    PrRect box{at.x - kRadius, at.y - kRadius, at.x + kRadius, at.y + kRadius};
    mermaidItemAdd(render, PrMermaidItemKind_Node, offsetRect(l, box), mermaidGitGraphText(*l.graph, commit.id),
        PrStringView{});
}

// Records a commit's ID below it, turned 45 degrees, as mermaid.js places it. Merges show IDs that the source gives,
// and cherry-picks show none.
void drawCommitLabel(Layout& l, uint32_t c) {
    const MermaidCommit& commit = l.graph->commits.data[c];
    if (commit.type == MermaidCommitType_CherryPick || (commit.merge && !commit.customId))
        return;
    MermaidRender& render = *l.render;
    PrVec2 at = commitAt(l, c);
    MermaidLabelIndex label = mermaidLabelCreate(render, mermaidGitGraphText(*l.graph, commit.id),
        MermaidTextFormat_Plain, mermaidTextStyle(kLabelFontSize));
    PrVec2 extent = mermaidLabelExtent(render, label);
    float k = (extent.x + 10) / 25;
    PrVec2 pivot{at.x - 10, at.y};
    PrAffine2 transform = prAffine2Concat(prAffine2Translate(PrVec2{-pivot.x, -pivot.y}),
        prAffine2Rotate(prF32DegreesToRadians(-45)));
    transform = prAffine2Concat(transform, prAffine2Translate(pivot + PrVec2{-7.5f - k * 9.5f, 6.6f + k * 8.5f}));
    PrRect background{at.x - extent.x * 0.5f - 2, at.y + 13.5f, at.x + extent.x * 0.5f + 2, at.y + 28.5f};
    if (l.measuring) {
        l.bounds.add(prAffine2MapPoint(transform, PrVec2{background.left, background.top}));
        l.bounds.add(prAffine2MapPoint(transform, PrVec2{background.right, background.top}));
        l.bounds.add(prAffine2MapPoint(transform, PrVec2{background.left, background.bottom}));
        l.bounds.add(prAffine2MapPoint(transform, PrVec2{background.right, background.bottom}));
        return;
    }
    mermaidPushTransform(render, transform);
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Opacity,
        .fill = paired(PrColor8{0xff, 0xff, 0xde, 0xff}, PrColor8{0x33, 0x32, 0x1e, 0xff}),
        .opacity = 0.5f,
    };
    PrPath* path = prPathCreate();
    prPathAddRect(path, background);
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
    mermaidLabelDrawCentered(render, label, PrVec2{at.x, (background.top + background.bottom) * 0.5f},
        paired(PrColor8{0x00, 0x00, 0x21, 0xff}, PrColor8{0xe2, 0xe2, 0xea, 0xff}));
    mermaidPopTransform(render);
}

// Records a commit's tags above it, the first nearest, each a label with a pointed end and a hole.
void drawTags(Layout& l, uint32_t c) {
    const MermaidCommit& commit = l.graph->commits.data[c];
    MermaidRender& render = *l.render;
    PrVec2 at = commitAt(l, c);
    for (uint32_t t = 0; t < commit.tagCount; ++t) {
        MermaidLabelIndex label = mermaidLabelCreate(render,
            mermaidGitGraphText(*l.graph, l.graph->tags.data[commit.firstTag + t]), MermaidTextFormat_Plain,
            mermaidTextStyle(kLabelFontSize));
        PrVec2 extent = mermaidLabelExtent(render, label);
        float shift = float(t) * 20;
        float top = at.y - 26.7f - shift;
        float bottom = at.y - 11.7f - shift;
        float middle = at.y - 19.2f - shift;
        float textLeft = at.x - extent.x * 0.5f;
        PrVec2 points[] = {
            {textLeft - 12, middle + 2}, {textLeft - 12, middle - 2}, {textLeft - 4, top},
            {textLeft + extent.x + 4, top}, {textLeft + extent.x + 4, bottom}, {textLeft - 4, bottom},
        };
        if (l.measuring) {
            for (PrVec2 p : points)
                l.bounds.add(p);
            continue;
        }
        PrPath* path = prPathCreate();
        prPathMoveTo(path, points[0]);
        for (uint32_t i = 1; i < 6; ++i)
            prPathLineTo(path, points[i]);
        prPathClose(path);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .fill = mermaidInk(MermaidRole_NodeFill),
            .stroke = paired(PrColor8{0xc6, 0xc6, 0xf2, 0xff}, PrColor8{0x5a, 0x56, 0x8a, 0xff}),
            .strokeWidth = 1,
        };
        mermaidDrawPath(render, path, paint);
        prPathClear(path);
        prPathAddEllipse(path, PrVec2{textLeft - 8, middle}, PrVec2{1.5f, 1.5f}, 0);
        mermaidFillPath(render, path, mermaidInk(MermaidRole_Line));
        prPathDestroy(path);
        mermaidLabelDraw(render, label, PrVec2{textLeft, middle - extent.y * 0.5f}, mermaidInk(MermaidRole_NodeText));
    }
}

// Records the lanes, dashed, with each branch's label before its lane.
void drawBranches(Layout& l, float end) {
    MermaidRender& render = *l.render;
    const MermaidGitGraph& g = *l.graph;
    for (uint32_t b = 0; b < g.branches.count; ++b) {
        uint32_t lane = l.lanes.data[b];
        float y = kLaneSpacing * float(lane);
        MermaidLabelIndex label = mermaidLabelCreate(render, mermaidGitGraphText(g, g.branches.data[b].name),
            MermaidTextFormat_Plain, mermaidTextStyle(kBranchFontSize));
        PrVec2 extent = mermaidLabelExtent(render, label);
        float width = extent.x + 2 * kBranchLabelPadding;
        PrRect box{
            -kBranchLabelGap - width, y - kBranchLabelHeight * 0.5f, -kBranchLabelGap,
            y + kBranchLabelHeight * 0.5f,
        };
        if (l.measuring) {
            l.bounds.add(box);
            l.bounds.add(PrVec2{end, y});
            continue;
        }
        mermaidDrawLine(render, PrVec2{0, y}, PrVec2{end, y}, mermaidInk(MermaidRole_Line), 1, kDash);
        PrPath* path = prPathCreate();
        PrCornerRect rounded{.rect = box};
        for (PrCorner& corner : rounded.corners)
            corner = PrCorner{.radii = PrVec2{4, 4}, .shape = 1};
        prPathAddCornerRect(path, rounded);
        mermaidFillPath(render, path, branchInk(lane));
        prPathDestroy(path);
        bool white = kBranchColors[lane % 8].whiteText;
        PrColor8 text = white ? PrColor8{0xff, 0xff, 0xff, 0xff} : PrColor8{0x00, 0x00, 0x00, 0xff};
        mermaidLabelDraw(render, label, PrVec2{box.left + kBranchLabelPadding, y - extent.y * 0.5f},
            mermaidInkOfColor(text));
        mermaidItemAdd(render, PrMermaidItemKind_Group, offsetRect(l, box),
            mermaidGitGraphText(g, g.branches.data[b].name), PrStringView{});
    }
}

void drawAll(Layout& l) {
    const MermaidGitGraph& g = *l.graph;
    float end = kFirstCommit + kCommitSpacing * float(g.commits.count > 0 ? g.commits.count - 1 : 0) + kLaneEnd;
    l.laneUsed.clear();
    l.laneBands.clear();
    drawBranches(l, end);
    for (uint32_t c = 0; c < g.commits.count; ++c) {
        for (uint32_t parent : g.commits.data[c].parents) {
            if (parent != kMermaidNone)
                drawArrow(l, parent, c);
        }
    }
    for (uint32_t c = 0; c < g.commits.count; ++c) {
        if (l.measuring) {
            PrVec2 at = commitAt(l, c);
            l.bounds.add(PrRect{at.x - kRadius, at.y - kRadius, at.x + kRadius, at.y + kRadius});
        } else {
            drawCommit(l, c);
        }
    }
    for (uint32_t c = 0; c < g.commits.count; ++c) {
        drawCommitLabel(l, c);
        drawTags(l, c);
    }
}

} // namespace

PrStatus mermaidGitGraphParse(MermaidGitGraph& graph, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    graph.title = MermaidSpan{};
    graph.text.clear();
    graph.commits.clear();
    graph.branches.clear();
    graph.tags.clear();
    Parser parser{};
    parser.error = error;
    parser.graph = &graph;
    addBranch(parser, "main", kMermaidNone, 0);
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_GitGraph, nullptr);
    graph.title = mermaidTextAdd(graph.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        ok = parseStatement(parser, line.content);
    }
    parser.scratch.reset();
    parser.branchNames.reset();
    parser.commitIds.reset();
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidGitGraphText(const MermaidGitGraph& graph, MermaidSpan span) {
    return mermaidTextAt(graph.text, span);
}

void mermaidGitGraphReset(MermaidGitGraph& graph) {
    graph.text.reset();
    graph.commits.reset();
    graph.branches.reset();
    graph.tags.reset();
}

PrStatus mermaidGitGraphDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidGitGraph graph{};
    PrStatus status = mermaidGitGraphParse(graph, source, error);
    if (status != PrStatus_Ok) {
        mermaidGitGraphReset(graph);
        return status;
    }
    Layout l{.render = &render, .graph = &graph};
    // Lanes go in the order of the branches' `order`, and then of their creation.
    RawArray<uint32_t> order{};
    for (uint32_t b = 0; b < graph.branches.count; ++b)
        order.push(b);
    sortItems(order.span(), [&](uint32_t a, uint32_t b) {
        double oa = graph.branches.data[a].order;
        double ob = graph.branches.data[b].order;
        return oa < ob || (oa == ob && a < b);
    });
    l.lanes.resize(graph.branches.count);
    for (uint32_t i = 0; i < order.count; ++i)
        l.lanes.data[order.data[i]] = i;
    order.reset();

    // Each commit's neighbors on its branch.
    RawArray<uint32_t> last{};          // by branch: its latest commit so far
    last.resize(graph.branches.count);
    for (uint32_t b = 0; b < graph.branches.count; ++b)
        last.data[b] = kMermaidNone;
    l.nextOnBranch.resize(graph.commits.count);
    for (uint32_t c = 0; c < graph.commits.count; ++c) {
        uint32_t& latest = last.data[graph.commits.data[c].branch];
        l.previousOnBranch.push(latest);
        if (latest != kMermaidNone)
            l.nextOnBranch.data[latest] = c;
        l.nextOnBranch.data[c] = kMermaidNone;
        latest = c;
    }
    last.reset();

    // The first pass finds the bounds, without drawing, and the second draws, moved into the diagram.
    l.measuring = true;
    drawAll(l);
    MermaidLabelIndex title =
        mermaidLabelCreate(render, mermaidGitGraphText(graph, graph.title), MermaidTextFormat_Plain,
            mermaidTextStyle(kTitleFontSize));
    PrVec2 titleExtent = mermaidLabelExtent(render, title);
    float titleSpace = titleExtent.y > 0 ? titleExtent.y + 10 : 0;
    Bounds b = l.bounds;
    float width = prF32Max(b.right - b.left, titleExtent.x);
    render.extent = PrVec2{width + 2 * kDiagramPadding, b.bottom - b.top + titleSpace + 2 * kDiagramPadding};
    if (titleExtent.y > 0) {
        mermaidLabelDraw(render, title, PrVec2{(render.extent.x - titleExtent.x) * 0.5f, kDiagramPadding},
            mermaidInk(MermaidRole_Title));
    }
    PrVec2 offset{kDiagramPadding - b.left + (width - (b.right - b.left)) * 0.5f, kDiagramPadding + titleSpace - b.top};
    l.measuring = false;
    l.offset = offset;
    mermaidPushTransform(render, prAffine2Translate(offset));
    drawAll(l);
    mermaidPopTransform(render);

    l.lanes.reset();
    l.laneUsed.reset();
    l.laneBands.reset();
    l.previousOnBranch.reset();
    l.nextOnBranch.reset();
    mermaidGitGraphReset(graph);
    return PrStatus_Ok;
}

} // namespace pr_impl
