// lib/draco's decoder beside the reference decoder, over streams that the reference encoder writes: meshes of each
// shape that changes what the encoder does, with each set of attributes, encoded sequentially and by Edgebreaker with
// each traversal, at every speed, with each prediction scheme forced and with each quantization. The two decoders must
// give the same faces, the same points in the same order, and the same value of every attribute to the bit.
//
// The streams also stand for data from outside the program: each is decoded cut short at every length and with each
// byte changed, which must fail or succeed without reading outside the stream.

#include "pr/draco.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// The reference encoder chooses between its standard and valence traversals and never writes the predictive one, which
// its decoder still reads. The encoder of the predictive traversal is in its sources, and is reached only by setting
// the implementation that the encoder keeps private.
#define private protected
#include "draco/compression/mesh/mesh_edgebreaker_encoder.h"
#undef private
#include "draco/compression/decode.h"
#include "draco/compression/expert_encode.h"
#include "draco/compression/mesh/mesh_edgebreaker_encoder_impl.h"
#include "draco/compression/mesh/mesh_edgebreaker_traversal_predictive_encoder.h"
#include "draco/mesh/triangle_soup_mesh_builder.h"
#include "draco/point_cloud/point_cloud_builder.h"

#include "tools/harness/test.h"

namespace {

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545f491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float unit() { return float(next() >> 8) / float(1u << 24); }
};

// The attributes that a mesh is built with.
enum : uint32_t {
    With_Normals = 1 << 0,
    With_TexCoords = 1 << 1,
    With_Colors = 1 << 2,
    With_Joints = 1 << 3,               // a generic attribute of four 16-bit integers
    With_Weights = 1 << 4,              // a generic attribute of four floats
    With_Scalar = 1 << 5,               // a generic attribute of one signed 32-bit integer
    With_HardNormals = 1 << 6,          // a normal for each face, so that every edge is a seam of the normals
    With_All = With_Normals | With_TexCoords | With_Colors | With_Joints | With_Weights | With_Scalar,
};
typedef uint32_t With;

// A mesh as a test describes it: vertices with positions, and faces of three vertices. A texture coordinate is given
// for each corner, so that a face can disagree with its neighbour about a vertex, which is a seam.
struct Shape {
    const char* name;
    std::vector<float> positions;       // three for a vertex
    std::vector<uint32_t> faces;        // three vertices for a face
    std::vector<float> cornerUvs;       // two for a corner
};

void addVertex(Shape* shape, float x, float y, float z) {
    shape->positions.insert(shape->positions.end(), {x, y, z});
}

void addFace(Shape* shape, uint32_t a, uint32_t b, uint32_t c, const float* uvs) {
    shape->faces.insert(shape->faces.end(), {a, b, c});
    shape->cornerUvs.insert(shape->cornerUvs.end(), uvs, uvs + 6);
}

// A sheet of `columns` by `rows` cells that waves, with the cells that `hole` names left out.
Shape grid(const char* name, uint32_t columns, uint32_t rows, bool (*hole)(uint32_t, uint32_t) = nullptr) {
    Shape shape = {name, {}, {}, {}};
    for (uint32_t y = 0; y <= rows; ++y) {
        for (uint32_t x = 0; x <= columns; ++x)
            addVertex(&shape, float(x), float(y), std::sin(float(x) * 0.7f) * std::cos(float(y) * 0.4f) * 2.0f);
    }
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = 0; x < columns; ++x) {
            if (hole != nullptr && hole(x, y))
                continue;
            uint32_t a = y * (columns + 1) + x;
            uint32_t b = a + 1;
            uint32_t c = a + columns + 1;
            uint32_t d = c + 1;
            float u0 = float(x) / float(columns);
            float u1 = float(x + 1) / float(columns);
            float v0 = float(y) / float(rows);
            float v1 = float(y + 1) / float(rows);
            float first[6] = {u0, v0, u1, v0, u0, v1};
            float second[6] = {u1, v0, u1, v1, u0, v1};
            addFace(&shape, a, b, c, first);
            addFace(&shape, b, d, c, second);
        }
    }
    return shape;
}

// A torus, which is closed and has a handle. Its texture coordinates run from 0 to 1 around each way, so they have a
// seam where they wrap. With `twist`, the last row joins the first turned over: a Klein bottle, which has no
// orientation.
Shape torus(const char* name, uint32_t around, uint32_t along, bool twist = false) {
    Shape shape = {name, {}, {}, {}};
    for (uint32_t j = 0; j < along; ++j) {
        for (uint32_t i = 0; i < around; ++i) {
            float u = float(i) / float(around) * 6.2831853f;
            float v = float(j) / float(along) * 6.2831853f;
            float r = 3.0f + std::cos(u);
            addVertex(&shape, r * std::cos(v), r * std::sin(v), std::sin(u));
        }
    }
    for (uint32_t j = 0; j < along; ++j) {
        for (uint32_t i = 0; i < around; ++i) {
            uint32_t i1 = (i + 1) % around;
            uint32_t j1 = (j + 1) % along;
            bool turned = twist && j + 1 == along;
            uint32_t a = j * around + i;
            uint32_t b = j * around + i1;
            uint32_t c = j1 * around + (turned ? (around - i) % around : i);
            uint32_t d = j1 * around + (turned ? (around - i1) % around : i1);
            float u0 = float(i) / float(around);
            float u1 = float(i + 1) / float(around);
            float v0 = float(j) / float(along);
            float v1 = float(j + 1) / float(along);
            float first[6] = {u0, v0, u1, v0, u0, v1};
            float second[6] = {u1, v0, u1, v1, u0, v1};
            addFace(&shape, a, b, c, first);
            addFace(&shape, b, d, c, second);
        }
    }
    return shape;
}

// A cube of twelve faces.
Shape cube(const char* name) {
    Shape shape = {name, {}, {}, {}};
    for (uint32_t i = 0; i < 8; ++i)
        addVertex(&shape, float(i & 1), float(i >> 1 & 1), float(i >> 2 & 1));
    static const uint32_t kQuads[6][4] = {
        {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5},
    };
    for (const uint32_t* quad : kQuads) {
        float first[6] = {0, 0, 1, 0, 1, 1};
        float second[6] = {0, 0, 1, 1, 0, 1};
        addFace(&shape, quad[0], quad[1], quad[2], first);
        addFace(&shape, quad[0], quad[2], quad[3], second);
    }
    return shape;
}

// A fan of faces around one vertex, which is closed for a cone and open otherwise.
Shape fan(const char* name, uint32_t count, bool closed) {
    Shape shape = {name, {}, {}, {}};
    addVertex(&shape, 0, 0, 1);
    for (uint32_t i = 0; i < count; ++i) {
        float angle = float(i) / float(count) * (closed ? 6.2831853f : 3.0f);
        addVertex(&shape, std::cos(angle), std::sin(angle), 0);
    }
    for (uint32_t i = 0; i + (closed ? 0 : 1) < count; ++i) {
        float uvs[6] = {0.5f, 0.5f, float(i) / float(count), 0, float(i + 1) / float(count), 0};
        addFace(&shape, 0, 1 + i, 1 + (i + 1) % count, uvs);
    }
    return shape;
}

// Faces between vertices chosen at random from a few, so that edges have any number of faces and some faces name a
// vertex twice.
Shape soup(const char* name, uint32_t vertices, uint32_t faces, Sequence* random) {
    Shape shape = {name, {}, {}, {}};
    for (uint32_t i = 0; i < vertices; ++i)
        addVertex(&shape, random->unit() * 4, random->unit() * 4, random->unit() * 4);
    for (uint32_t i = 0; i < faces; ++i) {
        float uvs[6];
        for (float& uv : uvs)
            uv = random->unit();
        addFace(&shape, random->next() % vertices, random->next() % vertices, random->next() % vertices, uvs);
    }
    return shape;
}

// Shapes that the encoder cannot code as a surface and has to cut: three faces at one edge, two fans that touch at one
// vertex, faces with no area, vertices at the same position, and a face that is repeated.
Shape nonManifold(const char* name) {
    Shape shape = {name, {}, {}, {}};
    static const float kPoints[][3] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {2, 1, 0}, {2, -1, 0}, {3, 0, 0}, {1, 0, 0}, {5, 5, 5},
        {6, 5, 5}, {5, 6, 5},
    };
    for (const float* point : kPoints)
        addVertex(&shape, point[0], point[1], point[2]);
    static const uint32_t kFaces[][3] = {
        {0, 1, 2}, {1, 0, 3}, {0, 1, 4},        // three faces at the edge 0-1
        {1, 5, 6}, {5, 7, 6},                   // a fan that meets the others at vertex 1 alone
        {2, 2, 4}, {0, 4, 4},                   // faces that name a vertex twice
        {0, 8, 2},                              // a face at a second vertex in the place of vertex 1
        {9, 10, 11}, {9, 10, 11}, {11, 10, 9},  // a face three times, once turned over
    };
    for (const uint32_t* face : kFaces) {
        float uvs[6] = {0, 0, 1, 0, 0, 1};
        addFace(&shape, face[0], face[1], face[2], uvs);
    }
    return shape;
}

// Joins shapes into one mesh of several components.
Shape join(const char* name, const std::vector<Shape>& parts) {
    Shape shape = {name, {}, {}, {}};
    for (const Shape& part : parts) {
        uint32_t base = uint32_t(shape.positions.size() / 3);
        float shift = float(base);
        for (size_t i = 0; i < part.positions.size(); ++i)
            shape.positions.push_back(part.positions[i] + (i % 3 == 0 ? shift : 0));
        for (uint32_t index : part.faces)
            shape.faces.push_back(base + index);
        shape.cornerUvs.insert(shape.cornerUvs.end(), part.cornerUvs.begin(), part.cornerUvs.end());
    }
    return shape;
}

// The ids of a built mesh's attributes, or -1.
struct Built {
    std::unique_ptr<draco::Mesh> mesh;
    int position = -1;
    int normal = -1;
    int texCoord = -1;
    int color = -1;
    int joints = -1;
    int weights = -1;
    int scalar = -1;
};

Built build(const Shape& shape, uint32_t with) {
    uint32_t faceCount = uint32_t(shape.faces.size() / 3);
    uint32_t vertexCount = uint32_t(shape.positions.size() / 3);
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(int(faceCount));
    Built built;
    built.position = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    if ((with & (With_Normals | With_HardNormals)) != 0)
        built.normal = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    if ((with & With_TexCoords) != 0)
        built.texCoord = builder.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
    if ((with & With_Colors) != 0)
        built.color = builder.AddAttribute(draco::GeometryAttribute::COLOR, 4, draco::DT_UINT8, true);
    if ((with & With_Joints) != 0)
        built.joints = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 4, draco::DT_UINT16);
    if ((with & With_Weights) != 0)
        built.weights = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 4, draco::DT_FLOAT32);
    if ((with & With_Scalar) != 0)
        built.scalar = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 1, draco::DT_INT32);

    // A vertex's normal is the sum of the normals of its faces, or else points away from the origin.
    std::vector<float> normals(vertexCount * 3, 0.0f);
    std::vector<float> faceNormals(faceCount * 3, 0.0f);
    for (uint32_t f = 0; f < faceCount; ++f) {
        const float* p[3];
        for (uint32_t k = 0; k < 3; ++k)
            p[k] = &shape.positions[shape.faces[f * 3 + k] * 3];
        float a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        float b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (uint32_t k = 0; k < 3; ++k) {
            faceNormals[f * 3 + k] = length > 0 ? n[k] / length : (k == 2 ? 1.0f : 0.0f);
            for (uint32_t c = 0; c < 3; ++c)
                normals[shape.faces[f * 3 + c] * 3 + k] += n[k];
        }
    }
    for (uint32_t v = 0; v < vertexCount; ++v) {
        float* n = &normals[v * 3];
        float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (uint32_t k = 0; k < 3; ++k)
            n[k] = length > 0 ? n[k] / length : (k == 0 ? 1.0f : 0.0f);
    }

    for (uint32_t f = 0; f < faceCount; ++f) {
        draco::FaceIndex face(f);
        const uint32_t* v = &shape.faces[f * 3];
        builder.SetAttributeValuesForFace(built.position, face, &shape.positions[v[0] * 3], &shape.positions[v[1] * 3],
            &shape.positions[v[2] * 3]);
        if ((with & With_HardNormals) != 0) {
            const float* n = &faceNormals[f * 3];
            builder.SetAttributeValuesForFace(built.normal, face, n, n, n);
        } else if (built.normal >= 0) {
            builder.SetAttributeValuesForFace(built.normal, face, &normals[v[0] * 3], &normals[v[1] * 3], &normals[v[2] * 3]);
        }
        if (built.texCoord >= 0) {
            const float* uv = &shape.cornerUvs[f * 6];
            builder.SetAttributeValuesForFace(built.texCoord, face, uv, uv + 2, uv + 4);
        }
        uint8_t colors[3][4];
        uint16_t joints[3][4];
        float weights[3][4];
        int32_t scalars[3];
        for (uint32_t c = 0; c < 3; ++c) {
            for (uint32_t k = 0; k < 4; ++k) {
                colors[c][k] = uint8_t(v[c] * (k + 3) * 17);
                joints[c][k] = uint16_t((v[c] / 3 + k * 7) % 300);
                weights[c][k] = float((v[c] + k) % 5) * 0.25f;
            }
            scalars[c] = int32_t(v[c] % 7) * 1000 - 3000;
        }
        if (built.color >= 0)
            builder.SetAttributeValuesForFace(built.color, face, colors[0], colors[1], colors[2]);
        if (built.joints >= 0)
            builder.SetAttributeValuesForFace(built.joints, face, joints[0], joints[1], joints[2]);
        if (built.weights >= 0)
            builder.SetAttributeValuesForFace(built.weights, face, weights[0], weights[1], weights[2]);
        if (built.scalar >= 0)
            builder.SetAttributeValuesForFace(built.scalar, face, &scalars[0], &scalars[1], &scalars[2]);
    }
    built.mesh = builder.Finalize();
    return built;
}

// What the comparison has seen, for the test that reports it.
struct Totals {
    uint32_t streams = 0;
    uint32_t points = 0;
    uint32_t faces = 0;
    uint32_t attributes = 0;
};

Totals gTotals;

// Decodes a stream with the reference decoder and with lib/draco, and compares everything that they give.
void compare(const char* data, size_t size, const char* what) {
    INFO(what);
    draco::DecoderBuffer buffer;
    buffer.Init(data, size);
    draco::Decoder decoder;
    bool isMesh = draco::Decoder::GetEncodedGeometryType(&buffer).value() == draco::TRIANGULAR_MESH;
    std::unique_ptr<draco::PointCloud> expected;
    const draco::Mesh* mesh = nullptr;
    if (isMesh) {
        draco::StatusOr<std::unique_ptr<draco::Mesh>> decoded = decoder.DecodeMeshFromBuffer(&buffer);
        REQUIRE(decoded.ok());
        mesh = decoded.value().get();
        expected = std::move(decoded).value();
    } else {
        draco::StatusOr<std::unique_ptr<draco::PointCloud>> decoded = decoder.DecodePointCloudFromBuffer(&buffer);
        REQUIRE(decoded.ok());
        expected = std::move(decoded).value();
    }

    PrDracoLimits limits = {1u << 26, 1u << 26};
    PrDraco* draco = nullptr;
    PrStatus status = prDracoCreate(PrByteView{reinterpret_cast<const uint8_t*>(data), size}, &limits, &draco);
    REQUIRE(status == PrStatus_Ok);
    REQUIRE(prDracoGeometry(draco) == (isMesh ? PrDracoGeometry_TriangleMesh : PrDracoGeometry_PointCloud));
    REQUIRE(prDracoPointCount(draco) == expected->num_points());
    uint32_t faceCount = mesh != nullptr ? uint32_t(mesh->num_faces()) : 0;
    REQUIRE(prDracoFaceCount(draco) == faceCount);
    const uint32_t* faces = prDracoFaces(draco);
    uint32_t wrongFaces = 0;
    for (uint32_t f = 0; f < faceCount; ++f) {
        const draco::Mesh::Face& face = mesh->face(draco::FaceIndex(f));
        for (uint32_t c = 0; c < 3; ++c)
            wrongFaces += faces[f * 3 + c] != face[c].value() ? 1 : 0;
    }
    CHECK(wrongFaces == 0);

    REQUIRE(prDracoAttributeCount(draco) == uint32_t(expected->num_attributes()));
    for (uint32_t a = 0; a < prDracoAttributeCount(draco); ++a) {
        const draco::PointAttribute* attribute = expected->attribute(int(a));
        const PrDracoAttribute& info = prDracoAttributes(draco)[a];
        CHECK(info.id == attribute->unique_id());
        CHECK(int(info.kind) == int(attribute->attribute_type()));
        CHECK(int(info.componentType) + 1 == int(attribute->data_type()));
        CHECK(info.componentCount == attribute->num_components());
        CHECK(info.normalized == attribute->normalized());
        size_t elementSize = size_t(attribute->byte_stride());
        PrByteView values = prDracoAttributeValues(draco, a);
        REQUIRE(values.size == elementSize * expected->num_points());
        std::vector<uint8_t> value(elementSize);
        uint32_t wrongValues = 0;
        for (uint32_t p = 0; p < expected->num_points(); ++p) {
            attribute->GetMappedValue(draco::PointIndex(p), value.data());
            wrongValues += std::memcmp(value.data(), values.data + p * elementSize, elementSize) != 0 ? 1 : 0;
        }
        CAPTURE(a);
        CHECK(wrongValues == 0);
    }
    gTotals.streams += 1;
    gTotals.points += prDracoPointCount(draco);
    gTotals.faces += faceCount;
    gTotals.attributes += prDracoAttributeCount(draco);
    prDracoDestroy(draco);
}

// How a mesh is encoded.
struct Encoding {
    int method = draco::MESH_EDGEBREAKER_ENCODING;
    int traversal = -1;                 // the Edgebreaker traversal, or -1 for the encoder's choice
    int speed = 5;
    bool compressConnectivity = false;  // of the sequential method
    bool builtIn = true;                // false stores every attribute whole
    int positionBits = 11;
    int normalBits = 8;
    int texCoordBits = 10;
    int weightBits = 12;
    int positionScheme = -1;            // a prediction scheme to force, or -1
    int normalScheme = -1;
    int texCoordScheme = -1;
    int genericScheme = -1;
};

// The Edgebreaker encoder with the predictive traversal, which the reference encoder never chooses.
class PredictiveEncoder : public draco::MeshEdgebreakerEncoder {
protected:
    bool InitializeEncoder() override {
        buffer()->Encode(static_cast<uint8_t>(draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING));
        impl_ = std::unique_ptr<draco::MeshEdgebreakerEncoderImplInterface>(
            new draco::MeshEdgebreakerEncoderImpl<draco::MeshEdgebreakerTraversalPredictiveEncoder>());
        return impl_->Init(this);
    }
};

// Encodes a built mesh. Returns false if the encoder refuses the combination, as it does a prediction scheme that the
// attribute cannot have.
bool encode(const Built& built, const Encoding& encoding, draco::EncoderBuffer* out) {
    struct Setting {
        int attribute;
        int bits;
        int scheme;
    };
    const Setting settings[] = {
        {built.position, encoding.positionBits, encoding.positionScheme},
        {built.normal, encoding.normalBits, encoding.normalScheme},
        {built.texCoord, encoding.texCoordBits, encoding.texCoordScheme},
        {built.weights, encoding.weightBits, encoding.genericScheme},
        {built.color, 0, encoding.genericScheme},
        {built.joints, 0, encoding.genericScheme},
        {built.scalar, 0, encoding.genericScheme},
    };
    draco::ExpertEncoder encoder(*built.mesh);
    draco::EncoderOptions options = draco::EncoderOptions::CreateDefaultOptions();
    options.SetGlobalBool("compress_connectivity", encoding.compressConnectivity);
    // The encoder refuses to be asked for the multi-parallelogram scheme, which the bitstream has withdrawn, and still
    // encodes with it if its options name it.
    for (const Setting& setting : settings) {
        if (setting.attribute >= 0 && setting.scheme == draco::MESH_PREDICTION_MULTI_PARALLELOGRAM)
            options.SetAttributeInt(setting.attribute, "prediction_scheme", setting.scheme);
    }
    encoder.Reset(options);
    encoder.SetSpeedOptions(encoding.speed, encoding.speed);
    encoder.SetEncodingMethod(encoding.method);
    if (encoding.traversal >= 0 && encoding.traversal != draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING)
        encoder.SetEncodingSubmethod(encoding.traversal);
    encoder.SetUseBuiltInAttributeCompression(encoding.builtIn);
    for (const Setting& setting : settings) {
        if (setting.attribute < 0)
            continue;
        if (setting.bits > 0)
            encoder.SetAttributeQuantization(setting.attribute, setting.bits);
        if (setting.scheme != -1 && setting.scheme != draco::MESH_PREDICTION_MULTI_PARALLELOGRAM &&
                !encoder.SetAttributePredictionScheme(setting.attribute, setting.scheme).ok()) {
            return false;
        }
    }
    if (encoding.traversal == draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING) {
        PredictiveEncoder predictive;
        predictive.SetMesh(*built.mesh);
        draco::EncoderOptions used = encoder.options();
        return predictive.Encode(used, out).ok();
    }
    return encoder.EncodeToBuffer(out).ok();
}

void encodeAndCompare(const Built& built, const Encoding& encoding, const char* what) {
    draco::EncoderBuffer buffer;
    if (encode(built, encoding, &buffer))
        compare(buffer.data(), buffer.size(), what);
}

bool gridHole(uint32_t x, uint32_t y) {
    return (x % 5 == 2 && y % 4 == 1) || (x == 0 && y == 0);
}

std::vector<Shape> shapes() {
    Sequence random;
    std::vector<Shape> all;
    all.push_back(grid("one cell", 1, 1));
    all.push_back(grid("sheet", 9, 7));
    all.push_back(grid("sheet with holes", 12, 9, gridHole));
    all.push_back(grid("large sheet", 40, 30));
    all.push_back(torus("torus", 12, 16));
    all.push_back(torus("klein bottle", 8, 10, true));
    all.push_back(cube("cube"));
    all.push_back(fan("cone", 9, true));
    all.push_back(fan("fan", 7, false));
    all.push_back(soup("soup", 30, 80, &random));
    all.push_back(soup("dense soup", 6, 60, &random));
    all.push_back(nonManifold("non-manifold"));
    all.push_back(join("components", {torus("", 5, 6), grid("", 3, 2), cube(""), fan("", 5, true), grid("", 1, 1)}));
    return all;
}

} // namespace

TEST_CASE("every speed of each method decodes as the reference decodes it") {
    for (const Shape& shape : shapes()) {
        Built built = build(shape, With_All);
        for (int speed = 0; speed <= 10; ++speed) {
            Encoding encoding;
            encoding.speed = speed;
            for (int traversal : {draco::MESH_EDGEBREAKER_STANDARD_ENCODING,
                draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING, draco::MESH_EDGEBREAKER_VALENCE_ENCODING}) {
                encoding.traversal = traversal;
                encodeAndCompare(built, encoding, shape.name);
            }
            encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
            encoding.traversal = -1;
            encodeAndCompare(built, encoding, shape.name);
            encoding.compressConnectivity = true;
            encodeAndCompare(built, encoding, shape.name);
        }
    }
}

TEST_CASE("each set of attributes decodes as the reference decodes it") {
    const uint32_t sets[] = {
        0, With_Normals, With_TexCoords, With_Colors, With_Joints, With_Weights, With_Scalar, With_HardNormals,
        With_Normals | With_TexCoords, With_HardNormals | With_TexCoords | With_Colors,
        With_TexCoords | With_Joints | With_Weights, With_All, With_All | With_HardNormals,
    };
    for (const Shape& shape : shapes()) {
        for (uint32_t with : sets) {
            Built built = build(shape, with);
            for (int speed : {0, 3, 7}) {
                Encoding encoding;
                encoding.speed = speed;
                encodeAndCompare(built, encoding, shape.name);
                encoding.traversal = draco::MESH_EDGEBREAKER_VALENCE_ENCODING;
                encodeAndCompare(built, encoding, shape.name);
                encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
                encodeAndCompare(built, encoding, shape.name);
            }
        }
    }
}

TEST_CASE("each prediction scheme decodes as the reference decodes it") {
    const int schemes[] = {
        draco::PREDICTION_NONE, draco::PREDICTION_DIFFERENCE, draco::MESH_PREDICTION_PARALLELOGRAM,
        draco::MESH_PREDICTION_MULTI_PARALLELOGRAM, draco::MESH_PREDICTION_CONSTRAINED_MULTI_PARALLELOGRAM,
        draco::MESH_PREDICTION_TEX_COORDS_PORTABLE, draco::MESH_PREDICTION_GEOMETRIC_NORMAL,
    };
    for (const Shape& shape : shapes()) {
        for (uint32_t with : {uint32_t(With_All), With_All | With_HardNormals}) {
            Built built = build(shape, with);
            for (int scheme : schemes) {
                for (int speed : {0, 5}) {
                    // One kind of attribute at a time takes the scheme, and then all of them.
                    for (uint32_t target = 0; target < 5; ++target) {
                        Encoding encoding;
                        encoding.speed = speed;
                        encoding.positionScheme = target == 0 || target == 4 ? scheme : -1;
                        encoding.normalScheme = target == 1 || target == 4 ? scheme : -1;
                        encoding.texCoordScheme = target == 2 || target == 4 ? scheme : -1;
                        encoding.genericScheme = target == 3 || target == 4 ? scheme : -1;
                        encodeAndCompare(built, encoding, shape.name);
                        encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
                        encodeAndCompare(built, encoding, shape.name);
                    }
                }
            }
        }
    }
}

TEST_CASE("each quantization decodes as the reference decodes it") {
    std::vector<Shape> all = shapes();
    for (const Shape& shape : all) {
        Built built = build(shape, With_All);
        for (int bits : {1, 2, 5, 8, 11, 14, 16, 20, 24, 30}) {
            for (int speed : {0, 5, 10}) {
                // The reference encoder keeps tables of the size of the integers, which it overruns past 21 bits when
                // it chooses among parallelograms, so the widest integers are coded as differences, and the very widest
                // for one shape.
                if ((bits > 16 && speed != 10) || (bits > 24 && &shape != &all[1]))
                    continue;
                Encoding encoding;
                encoding.speed = speed;
                encoding.positionBits = bits;
                encoding.normalBits = bits < 2 ? 2 : bits;
                encoding.texCoordBits = bits;
                encoding.weightBits = bits;
                encodeAndCompare(built, encoding, shape.name);
                encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
                encodeAndCompare(built, encoding, shape.name);
            }
        }
        // Without quantization a float is stored whole, and without the built-in compression every attribute is.
        Encoding whole;
        whole.positionBits = whole.normalBits = whole.texCoordBits = whole.weightBits = 0;
        encodeAndCompare(built, whole, shape.name);
        whole.builtIn = false;
        encodeAndCompare(built, whole, shape.name);
        whole.method = draco::MESH_SEQUENTIAL_ENCODING;
        encodeAndCompare(built, whole, shape.name);
    }
}

TEST_CASE("a mesh of many faces decodes as the reference decodes it") {
    // Enough faces that the encoder chooses the valence traversal and its symbols need the larger alphabets.
    Shape shape = grid("great sheet", 150, 120, gridHole);
    for (uint32_t with : {uint32_t(With_All), uint32_t(With_HardNormals | With_TexCoords)}) {
        Built built = build(shape, with);
        for (int speed : {0, 1, 4, 5, 9, 10}) {
            Encoding encoding;
            encoding.speed = speed;
            encoding.positionBits = 14;
            encodeAndCompare(built, encoding, shape.name);
            encoding.traversal = draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING;
            encodeAndCompare(built, encoding, shape.name);
        }
    }
    // With a point for each corner, the indices of the sequential method need more than 16 bits.
    for (uint32_t with : {uint32_t(With_All), uint32_t(With_HardNormals | With_TexCoords)}) {
        Encoding sequential;
        sequential.method = draco::MESH_SEQUENTIAL_ENCODING;
        Built built = build(shape, with);
        encodeAndCompare(built, sequential, shape.name);
        sequential.compressConnectivity = true;
        encodeAndCompare(built, sequential, shape.name);
    }
}

TEST_CASE("a stream with metadata decodes as the reference decodes it") {
    Shape shape = grid("sheet", 5, 4);
    Built built = build(shape, With_Normals | With_TexCoords);
    std::unique_ptr<draco::GeometryMetadata> metadata = std::make_unique<draco::GeometryMetadata>();
    metadata->AddEntryString("name", "sheet");
    metadata->AddEntryInt("cells", 20);
    std::unique_ptr<draco::Metadata> child = std::make_unique<draco::Metadata>();
    child->AddEntryDouble("scale", 0.5);
    std::unique_ptr<draco::Metadata> grandchild = std::make_unique<draco::Metadata>();
    grandchild->AddEntryIntArray("levels", {1, 2, 3});
    child->AddSubMetadata("deeper", std::move(grandchild));
    metadata->AddSubMetadata("child", std::move(child));
    metadata->AddSubMetadata("empty", std::make_unique<draco::Metadata>());
    std::unique_ptr<draco::AttributeMetadata> ofNormals = std::make_unique<draco::AttributeMetadata>();
    ofNormals->set_att_unique_id(built.mesh->attribute(built.normal)->unique_id());
    ofNormals->AddEntryString("space", "object");
    metadata->AddAttributeMetadata(std::move(ofNormals));
    built.mesh->AddMetadata(std::move(metadata));
    Encoding encoding;
    encodeAndCompare(built, encoding, shape.name);
    encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
    encodeAndCompare(built, encoding, shape.name);
}

TEST_CASE("a point cloud that is coded in order decodes as the reference decodes it") {
    Sequence random;
    for (uint32_t count : {1u, 7u, 500u}) {
        draco::PointCloudBuilder builder;
        builder.Start(count);
        int position = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
        int color = builder.AddAttribute(draco::GeometryAttribute::COLOR, 3, draco::DT_UINT8);
        for (uint32_t i = 0; i < count; ++i) {
            float point[3] = {random.unit(), random.unit() * 10, random.unit() - 0.5f};
            uint8_t rgb[3] = {uint8_t(i), uint8_t(i * 3), uint8_t(random.next())};
            builder.SetAttributeValueForPoint(position, draco::PointIndex(i), point);
            builder.SetAttributeValueForPoint(color, draco::PointIndex(i), rgb);
        }
        std::unique_ptr<draco::PointCloud> cloud = builder.Finalize(false);
        for (int bits : {0, 11}) {
            draco::ExpertEncoder encoder(*cloud);
            encoder.SetEncodingMethod(draco::POINT_CLOUD_SEQUENTIAL_ENCODING);
            if (bits != 0)
                encoder.SetAttributeQuantization(position, bits);
            draco::EncoderBuffer buffer;
            REQUIRE(encoder.EncodeToBuffer(&buffer).ok());
            compare(buffer.data(), buffer.size(), "point cloud");
        }
    }
}

TEST_CASE("the comparison saw streams of every kind") {
    MESSAGE(gTotals.streams << " streams, " << gTotals.faces << " faces, " << gTotals.points << " points");
    CHECK(gTotals.streams > 4000);
}

TEST_CASE("a stream that is cut short or changed fails or decodes, and reads nothing outside itself") {
    Sequence random;
    std::vector<Shape> all = {grid("sheet", 4, 3, gridHole), torus("torus", 4, 5), nonManifold("non-manifold")};
    uint32_t decoded = 0;
    uint32_t refused = 0;
    for (const Shape& shape : all) {
        Built built = build(shape, With_All | With_HardNormals);
        for (int variant = 0; variant < 5; ++variant) {
            Encoding encoding;
            encoding.speed = variant == 1 ? 10 : 0;
            if (variant == 2)
                encoding.traversal = draco::MESH_EDGEBREAKER_VALENCE_ENCODING;
            else if (variant == 3)
                encoding.traversal = draco::MESH_EDGEBREAKER_PREDICTIVE_ENCODING;
            if (variant == 4) {
                encoding.method = draco::MESH_SEQUENTIAL_ENCODING;
                encoding.compressConnectivity = true;
            }
            draco::EncoderBuffer buffer;
            REQUIRE(encode(built, encoding, &buffer));
            // A copy of exactly the stream's size, so that a tool that watches memory sees a read past its end.
            auto attempt = [&](const std::vector<uint8_t>& bytes) {
                std::unique_ptr<uint8_t[]> exact(new uint8_t[bytes.size() + 1]);
                std::memcpy(exact.get(), bytes.data(), bytes.size());
                PrDraco* draco = nullptr;
                PrStatus status = prDracoCreate(PrByteView{exact.get(), bytes.size()}, nullptr, &draco);
                bool known = status == PrStatus_Ok || status == PrStatus_InvalidData ||
                    status == PrStatus_NotSupported || status == PrStatus_Limit;
                CHECK(known);
                CHECK((draco != nullptr) == (status == PrStatus_Ok));
                if (draco != nullptr) {
                    // Every face names a point.
                    uint32_t outside = 0;
                    for (uint32_t i = 0; i < prDracoFaceCount(draco) * 3; ++i)
                        outside += prDracoFaces(draco)[i] >= prDracoPointCount(draco) ? 1 : 0;
                    CHECK(outside == 0);
                }
                (status == PrStatus_Ok ? decoded : refused) += 1;
                prDracoDestroy(draco);
            };
            std::vector<uint8_t> stream(buffer.data(), buffer.data() + buffer.size());
            for (size_t length = 0; length < stream.size(); ++length)
                attempt(std::vector<uint8_t>(stream.begin(), stream.begin() + long(length)));
            for (size_t at = 0; at < stream.size(); ++at) {
                for (uint8_t change : {uint8_t(0x01), uint8_t(0x80), uint8_t(0xff), uint8_t(random.next() | 1)}) {
                    std::vector<uint8_t> changed = stream;
                    changed[at] ^= change;
                    attempt(changed);
                }
            }
        }
    }
    MESSAGE("changed streams: " << decoded << " decoded, " << refused << " refused");
    CHECK(refused > 0);
}
