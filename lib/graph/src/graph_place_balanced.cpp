// Balanced placement (graph_place_balanced.h): places the vertices and groups along their layers by Brandes and Köpf's
// method ("Fast and Simple Horizontal Coordinate Assignment", 2001), in the form that dagre gives it.
//
// Four alignments each join vertices into blocks that share a position: every vertex joins the median of its neighbors
// in the layer before it, in one of two vertical directions, preferring the median on one of two horizontal sides. Each
// alignment's blocks are then packed as tightly as their order allows. The result puts each vertex between the middle
// two of its four positions.
//
// A group is a pair of borders in each layer that it spans, around its run. Each border joins the same border in the
// next layer, ahead of every other alignment, so that each border is one block and the group's sides are straight. The
// layout's other inputs enter as follows: ports as offsets within blocks, so that an edge runs straight from its port;
// lanes as space beside their vertices; and a group's least extent as a least distance between its borders.

#include "graph/src/graph_place_balanced.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/sort.h"

#include "graph/src/graph_place.h"

namespace pr_impl {

namespace {

enum {
    ItemKind_Vertex,
    ItemKind_Open,                  // a group's border at the start of its run
    ItemKind_Close,                 // a group's border at the end of its run
};
typedef uint8_t ItemKind;

// A vertex, or a group's border, in one layer.
struct Item {
    uint32_t vertex;                // kGraphNone for a border
    uint32_t group;                 // a border's group, or the innermost group that holds the vertex
    uint32_t layer;
    uint32_t pos;                   // the position within the layer
    uint32_t component;
    float before;                   // the extent from the item's position toward lower positions, with its lane
    float after;                    // the extent toward higher positions
    float spacing;
    ItemKind kind;
    bool isVirtual;                 // a dummy, a label or a border, which joins the others in long straight chains
};

// A link between items in adjacent layers: a segment of an edge, or a border to the same border in the next layer.
struct Link {
    uint32_t upper;
    uint32_t lower;
    float upperOffset;              // where the link meets each item, from its position
    float lowerOffset;
    bool border;
};

// One of the four alignments.
struct Alignment {
    bool down;                      // joins each item to its neighbors in the layer above, sweeping downward
    bool left;                      // prefers the median toward lower positions, sweeping from them
};

constexpr Alignment kAlignments[4] = {
    {.down = true, .left = true},
    {.down = true, .left = false},
    {.down = false, .left = true},
    {.down = false, .left = false},
};

struct Balancer {
    GraphLayered* layered;
    RawArray<Item> items;
    RawArray<uint32_t> layerStart;  // by layer: where its items start, with the total last
    RawArray<uint32_t> itemOf;      // by vertex: its item, or kGraphNone for a placeholder
    RawArray<uint32_t> standIn;     // by vertex: for a placeholder, the item that its group's edges align with
    RawArray<uint32_t> openItem;    // by node: a group's opening border in its first layer
    RawArray<uint32_t> closeItem;   // by node: a group's closing border in its first layer
    RawArray<Link> links;
    RawArray<uint8_t> marked;       // by link: true if no alignment may use it
    RawArray<uint32_t> upStart;     // by item: where its links to the layer above start in `up`, with the total last
    RawArray<uint32_t> up;          // links, by their upper items' positions
    RawArray<uint32_t> downStart;
    RawArray<uint32_t> down;

    // The current alignment's blocks: each item's block's root, the next item in its block, and its position from the
    // root's.
    RawArray<uint32_t> root;
    RawArray<uint32_t> next;
    RawArray<float> shift;

    RawArray<float> xs[4];          // by alignment, then by item
};

float padding(const GraphLayered& layered, uint32_t group, GraphSide side) {
    return layered.padding.data[group * GraphSide_Count + side];
}

// Returns true if `group` holds the item: a vertex in it, or a border of a group nested in it.
bool holds(const GraphLayered& layered, uint32_t group, const Item& item) {
    uint32_t g = item.kind == ItemKind_Vertex ? item.group : layered.scope->nodes.data[item.group].parent;
    for (; g != kGraphNone; g = layered.scope->nodes.data[g].parent) {
        if (g == group)
            return true;
    }
    return false;
}

// The least distance between the positions of adjacent items `a` and `b`, with `a` at the lower position.
float separation(const Balancer& b, uint32_t first, uint32_t second) {
    const GraphLayered& layered = *b.layered;
    const Item& a = b.items.data[first];
    const Item& c = b.items.data[second];
    if (a.kind == ItemKind_Open && c.kind == ItemKind_Close && a.group == c.group)
        return padding(layered, a.group, GraphSide_CrossStart) + padding(layered, a.group, GraphSide_CrossEnd);
    if (a.kind == ItemKind_Open && holds(layered, a.group, c))
        return padding(layered, a.group, GraphSide_CrossStart) + c.before;
    if (c.kind == ItemKind_Close && holds(layered, c.group, a))
        return a.after + padding(layered, c.group, GraphSide_CrossEnd);
    float spacing = (a.spacing + c.spacing) * 0.5f;
    if (a.component != c.component)
        spacing = layered.options.componentSpacing;
    return a.after + spacing + c.before;
}

// The least distance between a group's borders: its least extent across the layers, and room for its padding.
float leastWidth(const GraphLayered& layered, uint32_t group) {
    const GraphScope& scope = *layered.scope;
    bool vertical = scope.direction == PrGraphDirection_Down || scope.direction == PrGraphDirection_Up;
    float least = vertical ? scope.nodes.data[group].extent.x : scope.nodes.data[group].extent.y;
    float pads = padding(layered, group, GraphSide_CrossStart) + padding(layered, group, GraphSide_CrossEnd);
    return least > pads ? least : pads;
}

void pushItem(Balancer& b, Item item) {
    item.pos = b.items.count - b.layerStart.last();
    b.items.push(item);
}

// Lists each layer's items: its vertices in their order, without the groups' placeholders, and each group's borders
// around its run. Links each border to the same border in the layer before.
void buildItems(Balancer& b) {
    GraphLayered& layered = *b.layered;
    const GraphScope& scope = *layered.scope;
    uint32_t n = scope.nodes.count;
    b.itemOf.resize(layered.vertices.count);
    b.openItem.resize(n);
    b.closeItem.resize(n);
    RawArray<uint32_t> lastOpen{};
    RawArray<uint32_t> lastClose{};
    lastOpen.resize(n);
    lastClose.resize(n);
    for (uint32_t g = 0; g < n; ++g) {
        lastOpen.data[g] = kGraphNone;
        lastClose.data[g] = kGraphNone;
        b.openItem.data[g] = kGraphNone;
        b.closeItem.data[g] = kGraphNone;
    }
    // A group's borders carry the lanes of its self-loops on their outer sides.
    auto border = [&](uint32_t group, ItemKind kind, uint32_t layer) {
        pushItem(b, Item{
            .vertex = kGraphNone,
            .group = group,
            .layer = layer,
            .component = layered.component.data[group],
            .before = kind == ItemKind_Open ? layered.groupReserveStart.data[group] : 0,
            .after = kind == ItemKind_Close ? layered.groupReserveEnd.data[group] : 0,
            .spacing = layered.options.nodeSpacing,
            .kind = kind,
            .isVirtual = true,
        });
        uint32_t item = b.items.count - 1;
        RawArray<uint32_t>& last = kind == ItemKind_Open ? lastOpen : lastClose;
        RawArray<uint32_t>& first = kind == ItemKind_Open ? b.openItem : b.closeItem;
        if (first.data[group] == kGraphNone)
            first.data[group] = item;
        if (last.data[group] != kGraphNone) {
            b.links.push(Link{
                .upper = last.data[group],
                .lower = item,
                .upperOffset = 0,
                .lowerOffset = 0,
                .border = true,
            });
        }
        last.data[group] = item;
    };
    RawArray<uint32_t> open{};
    RawArray<uint32_t> chain{};
    b.layerStart.clear();
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        b.layerStart.push(b.items.count);
        open.clear();
        for (uint32_t i = layered.layerStart.data[layer]; i < layered.layerStart.data[layer + 1]; ++i) {
            uint32_t v = layered.layerVertices.data[i];
            const GraphVertex& vertex = layered.vertices.data[v];
            // The groups that hold the vertex, outermost first. A placeholder lies in its own group.
            chain.clear();
            for (uint32_t g = vertex.group; g != kGraphNone; g = scope.nodes.data[g].parent)
                chain.push(g);
            reverseItems(chain.span());
            uint32_t common = 0;
            while (common < open.count && common < chain.count && open.data[common] == chain.data[common])
                ++common;
            while (open.count > common) {
                border(open.last(), ItemKind_Close, layer);
                open.pop();
            }
            for (uint32_t k = common; k < chain.count; ++k) {
                border(chain.data[k], ItemKind_Open, layer);
                open.push(chain.data[k]);
            }
            b.itemOf.data[v] = kGraphNone;
            if (vertex.kind == GraphVertexKind_Placeholder)
                continue;
            b.itemOf.data[v] = b.items.count;
            pushItem(b, Item{
                .vertex = v,
                .group = vertex.group,
                .layer = layer,
                .component = graphLayeredVertexComponent(layered, v),
                .before = vertex.cross * 0.5f + vertex.reserveStart,
                .after = vertex.cross * 0.5f + vertex.reserveEnd,
                .spacing = graphLayeredSpacing(layered, v),
                .kind = ItemKind_Vertex,
                .isVirtual = vertex.kind != GraphVertexKind_Leaf,
            });
        }
        while (open.count > 0) {
            border(open.last(), ItemKind_Close, layer);
            open.pop();
        }
    }
    b.layerStart.push(b.items.count);
    lastOpen.reset();
    lastClose.reset();
    open.reset();
    chain.reset();
}

// Picks each placeholder's stand-in: the middle vertex of its group's run in its layer, as dagre's callers point an
// edge at a group to a node inside it. An edge between two groups joins their stand-ins, so that the groups line up. An
// edge between a group and a vertex needs none: the group's placeholder moves to meet it.
void findStandIns(Balancer& b) {
    GraphLayered& layered = *b.layered;
    b.standIn.resize(layered.vertices.count);
    for (uint32_t v = 0; v < layered.vertices.count; ++v)
        b.standIn.data[v] = kGraphNone;
    RawArray<uint32_t> inside{};
    for (uint32_t i = 0; i < b.items.count; ++i) {
        const Item& open = b.items.data[i];
        if (open.kind != ItemKind_Open)
            continue;
        inside.clear();
        uint32_t j = i + 1;
        for (; !(b.items.data[j].kind == ItemKind_Close && b.items.data[j].group == open.group); ++j) {
            if (b.items.data[j].kind == ItemKind_Vertex)
                inside.push(j);
        }
        if (inside.count == 0)
            continue;
        uint32_t g = open.group;
        uint32_t placeholder = layered.nodeVertex.data[g] + (open.layer - layered.rankTop.data[g]);
        b.standIn.data[placeholder] = inside.data[(inside.count - 1) / 2];
    }
    inside.reset();
}

// Links the items of each segment whose ends both have items, or both have stand-ins, and indexes each item's links in
// both directions by the other ends' positions.
void buildLinks(Balancer& b) {
    GraphLayered& layered = *b.layered;
    for (uint32_t s = 0; s < layered.segments.count; ++s) {
        const GraphSegment& segment = layered.segments.data[s];
        if (segment.chain)
            continue;
        uint32_t upper = b.itemOf.data[segment.upper];
        uint32_t lower = b.itemOf.data[segment.lower];
        if (upper == kGraphNone && lower == kGraphNone) {
            upper = b.standIn.data[segment.upper];
            lower = b.standIn.data[segment.lower];
        }
        if (upper == kGraphNone || lower == kGraphNone)
            continue;
        Link link{.upper = upper, .lower = lower, .upperOffset = 0, .lowerOffset = 0, .border = false};
        graphLayeredSegmentOffsets(layered, s, link.upperOffset, link.lowerOffset);
        b.links.push(link);
    }
    uint32_t count = b.items.count;
    auto index = [&](RawArray<uint32_t>& start, RawArray<uint32_t>& list, bool byUpper) {
        start.assignZeroed(count + 1);
        for (const Link& link : b.links.span())
            ++start.data[(byUpper ? link.lower : link.upper) + 1];
        for (uint32_t i = 0; i < count; ++i)
            start.data[i + 1] += start.data[i];
        list.resize(b.links.count);
        RawArray<uint32_t> fill{};
        fill.append(start.data, count);
        for (uint32_t l = 0; l < b.links.count; ++l) {
            const Link& link = b.links.data[l];
            list.data[fill.data[byUpper ? link.lower : link.upper]++] = l;
        }
        fill.reset();
        for (uint32_t i = 0; i < count; ++i) {
            Span<uint32_t> run(list.data + start.data[i], start.data[i + 1] - start.data[i]);
            sortItems(run, [&](uint32_t x, uint32_t y) {
                uint32_t ox = byUpper ? b.links.data[x].upper : b.links.data[x].lower;
                uint32_t oy = byUpper ? b.links.data[y].upper : b.links.data[y].lower;
                return ox < oy || (ox == oy && x < y);
            });
        }
    };
    index(b.upStart, b.up, true);
    index(b.downStart, b.down, false);
    b.marked.assignZeroed(b.links.count);
}

// Marks the links that cross a protected link, so that no alignment uses them. A protected link is a border's, in the
// first pass, and in the second, any unmarked link between two virtual items: an inner segment of a long edge (Brandes
// and Köpf's type 1 conflicts; dagre's type 2 conflicts are the first pass's).
void markConflicts(Balancer& b, bool borders) {
    auto isProtected = [&](uint32_t l) {
        const Link& link = b.links.data[l];
        if (borders)
            return link.border;
        return b.marked.data[l] == 0 && b.items.data[link.upper].isVirtual && b.items.data[link.lower].isVirtual;
    };
    uint32_t layerCount = b.layerStart.count - 1;
    for (uint32_t layer = 1; layer < layerCount; ++layer) {
        uint32_t first = b.layerStart.data[layer];
        uint32_t last = b.layerStart.data[layer + 1];
        uint32_t upperCount = first - b.layerStart.data[layer - 1];
        if (upperCount == 0 || first == last)
            continue;
        uint32_t k0 = 0;
        uint32_t scan = first;
        for (uint32_t l1 = first; l1 < last; ++l1) {
            uint32_t fence = kGraphNone;
            for (uint32_t i = b.upStart.data[l1]; i < b.upStart.data[l1 + 1]; ++i) {
                if (isProtected(b.up.data[i]))
                    fence = b.items.data[b.links.data[b.up.data[i]].upper].pos;
            }
            if (l1 + 1 != last && fence == kGraphNone)
                continue;
            uint32_t k1 = fence != kGraphNone ? fence : upperCount - 1;
            for (; scan <= l1; ++scan) {
                for (uint32_t i = b.upStart.data[scan]; i < b.upStart.data[scan + 1]; ++i) {
                    uint32_t l = b.up.data[i];
                    uint32_t p = b.items.data[b.links.data[l].upper].pos;
                    if (!isProtected(l) && (p < k0 || p > k1))
                        b.marked.data[l] = 1;
                }
            }
            k0 = k1;
        }
    }
}

// The item's position along the alignment's sweep within its layer.
uint32_t sweepPos(const Balancer& b, const Alignment& alignment, uint32_t item) {
    const Item& it = b.items.data[item];
    if (alignment.left)
        return it.pos;
    return b.layerStart.data[it.layer + 1] - b.layerStart.data[it.layer] - 1 - it.pos;
}

// Joins each item to the median of its neighbors in the layer before it in the sweep, where no link already joined
// crosses the link to it and the link is not marked (Brandes and Köpf, algorithm 2).
void alignBlocks(Balancer& b, const Alignment& alignment) {
    uint32_t count = b.items.count;
    b.root.resize(count);
    b.next.resize(count);
    b.shift.assignZeroed(count);
    for (uint32_t i = 0; i < count; ++i) {
        b.root.data[i] = i;
        b.next.data[i] = i;
    }
    float sign = alignment.left ? 1.0f : -1.0f;
    uint32_t layerCount = b.layerStart.count - 1;
    for (uint32_t step = 1; step < layerCount; ++step) {
        uint32_t layer = alignment.down ? step : layerCount - 1 - step;
        uint32_t first = b.layerStart.data[layer];
        uint32_t size = b.layerStart.data[layer + 1] - first;
        int64_t reached = -1;
        for (uint32_t k = 0; k < size; ++k) {
            uint32_t v = first + (alignment.left ? k : size - 1 - k);
            const RawArray<uint32_t>& start = alignment.down ? b.upStart : b.downStart;
            const RawArray<uint32_t>& list = alignment.down ? b.up : b.down;
            uint32_t from = start.data[v];
            uint32_t d = start.data[v + 1] - from;
            if (d == 0)
                continue;
            for (uint32_t m = (d - 1) / 2; m <= d / 2; ++m) {
                if (b.next.data[v] != v)
                    break;
                uint32_t l = list.data[from + (alignment.left ? m : d - 1 - m)];
                const Link& link = b.links.data[l];
                uint32_t u = alignment.down ? link.upper : link.lower;
                int64_t at = sweepPos(b, alignment, u);
                if (b.marked.data[l] != 0 || reached >= at)
                    continue;
                float uOffset = alignment.down ? link.upperOffset : link.lowerOffset;
                float vOffset = alignment.down ? link.lowerOffset : link.upperOffset;
                b.next.data[u] = v;
                b.root.data[v] = b.root.data[u];
                b.next.data[v] = b.root.data[v];
                b.shift.data[v] = b.shift.data[u] + sign * (uOffset - vOffset);
                reached = at;
            }
        }
    }
}

// Packs the blocks (dagre's horizontal compaction): each takes the least position that its predecessors in the sweep
// allow, and then moves toward its successors as far as they allow, except a group's closing border along the sweep,
// which would widen the group. Writes the positions to `out`.
void packBlocks(Balancer& b, const Alignment& alignment, RawArray<float>& out) {
    const GraphLayered& layered = *b.layered;
    uint32_t count = b.items.count;
    struct Constraint {
        uint32_t from;
        uint32_t to;
        float gap;
    };
    RawArray<Constraint> constraints{};
    uint32_t layerCount = b.layerStart.count - 1;
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        uint32_t first = b.layerStart.data[layer];
        uint32_t size = b.layerStart.data[layer + 1] - first;
        for (uint32_t k = 0; k + 1 < size; ++k) {
            uint32_t a = first + (alignment.left ? k : size - 1 - k);
            uint32_t c = first + (alignment.left ? k + 1 : size - 2 - k);
            float gap = alignment.left ? separation(b, a, c) : separation(b, c, a);
            constraints.push(Constraint{b.root.data[a], b.root.data[c], gap + b.shift.data[a] - b.shift.data[c]});
        }
    }
    for (uint32_t g = 0; g < layered.scope->nodes.count; ++g) {
        if (b.openItem.data[g] == kGraphNone)
            continue;
        uint32_t lead = alignment.left ? b.openItem.data[g] : b.closeItem.data[g];
        uint32_t trail = alignment.left ? b.closeItem.data[g] : b.openItem.data[g];
        constraints.push(Constraint{b.root.data[lead], b.root.data[trail], leastWidth(layered, g)});
    }

    // The blocks in a topological order of the constraints.
    RawArray<uint32_t> inStart{};
    RawArray<uint32_t> outStart{};
    RawArray<uint32_t> ins{};
    RawArray<uint32_t> outs{};
    inStart.assignZeroed(count + 1);
    outStart.assignZeroed(count + 1);
    for (const Constraint& c : constraints.span()) {
        ++inStart.data[c.to + 1];
        ++outStart.data[c.from + 1];
    }
    for (uint32_t i = 0; i < count; ++i) {
        inStart.data[i + 1] += inStart.data[i];
        outStart.data[i + 1] += outStart.data[i];
    }
    ins.resize(constraints.count);
    outs.resize(constraints.count);
    RawArray<uint32_t> fillIn{};
    RawArray<uint32_t> fillOut{};
    fillIn.append(inStart.data, count);
    fillOut.append(outStart.data, count);
    for (uint32_t c = 0; c < constraints.count; ++c) {
        ins.data[fillIn.data[constraints.data[c].to]++] = c;
        outs.data[fillOut.data[constraints.data[c].from]++] = c;
    }
    RawArray<uint32_t> waiting{};
    RawArray<uint32_t> order{};
    waiting.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        waiting.data[i] = inStart.data[i + 1] - inStart.data[i];
        if (b.root.data[i] == i && waiting.data[i] == 0)
            order.push(i);
    }
    for (uint32_t k = 0; k < order.count; ++k) {
        uint32_t v = order.data[k];
        for (uint32_t i = outStart.data[v]; i < outStart.data[v + 1]; ++i) {
            uint32_t to = constraints.data[outs.data[i]].to;
            if (--waiting.data[to] == 0)
                order.push(to);
        }
    }

    uint32_t roots = 0;
    for (uint32_t i = 0; i < count; ++i)
        roots += b.root.data[i] == i ? 1 : 0;
    PR_ASSERT(order.count == roots);
    (void)roots;
    RawArray<float> blockAt{};
    blockAt.assignZeroed(count);
    for (uint32_t v : order.span()) {
        float at = 0;
        for (uint32_t i = inStart.data[v]; i < inStart.data[v + 1]; ++i) {
            const Constraint& c = constraints.data[ins.data[i]];
            float least = blockAt.data[c.from] + c.gap;
            at = least > at ? least : at;
        }
        blockAt.data[v] = at;
    }
    ItemKind trailing = alignment.left ? ItemKind_Close : ItemKind_Open;
    for (uint32_t k = order.count; k-- > 0;) {
        uint32_t v = order.data[k];
        if (outStart.data[v] == outStart.data[v + 1] || b.items.data[v].kind == trailing)
            continue;
        float most = kPrF32Infinity;
        for (uint32_t i = outStart.data[v]; i < outStart.data[v + 1]; ++i) {
            const Constraint& c = constraints.data[outs.data[i]];
            float bound = blockAt.data[c.to] - c.gap;
            most = bound < most ? bound : most;
        }
        blockAt.data[v] = most > blockAt.data[v] ? most : blockAt.data[v];
    }

    out.resize(count);
    float sign = alignment.left ? 1.0f : -1.0f;
    for (uint32_t i = 0; i < count; ++i)
        out.data[i] = sign * (blockAt.data[b.root.data[i]] + b.shift.data[i]);

    constraints.reset();
    inStart.reset();
    outStart.reset();
    ins.reset();
    outs.reset();
    fillIn.reset();
    fillOut.reset();
    waiting.reset();
    order.reset();
    blockAt.reset();
}

// Returns true if the positions keep every adjacent pair of items apart and every group at its least width.
bool isSeparated(const Balancer& b, const RawArray<float>& x) {
    constexpr float kSlack = 0.01f;
    for (uint32_t layer = 0; layer + 1 < b.layerStart.count; ++layer) {
        for (uint32_t i = b.layerStart.data[layer]; i + 1 < b.layerStart.data[layer + 1]; ++i) {
            if (x.data[i + 1] - x.data[i] < separation(b, i, i + 1) - kSlack)
                return false;
        }
    }
    for (uint32_t g = 0; g < b.openItem.count; ++g) {
        if (b.openItem.data[g] == kGraphNone)
            continue;
        if (x.data[b.closeItem.data[g]] - x.data[b.openItem.data[g]] < leastWidth(*b.layered, g) - kSlack)
            return false;
    }
    return true;
}

// Combines the four alignments' positions into `out` (dagre's balance). Each alignment moves to share the narrowest
// one's left edge, if it prefers lower positions, or its right edge otherwise. Then each item takes the mean of its
// middle two positions. If that breaks a separation, as it can, the narrowest alignment stands instead.
void combine(Balancer& b, RawArray<float>& out) {
    uint32_t count = b.items.count;
    uint32_t narrowest = 0;
    float least = kPrF32Infinity;
    float lows[4];
    float highs[4];
    for (uint32_t a = 0; a < 4; ++a) {
        float low = kPrF32Infinity;
        float high = -kPrF32Infinity;
        float edgeLow = kPrF32Infinity;
        float edgeHigh = -kPrF32Infinity;
        for (uint32_t i = 0; i < count; ++i) {
            float x = b.xs[a].data[i];
            low = x < low ? x : low;
            high = x > high ? x : high;
            float l = x - b.items.data[i].before;
            float h = x + b.items.data[i].after;
            edgeLow = l < edgeLow ? l : edgeLow;
            edgeHigh = h > edgeHigh ? h : edgeHigh;
        }
        lows[a] = low;
        highs[a] = high;
        if (edgeHigh - edgeLow < least) {
            least = edgeHigh - edgeLow;
            narrowest = a;
        }
    }
    for (uint32_t a = 0; a < 4; ++a) {
        if (a == narrowest)
            continue;
        float delta = kAlignments[a].left ? lows[narrowest] - lows[a] : highs[narrowest] - highs[a];
        for (uint32_t i = 0; i < count; ++i)
            b.xs[a].data[i] += delta;
    }
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        float v[4] = {b.xs[0].data[i], b.xs[1].data[i], b.xs[2].data[i], b.xs[3].data[i]};
        sortItems(Span<float>(v, 4), [](float x, float y) { return x < y; });
        out.data[i] = (v[1] + v[2]) * 0.5f;
    }
    if (!isSeparated(b, out)) {
        out.clear();
        out.append(b.xs[narrowest].data, count);
    }
}

void balancerReset(Balancer& b) {
    b.items.reset();
    b.layerStart.reset();
    b.itemOf.reset();
    b.standIn.reset();
    b.openItem.reset();
    b.closeItem.reset();
    b.links.reset();
    b.marked.reset();
    b.upStart.reset();
    b.up.reset();
    b.downStart.reset();
    b.down.reset();
    b.root.reset();
    b.next.reset();
    b.shift.reset();
    for (RawArray<float>& x : b.xs)
        x.reset();
}

} // namespace

void graphLayeredPlaceBalanced(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    uint32_t n = scope.nodes.count;
    Balancer b{.layered = &layered};
    buildItems(b);
    findStandIns(b);
    buildLinks(b);
    markConflicts(b, true);
    markConflicts(b, false);
    for (uint32_t a = 0; a < 4; ++a) {
        alignBlocks(b, kAlignments[a]);
        packBlocks(b, kAlignments[a], b.xs[a]);
    }
    RawArray<float> x{};
    combine(b, x);

    for (uint32_t v = 0; v < layered.vertices.count; ++v) {
        if (b.itemOf.data[v] != kGraphNone)
            layered.vertices.data[v].x = x.data[b.itemOf.data[v]];
    }
    layered.groupLeft.resize(n);
    layered.groupRight.resize(n);
    for (uint32_t g = 0; g < n; ++g) {
        layered.groupLeft.data[g] = kPrF32Infinity;
        layered.groupRight.data[g] = -kPrF32Infinity;
    }
    for (uint32_t i = 0; i < b.items.count; ++i) {
        const Item& item = b.items.data[i];
        if (item.kind == ItemKind_Open && x.data[i] < layered.groupLeft.data[item.group])
            layered.groupLeft.data[item.group] = x.data[i];
        if (item.kind == ItemKind_Close && x.data[i] > layered.groupRight.data[item.group])
            layered.groupRight.data[item.group] = x.data[i];
    }

    // A placeholder sits at the mean of its edges' other ends, so that they run straight where they can, or else at its
    // stand-in, or else in the middle of its group, within the group's padding.
    RawArray<float> sum{};
    RawArray<uint32_t> ends{};
    sum.assignZeroed(layered.vertices.count);
    ends.assignZeroed(layered.vertices.count);
    for (const GraphSegment& segment : layered.segments.span()) {
        if (segment.chain)
            continue;
        uint32_t pair[2] = {segment.upper, segment.lower};
        for (uint32_t k = 0; k < 2; ++k) {
            uint32_t own = pair[k];
            uint32_t other = b.itemOf.data[pair[1 - k]];
            if (layered.vertices.data[own].kind != GraphVertexKind_Placeholder || other == kGraphNone)
                continue;
            sum.data[own] += x.data[other];
            ++ends.data[own];
        }
    }
    for (uint32_t v = 0; v < layered.vertices.count; ++v) {
        GraphVertex& vertex = layered.vertices.data[v];
        if (vertex.kind != GraphVertexKind_Placeholder)
            continue;
        uint32_t g = vertex.node;
        float low = layered.groupLeft.data[g] + padding(layered, g, GraphSide_CrossStart);
        float high = layered.groupRight.data[g] - padding(layered, g, GraphSide_CrossEnd);
        float at = (low + high) * 0.5f;
        if (ends.data[v] > 0)
            at = sum.data[v] / float(ends.data[v]);
        else if (b.standIn.data[v] != kGraphNone)
            at = x.data[b.standIn.data[v]];
        at = at < low ? low : at;
        at = at > high ? high : at;
        vertex.x = low <= high ? at : (low + high) * 0.5f;
    }
    sum.reset();
    ends.reset();
    x.reset();
    balancerReset(b);
}

} // namespace pr_impl
