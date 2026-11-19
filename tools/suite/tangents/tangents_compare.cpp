// The tangents harness, which tools/suite/tangents.py drives: lib/mesh's tangents beside MikkTSpace's and
// meshoptimizer's, over the primitives of glTF assets.
//
// `tangents_compare [--time] [--total] <path>...` reads the glTF asset at each path and, for every primitive that has
// triangles and a first set of texture coordinates, makes the unindexed corners that lib/model gives prMeshTangents
// (lib/model/src/model_mesh.cpp): the positions as they are, the normals at unit length, or flat normals if the
// primitive has none, and the texture coordinates with each value that is not finite as zero. Every generator gets
// those same corners. The harness writes a line of JSON for each asset:
//
//   {"status": the name of the read's status, "primitives": the primitives compared, "corners": their corners,
//    "engines": {name: comparison}, "time": {name: milliseconds}}
//
// The engines are "prax" (prMeshTangents), "meshopt" (meshopt_generateTangents) and "meshoptCompatible" (the same with
// meshopt_TangentCompatible), each compared with MikkTSpace corner by corner. A comparison is:
//
//   "equal": corners whose four floats equal the reference's,
//   "signs": corners whose handedness differs from the reference's,
//   "fallback": corners at which the reference has no tangent of its own and the engine has a replacement, which are
//       counted here and in no other count: the reference's tangent is zero or its default of (1, 0, 0) with a
//       handedness of -1, and the engine's is a unit tangent perpendicular to the normal with a handedness of 1, as
//       lib/mesh writes its replacement tangent,
//   "max": the largest angle between the engine's tangent and the reference's, in radians,
//   "bins": the corners by that angle: exactly zero, then at most 1e-7, 1e-6, ... 1e-1 radians, then more,
//   "kinds": the corners whose tangent is more than kFar from the reference's or whose handedness differs, by what is
//       around them (Kind below).
//
// "time" is present with --time: what one run of each generator over every primitive of the asset takes, measured as
// the harness measures a benchmark (harnessMeasure). --total writes a last line, {"total": {name: summary}}, with the
// angles of every asset together: the counts above, and the 50th, 99th and 99.9th percentiles of the angle.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gltf.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/mesh_normals.h"
#include "pr/mesh_tangents.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/vec.h"

#include "tools/harness/harness.h"
#include "tools/suite/tangents/tangent_references.h"

#include <math.h>

using namespace harness;
using namespace pr;

namespace {

constexpr uint32_t kEngines = 3;
const char* const kEngineNames[kEngines] = {"prax", "meshopt", "meshoptCompatible"};

constexpr uint32_t kBins = 9;

// The angle, in radians, past which a tangent is counted by what is around its corner: past what rounding explains.
constexpr double kFar = 1e-4;

// The smallest magnitude that MikkTSpace counts as different from zero: FLT_MIN, the smallest normal float.
constexpr float kTiny = 1.17549435e-38f;

// What surrounds a corner whose tangent differs, from the first that applies. A welded vertex is the corners with equal
// position, normal and texture coordinate, which MikkTSpace treats as one vertex.
enum {
    Kind_Degenerate,  // its triangle has two equal positions
    Kind_Uvless,      // its triangle's texture mapping has no area, so the triangle joins any group
    Kind_BesideBad,   // its welded vertex has a corner of a triangle of the two kinds above
    Kind_Nonmanifold, // its welded vertex is on an edge that more than two triangles share
    Kind_Mirror,      // its welded vertex has triangles of both texture orientations
    Kind_Regular,
    Kind_Count,
};
typedef uint8_t Kind;
const char* const kKindNames[Kind_Count] = {"degenerate", "uvless", "besideBad", "nonmanifold", "mirror", "regular"};

// One engine's comparison with the reference.
struct Comparison {
    uint64_t equal;
    uint64_t signs;
    uint64_t fallback;
    double max;
    uint64_t bins[kBins];
    uint64_t kinds[Kind_Count];
};

// The corners of one primitive, unindexed, as lib/model gives them to prMeshTangents.
struct Primitive {
    Array<float> positions;
    Array<float> normals;
    Array<float> texcoords;
    uint32_t count;

    tangents::Corners corners() const {
        return tangents::Corners{positions.data(), normals.data(), texcoords.data(), count};
    }
};

float finiteOr(float value, float fallback) {
    return prF32IsFinite(value) ? value : fallback;
}

// Returns false if the primitive has nothing to compare.
bool unshare(const PrGltf* gltf, uint32_t p, Primitive* out) {
    PrGltfAccessorIndex positions = prGltfPrimitiveAttribute(gltf, p, PrGltfSemantic_Position, 0);
    PrGltfAccessorIndex normals = prGltfPrimitiveAttribute(gltf, p, PrGltfSemantic_Normal, 0);
    PrGltfAccessorIndex texcoords = prGltfPrimitiveAttribute(gltf, p, PrGltfSemantic_TexCoord, 0);
    uint32_t count = prGltfPrimitiveTriangles(gltf, p, nullptr, 0);
    if (positions == kPrGltfNone || texcoords == kPrGltfNone || count == 0)
        return false;
    uint32_t vertexCount = prGltfAccessors(gltf)[positions].count;
    Array<float> sourcePositions(vertexCount * 3);
    Array<float> sourceNormals(normals != kPrGltfNone ? vertexCount * 3 : 0);
    Array<float> sourceTexcoords(vertexCount * 2);
    Array<uint32_t> triangles(count);
    if (!prGltfAccessorReadFloats(gltf, positions, 0, vertexCount, sourcePositions.data()) ||
            !prGltfAccessorReadFloats(gltf, texcoords, 0, vertexCount, sourceTexcoords.data()) ||
            (normals != kPrGltfNone && !prGltfAccessorReadFloats(gltf, normals, 0, vertexCount, sourceNormals.data())) ||
            prGltfPrimitiveTriangles(gltf, p, triangles.data(), count) != count)
        return false;
    out->count = count;
    out->positions.resize(count * 3);
    out->normals.resize(count * 3);
    out->texcoords.resize(count * 2);
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t i = triangles[k];
        prMemCopy(&out->positions[k * 3], &sourcePositions[i * 3], 3 * sizeof(float));
        if (normals != kPrGltfNone) {
            // As lib/model's unitOr does.
            const float* v = &sourceNormals[i * 3];
            float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            PrVec3 unit{0, 0, 1};
            if (prF32IsFinite(length2) && length2 >= 1e-20f) {
                float scale = 1 / prF32Sqrt(length2);
                unit = PrVec3{v[0] * scale, v[1] * scale, v[2] * scale};
            }
            prMemCopy(&out->normals[k * 3], &unit, 3 * sizeof(float));
        }
        out->texcoords[k * 2] = finiteOr(sourceTexcoords[i * 2], 0);
        out->texcoords[k * 2 + 1] = finiteOr(sourceTexcoords[i * 2 + 1], 0);
    }
    if (normals == kPrGltfNone) {
        prMeshFlatNormals(PrMeshAttributeView{out->positions.data(), count, 3 * sizeof(float), prMeshFormatFloat32(3)},
            PrMeshIndexView{nullptr, count, PrMeshIndexType_U32},
            PrMeshAttribute{out->normals.data(), count, 3 * sizeof(float), prMeshFormatFloat32(3)});
    }
    return true;
}

bool prax(const Primitive& p, float* out) {
    return prMeshTangents(PrMeshAttributeView{p.positions.data(), p.count, 3 * sizeof(float), prMeshFormatFloat32(3)},
        PrMeshAttributeView{p.normals.data(), p.count, 3 * sizeof(float), prMeshFormatFloat32(3)},
        PrMeshAttributeView{p.texcoords.data(), p.count, 2 * sizeof(float), prMeshFormatFloat32(2)},
        PrMeshIndexView{nullptr, p.count, PrMeshIndexType_U32},
        PrMeshAttribute{out, p.count, 4 * sizeof(float), prMeshFormatFloat32(4)}) == PrStatus_Ok;
}

void runEngine(uint32_t engine, const Primitive& p, float* out) {
    if (engine == 0)
        prax(p, out);
    else
        tangents::meshoptimizer(p.corners(), engine == 2, out);
}

// The key that MikkTSpace welds a corner by: its position, normal and texture coordinate, with zero for negative zero.
struct Key {
    float values[8];
};

Key keyOf(const Primitive& p, uint32_t corner) {
    Key key;
    prMemCopy(key.values, &p.positions[corner * 3], 3 * sizeof(float));
    prMemCopy(key.values + 3, &p.normals[corner * 3], 3 * sizeof(float));
    prMemCopy(key.values + 6, &p.texcoords[corner * 2], 2 * sizeof(float));
    for (float& value : key.values)
        value = value == 0 ? 0.0f : value;
    return key;
}

bool equalPositions(const float* a, const float* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

// Returns each corner's kind, as Kind describes it.
Array<uint8_t> kindsOf(const Primitive& p) {
    uint32_t count = p.count;
    uint32_t triangleCount = count / 3;
    // Weld the corners by sorting them by key.
    Array<Key> keys(count);
    Array<uint32_t> order(count);
    for (uint32_t k = 0; k < count; ++k) {
        keys[k] = keyOf(p, k);
        order[k] = k;
    }
    sortItems(order.span(), [&](uint32_t a, uint32_t b) {
        for (uint32_t i = 0; i < 8; ++i) {
            if (keys[a].values[i] != keys[b].values[i])
                return keys[a].values[i] < keys[b].values[i];
        }
        return a < b;
    });
    Array<uint32_t> welded(count);
    uint32_t vertexCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        bool same = i > 0 && prMemEquals(&keys[order[i]], &keys[order[i - 1]], sizeof(Key));
        if (!same)
            ++vertexCount;
        welded[order[i]] = vertexCount - 1;
    }

    // What each triangle is, as MikkTSpace decides it.
    Array<uint8_t> degenerate(triangleCount);
    Array<uint8_t> uvless(triangleCount);
    Array<uint8_t> preserves(triangleCount);
    for (uint32_t t = 0; t < triangleCount; ++t) {
        const float* p0 = &p.positions[t * 9];
        const float* p1 = p0 + 3;
        const float* p2 = p0 + 6;
        degenerate[t] = equalPositions(p0, p1) || equalPositions(p0, p2) || equalPositions(p1, p2);
        const float* t0 = &p.texcoords[t * 6];
        float t21x = t0[2] - t0[0];
        float t21y = t0[3] - t0[1];
        float t31x = t0[4] - t0[0];
        float t31y = t0[5] - t0[1];
        float area = t21x * t31y - t21y * t31x;
        preserves[t] = area > 0;
        PrVec3 d1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        PrVec3 d2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        PrVec3 os = d1 * t31y - d2 * t21y;
        PrVec3 ot = d1 * -t31x + d2 * t21x;
        float absArea = prF32Abs(area);
        uvless[t] = !(absArea > kTiny) || !(prVec3Length(os) / absArea > kTiny) || !(prVec3Length(ot) / absArea > kTiny);
    }

    // What each welded vertex has around it.
    enum : uint8_t { HasBad = 1, HasPreserving = 2, HasMirrored = 4, OnNonmanifold = 8 };
    Array<uint8_t> around(vertexCount, uint8_t(0));
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t t = k / 3;
        uint8_t& a = around[welded[k]];
        if (degenerate[t] || uvless[t])
            a |= HasBad;
        else
            a |= preserves[t] ? HasPreserving : HasMirrored;
    }
    // The edges of the triangles that are not degenerate, by their welded ends.
    struct Edge {
        uint32_t low;
        uint32_t high;
    };
    Array<Edge> edges;
    for (uint32_t t = 0; t < triangleCount; ++t) {
        if (degenerate[t])
            continue;
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t a = welded[t * 3 + i];
            uint32_t b = welded[t * 3 + (i + 1) % 3];
            edges.push(Edge{a < b ? a : b, a < b ? b : a});
        }
    }
    sortItems(edges.span(), [](const Edge& a, const Edge& b) {
        return a.low != b.low ? a.low < b.low : a.high < b.high;
    });
    for (uint32_t start = 0; start < edges.count();) {
        uint32_t end = start + 1;
        while (end < edges.count() && edges[end].low == edges[start].low && edges[end].high == edges[start].high)
            ++end;
        if (end - start > 2) {
            around[edges[start].low] |= OnNonmanifold;
            around[edges[start].high] |= OnNonmanifold;
        }
        start = end;
    }

    Array<uint8_t> kinds(count);
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t t = k / 3;
        uint8_t a = around[welded[k]];
        Kind kind = Kind_Regular;
        if (degenerate[t])
            kind = Kind_Degenerate;
        else if (uvless[t])
            kind = Kind_Uvless;
        else if (a & HasBad)
            kind = Kind_BesideBad;
        else if (a & OnNonmanifold)
            kind = Kind_Nonmanifold;
        else if ((a & HasPreserving) && (a & HasMirrored))
            kind = Kind_Mirror;
        kinds[k] = kind;
    }
    return kinds;
}

// Returns the angle between two tangents in radians, or pi if either has no direction.
double angleBetween(const float* a, const float* b) {
    double ax = double(a[0]), ay = double(a[1]), az = double(a[2]);
    double bx = double(b[0]), by = double(b[1]), bz = double(b[2]);
    double cx = ay * bz - az * by;
    double cy = az * bx - ax * bz;
    double cz = ax * by - ay * bx;
    double cross = sqrt(cx * cx + cy * cy + cz * cz);
    double dot = ax * bx + ay * by + az * bz;
    if (ax * ax + ay * ay + az * az == 0 || bx * bx + by * by + bz * bz == 0)
        return kPrF64Pi;
    return atan2(cross, dot);
}

uint32_t binOf(double angle) {
    if (angle == 0)
        return 0;
    double bound = 1e-7;
    for (uint32_t bin = 1; bin < kBins - 1; ++bin, bound *= 10) {
        if (angle <= bound)
            return bin;
    }
    return kBins - 1;
}

// Returns true if the reference has no tangent of its own at corner k, where the engine has a replacement, as
// Comparison's "fallback" describes.
bool isReferenceFallback(const Primitive& p, uint32_t k, const float* reference, const float* engine) {
    float length2 = reference[0] * reference[0] + reference[1] * reference[1] + reference[2] * reference[2];
    bool isDefault = reference[0] == 1 && reference[1] == 0 && reference[2] == 0 && reference[3] == -1;
    if (length2 > 0.5f && length2 < 2.0f && !isDefault)
        return false;
    PrVec3 tangent{engine[0], engine[1], engine[2]};
    PrVec3 normal{p.normals[k * 3], p.normals[k * 3 + 1], p.normals[k * 3 + 2]};
    return engine[3] == 1 && prF32Abs(prVec3Length(tangent) - 1) < 1e-5f && prF32Abs(prVec3Dot(tangent, normal)) < 1e-5f;
}

void compare(const Primitive& p, const float* reference, const float* engine, const Array<uint8_t>& kinds,
        Comparison* c, Array<double>* angles) {
    for (uint32_t k = 0; k < p.count; ++k) {
        const float* r = reference + k * 4;
        const float* e = engine + k * 4;
        bool equal = r[0] == e[0] && r[1] == e[1] && r[2] == e[2] && r[3] == e[3];
        if (!equal && isReferenceFallback(p, k, r, e)) {
            ++c->fallback;
            continue;
        }
        double angle = angleBetween(r, e);
        if (equal) {
            ++c->equal;
            angle = 0;
        }
        if (angle > kFar || r[3] != e[3])
            ++c->kinds[kinds[k]];
        c->signs += r[3] != e[3];
        c->max = angle > c->max ? angle : c->max;
        ++c->bins[binOf(angle)];
        if (angles != nullptr)
            angles->push(angle);
    }
}

void addComparison(Comparison* to, const Comparison& from) {
    to->equal += from.equal;
    to->signs += from.signs;
    to->fallback += from.fallback;
    to->max = from.max > to->max ? from.max : to->max;
    for (uint32_t i = 0; i < kBins; ++i)
        to->bins[i] += from.bins[i];
    for (uint32_t i = 0; i < Kind_Count; ++i)
        to->kinds[i] += from.kinds[i];
}

void writeComparison(PrTextBuffer* out, const Comparison& c) {
    harnessAppendFormat(out, "{\"equal\":%llu,\"signs\":%llu,\"fallback\":%llu,\"max\":%.9g,\"bins\":[",
        (unsigned long long)c.equal, (unsigned long long)c.signs, (unsigned long long)c.fallback, c.max);
    for (uint32_t i = 0; i < kBins; ++i)
        harnessAppendFormat(out, "%s%llu", i == 0 ? "" : ",", (unsigned long long)c.bins[i]);
    prTextBufferAppend(out, "],\"kinds\":{");
    for (uint32_t i = 0; i < Kind_Count; ++i)
        harnessAppendFormat(out, "%s\"%s\":%llu", i == 0 ? "" : ",", kKindNames[i], (unsigned long long)c.kinds[i]);
    prTextBufferAppend(out, "}}");
}

// What the run compares across every asset, for --total.
struct Totals {
    Comparison comparisons[kEngines];
    Array<double> angles[kEngines];
    uint64_t corners;
};

double percentile(Array<double>& values, double fraction) {
    if (values.count() == 0)
        return 0;
    uint32_t at = uint32_t(fraction * double(values.count() - 1) + 0.5);
    return values[at];
}

void run(const char* path, bool times, Totals* totals) {
    PrTextBuffer out = {};
    PrGltf* gltf = nullptr;
    PrStatus status = prGltfFileRead(path, nullptr, &gltf);
    Array<Primitive> primitives;
    for (uint32_t p = 0; gltf != nullptr && p < prGltfPrimitiveCount(gltf); ++p) {
        Primitive primitive;
        if (unshare(gltf, p, &primitive))
            primitives.push(move(primitive));
    }
    prGltfDestroy(gltf);

    Comparison comparisons[kEngines] = {};
    uint64_t corners = 0;
    for (const Primitive& p : primitives) {
        corners += p.count;
        Array<float> reference(p.count * 4, 0.0f);
        Array<float> engine(p.count * 4, 0.0f);
        tangents::mikktspace(p.corners(), reference.data());
        Array<uint8_t> kinds = kindsOf(p);
        for (uint32_t e = 0; e < kEngines; ++e) {
            runEngine(e, p, engine.data());
            Comparison c = {};
            compare(p, reference.data(), engine.data(), kinds, &c, totals != nullptr ? &totals->angles[e] : nullptr);
            addComparison(&comparisons[e], c);
        }
    }

    prTextBufferAppend(&out, "{\"status\":");
    harnessWriteString(&out, prStatusName(status));
    harnessAppendFormat(&out, ",\"primitives\":%u,\"corners\":%llu,\"engines\":{", primitives.count(),
        (unsigned long long)corners);
    for (uint32_t e = 0; e < kEngines; ++e) {
        harnessAppendFormat(&out, "%s\"%s\":", e == 0 ? "" : ",", kEngineNames[e]);
        writeComparison(&out, comparisons[e]);
        if (totals != nullptr)
            addComparison(&totals->comparisons[e], comparisons[e]);
    }
    prTextBufferAppendChar(&out, '}');
    if (totals != nullptr)
        totals->corners += corners;

    if (times && primitives.count() != 0) {
        Array<float> scratch;
        for (const Primitive& p : primitives)
            scratch.resize(p.count * 4 > scratch.count() ? p.count * 4 : scratch.count());
        auto measure = [&](uint32_t engine) {
            return harnessMeasure([&] {
                for (const Primitive& p : primitives) {
                    if (engine == kEngines)
                        tangents::mikktspace(p.corners(), scratch.data());
                    else
                        runEngine(engine, p, scratch.data());
                }
            }).millis;
        };
        harnessAppendFormat(&out, ",\"time\":{\"mikktspace\":%.6g", measure(kEngines));
        for (uint32_t e = 0; e < kEngines; ++e)
            harnessAppendFormat(&out, ",\"%s\":%.6g", kEngineNames[e], measure(e));
        prTextBufferAppendChar(&out, '}');
    }
    prTextBufferAppendChar(&out, '}');
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
}

void writeTotals(Totals* totals) {
    PrTextBuffer out = {};
    harnessAppendFormat(&out, "{\"total\":{\"corners\":%llu", (unsigned long long)totals->corners);
    for (uint32_t e = 0; e < kEngines; ++e) {
        Array<double>& angles = totals->angles[e];
        sortItems(angles.span(), [](double a, double b) {
            return a < b;
        });
        harnessAppendFormat(&out, ",\"%s\":{\"comparison\":", kEngineNames[e]);
        writeComparison(&out, totals->comparisons[e]);
        harnessAppendFormat(&out, ",\"p50\":%.9g,\"p99\":%.9g,\"p999\":%.9g}", percentile(angles, 0.5),
            percentile(angles, 0.99), percentile(angles, 0.999));
    }
    prTextBufferAppend(&out, "}}");
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
}

} // namespace

int main(int argc, char** argv) {
    bool times = false;
    bool total = false;
    int first = 1;
    for (; first < argc; ++first) {
        PrStringView option = argv[first];
        if (option == "--time")
            times = true;
        else if (option == "--total")
            total = true;
        else
            break;
    }
    Totals* totals = total ? new Totals{} : nullptr;
    for (int i = first; i < argc; ++i)
        run(argv[i], times, totals);
    if (totals != nullptr) {
        writeTotals(totals);
        delete totals;
    }
    return 0;
}
