// Triangle meshes with known geometry, which lib/mesh's tests and the tools that compare lib/mesh with other
// implementations build alike.

#pragma once

#include "pr/array.h"
#include "pr/float.h"
#include "pr/random.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// A triangle list with one position, normal and texture coordinate for each vertex.
struct MeshShape {
    Array<PrVec3> positions;
    Array<PrVec3> normals;
    Array<PrVec2> texcoords;
    Array<uint32_t> indices;
};

// Returns a flat grid of `columns` by `rows` squares of side 1 in the plane z = 0, facing +z. The texture coordinates
// are the positions divided by the grid's size. The triangles run along the rows.
inline MeshShape meshGrid(uint32_t columns, uint32_t rows) {
    MeshShape shape;
    for (uint32_t y = 0; y <= rows; ++y) {
        for (uint32_t x = 0; x <= columns; ++x) {
            shape.positions.push(PrVec3{float(x), float(y), 0});
            shape.normals.push(PrVec3{0, 0, 1});
            shape.texcoords.push(PrVec2{float(x) / float(columns), float(y) / float(rows)});
        }
    }
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < columns; ++x) {
            uint32_t corner = y * (columns + 1) + x;
            uint32_t quad[6] = {
                corner, corner + 1, corner + columns + 2, corner, corner + columns + 2,
                corner + columns + 1,
            };
            shape.indices.append(quad, 6);
        }
    }
    return shape;
}

// Returns one triangle in the plane z = 0, facing +z, with the texture coordinates given.
inline MeshShape meshTriangle(PrVec2 t0, PrVec2 t1, PrVec2 t2) {
    MeshShape shape;
    shape.positions.push(PrVec3{0, 0, 0});
    shape.positions.push(PrVec3{1, 0, 0});
    shape.positions.push(PrVec3{0, 1, 0});
    shape.texcoords.push(t0);
    shape.texcoords.push(t1);
    shape.texcoords.push(t2);
    for (uint32_t i = 0; i < 3; ++i) {
        shape.normals.push(PrVec3{0, 0, 1});
        shape.indices.push(i);
    }
    return shape;
}

// Returns two triangles in the plane z = 0, facing +z, that share the edge from (0, 0) to (0, 1): the first to its
// left, the second to its right. `texcoords` are those of the vertices (0, 0), (0, 1), (-1, 0) and (1, 0).
inline MeshShape meshTrianglePair(const PrVec2 (&texcoords)[4]) {
    MeshShape shape;
    PrVec3 positions[4] = {{0, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    for (uint32_t i = 0; i < 4; ++i) {
        shape.positions.push(positions[i]);
        shape.normals.push(PrVec3{0, 0, 1});
        shape.texcoords.push(texcoords[i]);
    }
    uint32_t indices[6] = {0, 1, 2, 0, 3, 1};
    shape.indices.append(indices, 6);
    return shape;
}

// Returns a flat disc of radius 1 in the plane z = 0, facing +z, as a fan of `segments` triangles around its center,
// whose texture wraps around the center: the first coordinate follows the angle and the second increases outward.
inline MeshShape meshDisc(uint32_t segments) {
    MeshShape disc;
    disc.positions.push(PrVec3{0, 0, 0});
    disc.normals.push(PrVec3{0, 0, 1});
    disc.texcoords.push(PrVec2{0.5f, 0});
    for (uint32_t i = 0; i <= segments; ++i) {
        float angle = 2 * kPrF32Pi * float(i % segments) / float(segments);
        disc.positions.push(PrVec3{prF32Cos(angle), prF32Sin(angle), 0});
        disc.normals.push(PrVec3{0, 0, 1});
        disc.texcoords.push(PrVec2{float(i) / float(segments), 1});
    }
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t fan[3] = {0, i + 1, i + 2};
        disc.indices.append(fan, 3);
    }
    return disc;
}

// Returns a fan of `count` thin triangles in the plane z = 0, facing +z, around the origin, with a texture laid flat
// over it: the texture coordinates are the positions' x and y.
inline MeshShape meshFan(uint32_t count) {
    MeshShape fan;
    fan.positions.push(PrVec3{0, 0, 0});
    for (uint32_t i = 0; i <= count; ++i)
        fan.positions.push(PrVec3{float(i) / float(count), 1, 0});
    for (PrVec3 position : fan.positions) {
        fan.normals.push(PrVec3{0, 0, 1});
        fan.texcoords.push(PrVec2{position.x, position.y});
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t triangle[3] = {0, i + 2, i + 1};
        fan.indices.append(triangle, 3);
    }
    return fan;
}

// Returns a sphere of radius 1 around the origin, of `segments` around and `rings` from pole to pole, with outward
// normals and a texture mapped by longitude and latitude. The vertices along the seam and at the poles are repeated
// with their own texture coordinates.
inline MeshShape meshSphere(uint32_t segments, uint32_t rings) {
    MeshShape shape;
    for (uint32_t ring = 0; ring <= rings; ++ring) {
        float latitude = kPrF32Pi * float(ring) / float(rings);
        for (uint32_t segment = 0; segment <= segments; ++segment) {
            float longitude = 2 * kPrF32Pi * float(segment) / float(segments);
            PrVec3 point{
                prF32Sin(latitude) * prF32Cos(longitude),
                prF32Cos(latitude),
                prF32Sin(latitude) * prF32Sin(longitude),
            };
            // The poles are exact, so that every vertex of a pole has the same position.
            if (ring == 0 || ring == rings)
                point = PrVec3{0, ring == 0 ? 1.0f : -1.0f, 0};
            float length = prVec3Length(point);
            shape.positions.push(point / length);
            shape.normals.push(point / length);
            shape.texcoords.push(PrVec2{float(segment) / float(segments), float(ring) / float(rings)});
        }
    }
    for (uint32_t ring = 0; ring < rings; ++ring) {
        for (uint32_t segment = 0; segment < segments; ++segment) {
            uint32_t corner = ring * (segments + 1) + segment;
            uint32_t below = corner + segments + 1;
            if (ring != 0) {
                uint32_t triangle[3] = {corner, corner + 1, below + 1};
                shape.indices.append(triangle, 3);
            }
            if (ring != rings - 1) {
                uint32_t triangle[3] = {corner, below + 1, below};
                shape.indices.append(triangle, 3);
            }
        }
    }
    return shape;
}

// Exchanges whole triangles of `indices` into an order that `seed` decides.
inline void meshShuffleTriangles(Array<uint32_t>& indices, uint64_t seed) {
    PrRandom random{.state = seed};
    uint32_t triangleCount = indices.count() / 3;
    for (uint32_t i = triangleCount; i > 1; --i) {
        uint32_t other = prRandomU32Below(&random, i);
        for (uint32_t corner = 0; corner < 3; ++corner) {
            uint32_t moving = indices[(i - 1) * 3 + corner];
            indices[(i - 1) * 3 + corner] = indices[other * 3 + corner];
            indices[other * 3 + corner] = moving;
        }
    }
}

} // namespace harness

PR_ASSUME_NONNULL_END
