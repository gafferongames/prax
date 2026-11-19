// The tangent generators that lib/mesh's tangents are compared with, behind one interface.

#include "tools/suite/tangents/tangent_references.h"

#include "pr/mem.h"

#include "meshoptimizer.h"
#include "mikktspace.h"

namespace tangents {

namespace {

// The context's user data, which carries the output beside the input.
struct Mikk {
    Corners corners;
    float* out;
};

const Corners& cornersOf(const SMikkTSpaceContext* context) {
    return static_cast<const Mikk*>(context->m_pUserData)->corners;
}

int faceCount(const SMikkTSpaceContext* context) {
    return int(cornersOf(context).count / 3);
}

int verticesOfFace(const SMikkTSpaceContext*, const int) {
    return 3;
}

void position(const SMikkTSpaceContext* context, float out[], const int face, const int vertex) {
    prMemCopy(out, cornersOf(context).positions + (face * 3 + vertex) * 3, 3 * sizeof(float));
}

void normal(const SMikkTSpaceContext* context, float out[], const int face, const int vertex) {
    prMemCopy(out, cornersOf(context).normals + (face * 3 + vertex) * 3, 3 * sizeof(float));
}

void texcoord(const SMikkTSpaceContext* context, float out[], const int face, const int vertex) {
    prMemCopy(out, cornersOf(context).texcoords + (face * 3 + vertex) * 2, 2 * sizeof(float));
}

void setTangent(const SMikkTSpaceContext* context, const float tangent[], const float sign, const int face, const int vertex) {
    const Mikk* mikk = static_cast<const Mikk*>(context->m_pUserData);
    float* at = mikk->out + (face * 3 + vertex) * 4;
    prMemCopy(at, tangent, 3 * sizeof(float));
    at[3] = sign;
}

} // namespace

bool mikktspace(const Corners& corners, float* out) {
    Mikk mikk{.corners = corners, .out = out};
    SMikkTSpaceInterface callbacks{
        .m_getNumFaces = faceCount,
        .m_getNumVerticesOfFace = verticesOfFace,
        .m_getPosition = position,
        .m_getNormal = normal,
        .m_getTexCoord = texcoord,
        .m_setTSpaceBasic = setTangent,
        .m_setTSpace = nullptr,
    };
    SMikkTSpaceContext context{.m_pInterface = &callbacks, .m_pUserData = &mikk};
    return genTangSpaceDefault(&context) != 0;
}

void meshoptimizer(const Corners& corners, bool compatible, float* out) {
    meshopt_generateTangents(out, nullptr, corners.count, corners.positions, corners.count, 3 * sizeof(float),
        corners.normals, 3 * sizeof(float), corners.texcoords, 2 * sizeof(float), compatible ? meshopt_TangentCompatible : 0);
}

} // namespace tangents
