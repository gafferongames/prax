// Mesh bounding volume hierarchies: a tree of boxes over a mesh's triangles, for ray queries.

#include "pr/mesh_bvh.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/raw_array.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

constexpr uint32_t kBinCount = 16;
constexpr uint32_t kLeafMaxTriangles = 4;

// The cost of visiting an interior node, in units of one ray and triangle test.
constexpr float kTraversalCost = 1.0f;

// The depth from which the build splits a node's triangles into halves by count. Halving ends within 30 levels for any
// number of triangles, which keeps every path within kPrMeshBvhMaxDepth however the heuristic split above.
constexpr uint32_t kHalvingDepth = 32;

// A box that grows to hold what is added to it. An empty box has `min` above `max`.
struct Box {
    PrVec3 min;
    PrVec3 max;
};

// The triangles of a node that wait to be split: a range of the triangle order.
struct BuildTask {
    uint32_t node;
    uint32_t first;
    uint32_t count;
    uint32_t depth;                 // the number of nodes from the root to this node, counting both
};

// The triangles whose centers fall in one bin.
struct Bin {
    Box box;
    uint32_t count;
};

// Where the surface area heuristic splits a node.
struct Split {
    uint32_t axis;
    uint32_t bin;                   // triangles in bins below this one go to the first child
    uint32_t firstCount;            // the number of triangles that go to the first child
    float cost;                     // the sum over both children of surface area times triangles
    bool isFound;
};

Box emptyBox() {
    return Box{{kPrF32Infinity, kPrF32Infinity, kPrF32Infinity}, {-kPrF32Infinity, -kPrF32Infinity, -kPrF32Infinity}};
}

float lesser(float a, float b) {
    return b < a ? b : a;
}

float greater(float a, float b) {
    return b > a ? b : a;
}

void addPoint(Box* box, PrVec3 point) {
    box->min = PrVec3{lesser(box->min.x, point.x), lesser(box->min.y, point.y), lesser(box->min.z, point.z)};
    box->max = PrVec3{greater(box->max.x, point.x), greater(box->max.y, point.y), greater(box->max.z, point.z)};
}

void addBox(Box* box, const Box& other) {
    addPoint(box, other.min);
    addPoint(box, other.max);
}

PrVec3 centerOf(const Box& box) {
    return box.min * 0.5f + box.max * 0.5f;
}

// Returns half the surface area of `box`, which orders boxes as the area does.
float areaOf(const Box& box) {
    PrVec3 extent = box.max - box.min;
    return extent.x * extent.y + extent.y * extent.z + extent.z * extent.x;
}

float axisOf(PrVec3 v, uint32_t axis) {
    if (axis == 0)
        return v.x;
    return axis == 1 ? v.y : v.z;
}

// How centers map to bins along one axis.
struct Binning {
    float origin;
    float scale;
    bool isUsable;                  // false if the centers do not spread along the axis
};

Binning binningOf(const Box& centers, uint32_t axis) {
    float low = axisOf(centers.min, axis);
    float extent = axisOf(centers.max, axis) - low;
    float scale = float(kBinCount) / extent;
    return Binning{low, scale, extent > 0 && prF32IsFinite(extent) && prF32IsFinite(scale)};
}

uint32_t binOf(const Binning& binning, float center) {
    float bin = (center - binning.origin) * binning.scale;
    if (bin >= float(kBinCount - 1))
        return kBinCount - 1;
    return bin > 0 ? uint32_t(bin) : 0;
}

// Returns the cheapest split of the triangles `order` among the boundaries between bins on each axis.
Split findSplit(const Box* boxes, Span<const uint32_t> order, const Box& centers) {
    Split best{};
    for (uint32_t axis = 0; axis < 3; ++axis) {
        Binning binning = binningOf(centers, axis);
        if (!binning.isUsable)
            continue;
        Bin bins[kBinCount];
        for (Bin& bin : bins)
            bin = Bin{emptyBox(), 0};
        for (uint32_t triangle : order) {
            Bin& bin = bins[binOf(binning, axisOf(centerOf(boxes[triangle]), axis))];
            addBox(&bin.box, boxes[triangle]);
            ++bin.count;
        }

        // The cost of the triangles above each boundary, from the last boundary down.
        float aboveCosts[kBinCount];
        Box above = emptyBox();
        uint32_t aboveCount = 0;
        for (uint32_t boundary = kBinCount - 1; boundary > 0; --boundary) {
            if (bins[boundary].count != 0)
                addBox(&above, bins[boundary].box);
            aboveCount += bins[boundary].count;
            aboveCosts[boundary] = aboveCount != 0 ? areaOf(above) * float(aboveCount) : 0;
        }
        Box below = emptyBox();
        uint32_t belowCount = 0;
        for (uint32_t boundary = 1; boundary < kBinCount; ++boundary) {
            if (bins[boundary - 1].count != 0)
                addBox(&below, bins[boundary - 1].box);
            belowCount += bins[boundary - 1].count;
            if (belowCount == 0 || belowCount == order.count())
                continue;
            float cost = areaOf(below) * float(belowCount) + aboveCosts[boundary];
            if (prF32IsFinite(cost) && (!best.isFound || cost < best.cost))
                best = Split{.axis = axis, .bin = boundary, .firstCount = belowCount, .cost = cost, .isFound = true};
        }
    }
    return best;
}

// Moves the triangles of `order` whose centers are in bins below the split to the front. Returns how many moved.
uint32_t partition(const Box* boxes, Span<uint32_t> order, const Box& centers, const Split& split) {
    Binning binning = binningOf(centers, split.axis);
    uint32_t firstCount = 0;
    for (uint32_t i = 0; i < order.count(); ++i) {
        uint32_t triangle = order[i];
        if (binOf(binning, axisOf(centerOf(boxes[triangle]), split.axis)) < split.bin) {
            order[i] = order[firstCount];
            order[firstCount++] = triangle;
        }
    }
    return firstCount;
}

// Returns the number of triangles that go to the first child of the node `task`, after arranging the node's triangles
// so that those come first. Returns zero if the node is a leaf.
uint32_t splitNode(const Box* boxes, Span<uint32_t> order, const BuildTask& task, const Box& bounds, const Box& centers) {
    if (task.count == 1)
        return 0;
    if (task.depth < kHalvingDepth) {
        Split split = findSplit(boxes, order, centers);
        if (split.isFound) {
            float splitCost = kTraversalCost + split.cost / areaOf(bounds);
            if (task.count <= kLeafMaxTriangles && !(splitCost < float(task.count)))
                return 0;
            if (partition(boxes, order, centers, split) == split.firstCount)
                return split.firstCount;
        }
    }
    return task.count > kLeafMaxTriangles ? task.count / 2 : 0;
}

// Writes to `boxes` the box of each triangle, and to `order` the triangles whose coordinates are all finite. Returns
// their number.
uint32_t collectTriangles(PrMeshAttributeView positions, Span<const uint32_t> indices, Box* boxes, uint32_t* order) {
    uint32_t count = 0;
    for (uint32_t triangle = 0; triangle < indices.count() / 3; ++triangle) {
        Box box = emptyBox();
        bool isFinite = true;
        for (uint32_t corner = 0; corner < 3; ++corner) {
            PrVec3 position = meshReadVec3(positions, indices[triangle * 3 + corner]);
            isFinite = isFinite && meshIsFinite(position);
            addPoint(&box, position);
        }
        boxes[triangle] = box;
        if (isFinite)
            order[count++] = triangle;
    }
    return count;
}

// A ray prepared for box tests.
struct BoxRay {
    PrVec3 origin;
    PrVec3 direction;
    PrVec3 inverse;                 // one over each coordinate of the direction
};

// Returns true if the ray meets the node's box between `tMin` and `tMax`, and writes where it enters to `entry`.
bool hitsBox(const PrMeshBvhNode& node, const BoxRay& ray, float tMin, float tMax, float* entry) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
        float origin = axisOf(ray.origin, axis);
        float low = axisOf(node.min, axis);
        float high = axisOf(node.max, axis);
        if (axisOf(ray.direction, axis) == 0) {
            if (!(origin >= low && origin <= high))
                return false;
            continue;
        }
        float inverse = axisOf(ray.inverse, axis);
        float near = (low - origin) * inverse;
        float far = (high - origin) * inverse;
        if (near > far) {
            float swapped = near;
            near = far;
            far = swapped;
        }
        if (near > tMin)
            tMin = near;
        if (far < tMax)
            tMax = far;
    }
    *entry = tMin;
    return tMin <= tMax;
}

// Tests the ray against the triangle with corners `p`, by Moller and Trumbore's method. Returns true and updates `hit`
// if the ray meets the triangle nearer than `hit->t`.
bool hitsTriangle(const PrVec3 p[PR_NONNULL 3], const PrMeshRay& ray, PrMeshRayHit* hit) {
    PrVec3 edge1 = p[1] - p[0];
    PrVec3 edge2 = p[2] - p[0];
    PrVec3 pvec = prVec3Cross(ray.direction, edge2);
    float determinant = prVec3Dot(edge1, pvec);
    if (determinant == 0)
        return false;
    float inverse = 1.0f / determinant;
    PrVec3 tvec = ray.origin - p[0];
    float u = prVec3Dot(tvec, pvec) * inverse;
    if (!(u >= 0 && u <= 1))
        return false;
    PrVec3 qvec = prVec3Cross(tvec, edge1);
    float v = prVec3Dot(ray.direction, qvec) * inverse;
    if (!(v >= 0 && u + v <= 1))
        return false;
    float t = prVec3Dot(edge2, qvec) * inverse;
    if (!(t >= ray.tMin && t < hit->t))
        return false;
    hit->t = t;
    hit->barycentrics = PrVec2{u, v};
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prMeshBvhMaxNodes(uint32_t triangleCount) {
    if (triangleCount == 0)
        return 0;
    return triangleCount > 0x80000000u ? 0xffffffffu : triangleCount * 2 - 1;
}

PrStatus prMeshBvhBuild(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshBvhNode* PR_NULLABLE nodes,
        uint32_t* PR_NULLABLE triangles, PrMeshBvhView* out) {
    *out = PrMeshBvhView{};
    if (!meshAttributeIsReadable(positions, 3))
        return PrStatus_InvalidData;
    RawArray<uint32_t> loaded;
    PrStatus status = meshTrianglesLoad(indices, positions.count, &loaded);
    if (status != PrStatus_Ok || loaded.count == 0) {
        loaded.reset();
        return status;
    }
    PR_ASSERT(nodes != nullptr && triangles != nullptr);

    RawArray<Box> boxes;            // by triangle
    boxes.resize(loaded.count / 3);
    uint32_t triangleCount = collectTriangles(positions, loaded.span(), boxes.data, triangles);
    loaded.reset();

    uint32_t nodeCount = 0;
    BuildTask stack[kPrMeshBvhMaxDepth + 1];
    uint32_t stackCount = 0;
    if (triangleCount != 0) {
        nodeCount = 1;
        stack[stackCount++] = BuildTask{.node = 0, .first = 0, .count = triangleCount, .depth = 1};
    }
    while (stackCount != 0) {
        BuildTask task = stack[--stackCount];
        Span<uint32_t> order(triangles + task.first, task.count);
        Box bounds = emptyBox();
        Box centers = emptyBox();
        for (uint32_t triangle : order) {
            addBox(&bounds, boxes.data[triangle]);
            addPoint(&centers, centerOf(boxes.data[triangle]));
        }
        uint32_t firstCount = splitNode(boxes.data, order, task, bounds, centers);
        PrMeshBvhNode& node = nodes[task.node];
        node.min = bounds.min;
        node.max = bounds.max;
        if (firstCount == 0) {
            node.first = task.first;
            node.count = task.count;
            continue;
        }
        node.first = nodeCount;
        node.count = 0;
        nodeCount += 2;
        // The first child is built next, so the stack holds one waiting node for each level of the path.
        stack[stackCount++] = BuildTask{
            .node = node.first + 1,
            .first = task.first + firstCount,
            .count = task.count - firstCount,
            .depth = task.depth + 1,
        };
        stack[stackCount++] = BuildTask{
            .node = node.first,
            .first = task.first,
            .count = firstCount,
            .depth = task.depth + 1,
        };
    }
    boxes.reset();
    *out =
        PrMeshBvhView{.nodes = nodes, .nodeCount = nodeCount, .triangleCount = triangleCount, .triangles = triangles};
    return PrStatus_Ok;
}

PrStatus prMeshBvhRefit(PrMeshBvhView bvh, PrMeshAttributeView positions, PrMeshIndexView indices,
        PrMeshBvhNode* PR_NULLABLE out) {
    if (!meshAttributeIsReadable(positions, 3))
        return PrStatus_InvalidData;
    if (bvh.nodeCount == 0)
        return PrStatus_Ok;
    PR_ASSERT(out != nullptr && bvh.nodes != nullptr);
    uint32_t meshTriangleCount = indices.count / 3;
    // A child's index is greater than its parent's, so a pass from the last node reaches every node after its children.
    // Each node's own fields are read before its entry of `out` is written, which may be the same node.
    for (uint32_t i = bvh.nodeCount; i-- > 0;) {
        PrMeshBvhNode node = bvh.nodes[i];
        Box box = emptyBox();
        if (node.count != 0) {
            for (uint32_t entry = node.first; entry < node.first + node.count; ++entry) {
                uint32_t triangle = bvh.triangles[entry];
                if (triangle >= meshTriangleCount)
                    continue;
                Box triangleBox = emptyBox();
                bool isFinite = true;
                for (uint32_t corner = 0; corner < 3 && isFinite; ++corner) {
                    uint32_t vertex = meshIndex(indices, triangle * 3 + corner);
                    PrVec3 position = vertex < positions.count ? meshReadVec3(positions, vertex) : PrVec3{};
                    isFinite = vertex < positions.count && meshIsFinite(position);
                    addPoint(&triangleBox, position);
                }
                if (isFinite)
                    addBox(&box, triangleBox);
            }
        } else {
            // An empty box is the identity of the union, which addBox is not for an empty child.
            const PrMeshBvhNode& first = out[node.first];
            const PrMeshBvhNode& second = out[node.first + 1];
            box.min = PrVec3{
                lesser(first.min.x, second.min.x), lesser(first.min.y, second.min.y),
                lesser(first.min.z, second.min.z),
            };
            box.max = PrVec3{
                greater(first.max.x, second.max.x), greater(first.max.y, second.max.y),
                greater(first.max.z, second.max.z),
            };
        }
        node.min = box.min;
        node.max = box.max;
        out[i] = node;
    }
    return PrStatus_Ok;
}

float prMeshBvhCost(PrMeshBvhView bvh) {
    if (bvh.nodeCount == 0)
        return 0;
    float rootArea = areaOf(Box{bvh.nodes[0].min, bvh.nodes[0].max});
    bool isWeighted = rootArea > 0 && prF32IsFinite(rootArea);
    float cost = 0;
    for (uint32_t i = 0; i < bvh.nodeCount; ++i) {
        const PrMeshBvhNode& node = bvh.nodes[i];
        float area = areaOf(Box{node.min, node.max});
        // An empty box, which holds only triangles that are not finite, is never entered.
        float weight = 1;
        if (isWeighted)
            weight = node.min.x <= node.max.x ? area / rootArea : 0;
        cost += weight * (node.count != 0 ? float(node.count) : kTraversalCost);
    }
    return cost;
}

bool prMeshBvhIsValid(PrMeshBvhView bvh, uint32_t triangleCount) {
    if (bvh.nodeCount == 0)
        return bvh.triangleCount == 0;
    if (bvh.nodes == nullptr || bvh.triangles == nullptr || bvh.triangleCount == 0)
        return false;
    for (uint32_t i = 0; i < bvh.triangleCount; ++i) {
        if (bvh.triangles[i] >= triangleCount)
            return false;
    }

    // A child's index is greater than its parent's, so one pass in order reaches every node after its parent.
    RawArray<uint8_t> depths;       // by node: the number of nodes from the root, or zero if no node refers to it
    depths.resizeZeroed(bvh.nodeCount);
    depths.data[0] = 1;
    bool isValid = true;
    for (uint32_t i = 0; i < bvh.nodeCount && isValid; ++i) {
        const PrMeshBvhNode& node = bvh.nodes[i];
        uint8_t depth = depths.data[i];
        if (depth == 0) {
            isValid = false;
        } else if (node.count != 0) {
            isValid = node.first < bvh.triangleCount && node.count <= bvh.triangleCount - node.first;
        } else {
            isValid = node.first > i && node.first < bvh.nodeCount - 1 && depth < kPrMeshBvhMaxDepth &&
                depths.data[node.first] == 0 && depths.data[node.first + 1] == 0;
            if (isValid) {
                depths.data[node.first] = uint8_t(depth + 1);
                depths.data[node.first + 1] = uint8_t(depth + 1);
            }
        }
    }
    depths.reset();
    return isValid;
}

bool prMeshBvhIntersect(PrMeshBvhView bvh, PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshRay ray,
        PrMeshRayHit* out) {
    *out = PrMeshRayHit{.triangle = kPrMeshIndexNone};
    PrVec3 direction = ray.direction;
    bool hasDirection = direction.x != 0 || direction.y != 0 || direction.z != 0;
    if (bvh.nodeCount == 0 || !meshAttributeIsReadable(positions, 3) || meshIndexSize(indices.type) == 0 ||
            !meshIsFinite(ray.origin) || !meshIsFinite(direction) || !hasDirection || !prF32IsFinite(ray.tMin) ||
            prF32IsNan(ray.tMax))
        return false;
    BoxRay boxRay{
        .origin = ray.origin,
        .direction = direction,
        .inverse = PrVec3{1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z},
    };
    uint32_t meshTriangleCount = indices.count / 3;

    PrMeshRayHit hit{.triangle = kPrMeshIndexNone, .t = ray.tMax};
    uint32_t stack[kPrMeshBvhMaxDepth];
    uint32_t stackCount = 0;
    float entry;
    uint32_t current = hitsBox(bvh.nodes[0], boxRay, ray.tMin, hit.t, &entry) ? 0 : kPrMeshIndexNone;
    for (;;) {
        if (current == kPrMeshIndexNone) {
            if (stackCount == 0)
                break;
            current = stack[--stackCount];
            // The hit may have come nearer since the node was pushed.
            if (!hitsBox(bvh.nodes[current], boxRay, ray.tMin, hit.t, &entry)) {
                current = kPrMeshIndexNone;
                continue;
            }
        }
        const PrMeshBvhNode& node = bvh.nodes[current];
        current = kPrMeshIndexNone;
        if (node.count != 0) {
            if (node.first >= bvh.triangleCount || node.count > bvh.triangleCount - node.first)
                continue;
            for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                uint32_t triangle = bvh.triangles[i];
                if (triangle >= meshTriangleCount)
                    continue;
                uint32_t vertices[3] = {
                    meshIndex(indices, triangle * 3),
                    meshIndex(indices, triangle * 3 + 1),
                    meshIndex(indices, triangle * 3 + 2),
                };
                if (vertices[0] >= positions.count || vertices[1] >= positions.count || vertices[2] >= positions.count)
                    continue;
                PrVec3 p[3] = {
                    meshReadVec3(positions, vertices[0]),
                    meshReadVec3(positions, vertices[1]),
                    meshReadVec3(positions, vertices[2]),
                };
                if (hitsTriangle(p, ray, &hit))
                    hit.triangle = triangle;
            }
            continue;
        }
        if (node.first >= bvh.nodeCount - 1)
            continue;
        float entries[2];
        bool hits[2] = {
            hitsBox(bvh.nodes[node.first], boxRay, ray.tMin, hit.t, &entries[0]),
            hitsBox(bvh.nodes[node.first + 1], boxRay, ray.tMin, hit.t, &entries[1]),
        };
        // The nearer child is visited first, and the farther one waits on the stack.
        uint32_t nearer = hits[1] && (!hits[0] || entries[1] < entries[0]) ? 1 : 0;
        uint32_t farther = 1 - nearer;
        if (hits[farther] && stackCount < kPrMeshBvhMaxDepth)
            stack[stackCount++] = node.first + farther;
        if (hits[nearer])
            current = node.first + nearer;
    }
    if (hit.triangle == kPrMeshIndexNone)
        return false;
    *out = hit;
    return true;
}
