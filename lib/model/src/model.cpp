// Models: an asset of a scene format made ready to draw with a renderer, through a source that describes it.

#include "pr/model.h"
#include "model/src/model.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/blob.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mime_sniff.h"
#include "pr/raw_string.h"
#include "pr/render_shaders.h"
#include "pr/search.h"

#include "model/src/model_bind.h"
#include "model/src/model_mesh.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Arithmetic

template <typename T>
T* allocArray(uint32_t count) {
    return static_cast<T*>(prMemoryAllocZeroed(size_t(count != 0 ? count : 1) * sizeof(T)));
}

constexpr float kPi = 3.14159265358979323846f;

// Returns the unit vector along `v`, or `fallback` if `v` has no direction.
PrVec3 unitOr(PrVec3 v, PrVec3 fallback) {
    float length2 = prVec3Dot(v, v);
    if (!prF32IsFinite(length2) || length2 < 1e-20f)
        return fallback;
    return v / prF32Sqrt(length2);
}

// Return a column of a matrix's upper three rows: an axis of the space that it maps from, or that space's origin.
PrVec3 columnOf(const PrMatrix4& matrix, uint32_t column) {
    return PrVec3{matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]};
}

// Writes to `out` the unit quaternion at `t` of the shorter arc from `a` to `b`.
void slerp(const float* a, const float* b, float t, float* out) {
    float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sign = cosine < 0 ? -1.0f : 1.0f;
    cosine *= sign;
    float fromWeight = 1 - t;
    float toWeight = t;
    // Quaternions that nearly coincide mix linearly, where the arc's formula divides by a sine near zero.
    if (cosine < 0.9995f) {
        float angle = prF32Acos(cosine);
        float sine = prF32Sin(angle);
        fromWeight = prF32Sin((1 - t) * angle) / sine;
        toWeight = prF32Sin(t * angle) / sine;
    }
    float length2 = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = a[i] * fromWeight + b[i] * (toWeight * sign);
        length2 += out[i] * out[i];
    }
    float scale = length2 > 0 ? 1 / prF32Sqrt(length2) : 0;
    for (uint32_t i = 0; i < 4; ++i)
        out[i] *= scale;
    if (length2 <= 0)
        out[3] = 1;
}

// Scales a quaternion to unit length, or makes it the identity if it has none.
void normalizeQuaternion(float* q) {
    float length2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(length2 > 0) || !prF32IsFinite(length2)) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    float scale = 1 / prF32Sqrt(length2);
    for (uint32_t i = 0; i < 4; ++i)
        q[i] *= scale;
}

// Returns the matrix of a translation, a rotation and a scale: the scale first, then the rotation, then the
// translation.
PrMatrix4 matrixFromTrs(const float* t, const float* r, const float* s) {
    float x = r[0], y = r[1], z = r[2], w = r[3];
    PrMatrix4 m{};
    m.m[0][0] = (1 - 2 * (y * y + z * z)) * s[0];
    m.m[0][1] = (2 * (x * y - z * w)) * s[1];
    m.m[0][2] = (2 * (x * z + y * w)) * s[2];
    m.m[1][0] = (2 * (x * y + z * w)) * s[0];
    m.m[1][1] = (1 - 2 * (x * x + z * z)) * s[1];
    m.m[1][2] = (2 * (y * z - x * w)) * s[2];
    m.m[2][0] = (2 * (x * z - y * w)) * s[0];
    m.m[2][1] = (2 * (y * z + x * w)) * s[1];
    m.m[2][2] = (1 - 2 * (x * x + y * y)) * s[2];
    m.m[0][3] = t[0];
    m.m[1][3] = t[1];
    m.m[2][3] = t[2];
    m.m[3][3] = 1;
    return m;
}

// Returns the matrix that maps a node to its parent's space, `local`, and then the parent's to the world.
PrMatrix4 worldOf(const PrMatrix4& local, const PrMatrix4& parent) {
    return prMatrix4Concat(&local, &parent);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tracks

// Writes to `out` the value of a track at `seconds`: held, interpolated linearly, along the shorter arc for a rotation,
// or along the cubic spline of its keys' tangents. A rotation is a unit quaternion in every case.
void evaluateTrack(const PrModelSourceInfo& info, const PrModelSourceTrack& track, float seconds, float* out) {
    const float* times = info.keyTimes + track.firstKey;
    const float* values = info.keyValues + track.firstValue;
    uint32_t count = track.keyCount;
    uint32_t width = track.width;
    bool cubic = track.interpolation == PrModelInterpolation_CubicSpline;
    uint32_t stride = cubic ? width * 3 : width;
    uint32_t at = cubic ? width : 0;
    bool rotation = track.target == PrModelTrackTarget_Rotation;
    if (count == 0)
        return;
    if (!(seconds > times[0]) || count == 1) {
        prMemCopy(out, values + at, width * sizeof(float));
    } else if (seconds >= times[count - 1]) {
        prMemCopy(out, values + size_t(count - 1) * stride + at, width * sizeof(float));
    } else {
        uint32_t next = upperBound(Span<const float>{times, count}, seconds);
        uint32_t key = next - 1;
        float span = times[next] - times[key];
        float u = span > 0 ? (seconds - times[key]) / span : 0;
        const float* a = values + size_t(key) * stride;
        const float* b = values + size_t(next) * stride;
        if (track.interpolation == PrModelInterpolation_Step) {
            prMemCopy(out, a + at, width * sizeof(float));
        } else if (!cubic && rotation && width == 4) {
            slerp(a, b, u, out);
        } else if (!cubic) {
            for (uint32_t k = 0; k < width; ++k)
                out[k] = a[k] + (b[k] - a[k]) * u;
        } else {
            // The Hermite spline of the keys' values and of their tangents, which are per second.
            float u2 = u * u;
            float u3 = u2 * u;
            for (uint32_t k = 0; k < width; ++k) {
                float p0 = a[width + k];
                float m0 = a[2 * width + k] * span;
                float p1 = b[width + k];
                float m1 = b[k] * span;
                out[k] = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
            }
        }
    }
    if (rotation && width == 4)
        normalizeQuaternion(out);
}

// Returns true if a track sets a property of a material.
bool ofMaterial(PrModelTrackTarget target) {
    return target >= PrModelTrackTarget_Input && target <= PrModelTrackTarget_TextureScale;
}

// ---------------------------------------------------------------------------------------------------------------------
// The description

void setLoad(ModelLoadState* state, PrModelLoad load, PrStatus status) {
    state->status.store(status);
    state->load.store(load);
}

// Records how a mesh's job loaded.
void finishLoad(ModelLoadState* state, PrStatus status) {
    setLoad(state, status == PrStatus_Ok ? PrModelLoad_Ready : PrModelLoad_Failed, status);
}

PrModelResourceInfo infoOf(const ModelLoadState& state) {
    return PrModelResourceInfo{.load = state.load.load(), .status = state.status.load()};
}

// Reads, for each mesh that a draw has a skin for, the box of the positions that each of its joints moves. The bounds
// of a placement in a pose follow from them.
void readJointBoxes(Model* m) {
    for (uint32_t i = 0; i < m->info->meshCount; ++i) {
        ModelMesh& mesh = m->meshes[i];
        if (mesh.jointCount == 0)
            continue;
        mesh.jointBoxes = allocArray<PrBox3>(mesh.jointCount);
        for (uint32_t j = 0; j < mesh.jointCount; ++j)
            mesh.jointBoxes[j] = PrBox3{{1, 1, 1}, {-1, -1, -1}};
        PrModelMeshRequest request{.skin = true};
        PrModelMeshData data{};
        mesh.jointBoxesRead = prModelSourceMeshRead(m->source, i, &request, &data) == PrStatus_Ok && data.positions != nullptr;
        for (uint32_t v = 0; mesh.jointBoxesRead && data.joints != nullptr && v < data.vertexCount; ++v) {
            const float* at = data.positions + size_t(v) * 3;
            PrVec3 point{at[0], at[1], at[2]};
            // Every influence counts, though a vertex takes only the four of the greatest weight.
            for (uint32_t k = 0; k < data.influenceCount; ++k) {
                size_t influence = size_t(v) * data.influenceCount + k;
                uint32_t joint = data.joints[influence];
                if (!(data.weights[influence] > 0) || joint >= mesh.jointCount)
                    continue;
                PrBox3& box = mesh.jointBoxes[joint];
                if (box.min.x > box.max.x)
                    box = PrBox3{point, point};
                box = prBox3Include(box, point);
            }
        }
        prModelMeshDataReset(&data);
    }
}

// Reads the nodes of the model's scene: their order, the draws of the scene, and the nodes that carry a camera or a
// light, and what the animations change.
void readScene(Model* m) {
    const PrModelSourceInfo& info = *m->info;
    uint32_t nodeCount = info.nodeCount;
    // The nodes that a track moves, and the nodes and meshes that one gives weights.
    bool* targeted = allocArray<bool>(nodeCount);
    bool* weighted = allocArray<bool>(nodeCount);
    bool* meshWeighted = allocArray<bool>(info.meshCount);
    for (uint32_t i = 0; i < info.trackCount; ++i) {
        const PrModelSourceTrack& track = info.tracks[i];
        if (track.width > m->trackWidth)
            m->trackWidth = track.width;
        if (ofMaterial(track.target))
            m->animatesMaterials = true;
        bool node = track.object < nodeCount;
        bool moves = track.target <= PrModelTrackTarget_Matrix;
        if (moves && node)
            targeted[track.object] = true;
        if ((track.target == PrModelTrackTarget_Weights || track.target == PrModelTrackTarget_Weight) && node)
            weighted[track.object] = true;
        bool ofMesh = track.target == PrModelTrackTarget_MeshWeights || track.target == PrModelTrackTarget_MeshWeight;
        if (ofMesh && track.object < info.meshCount)
            meshWeighted[track.object] = true;
    }

    // The roots are pushed in reverse, so that the nodes are visited in the source's order.
    RawArray<PrModelNodeIndex> stack;
    for (uint32_t i = info.rootCount; i-- > 0;)
        stack.push(info.roots[i]);
    bool* animated = allocArray<bool>(nodeCount);
    while (stack.count != 0) {
        PrModelNodeIndex index = stack.last();
        stack.pop();
        if (index >= nodeCount || m->inScene[index])
            continue;
        const PrModelSourceNode& node = info.nodes[index];
        m->order.push(index);
        m->inScene[index] = true;
        animated[index] = targeted[index] || (node.parent != kPrModelNodeNone && animated[node.parent]);
        if (node.camera != kPrModelCameraNone)
            m->cameras.push(index);
        if (node.light != kPrModelLightNone)
            m->lights.push(index);
        for (uint32_t i = node.childCount; i-- > 0;)
            stack.push(info.children[node.firstChild + i]);
    }
    stack.reset();

    // The draws of the scene, at their nodes, and each instance's transform within its node.
    RawArray<PrMatrix4> locals;
    for (uint32_t i = 0; i < info.drawCount; ++i) {
        const PrModelSourceDraw& given = info.draws[i];
        if (given.node >= nodeCount || !m->inScene[given.node] || m->meshes[given.mesh].partCount == 0)
            continue;
        ModelMesh& mesh = m->meshes[given.mesh];
        // A skinned mesh follows its joints, which an animation may move, and not its node.
        bool deforms = given.skin != kPrModelSkinNone || weighted[given.node] || meshWeighted[given.mesh];
        uint32_t instances = given.instanceCount != 0 ? given.instanceCount : 1;
        m->draws.push(ModelDraw{
            .node = given.node,
            .mesh = given.mesh,
            .skin = given.skin,
            .animated = animated[given.node] || deforms,
            .firstWeight = m->weightCount,
            .weightCount = mesh.morphTargetCount,
            .firstInstance = m->instanceCount,
            .instanceCount = instances,
        });
        if (given.instanceCount != 0)
            locals.append(info.instances + given.firstInstance, given.instanceCount);
        else
            locals.push(prMatrix4Identity());
        m->instanceCount += instances;
        if (m->nodeDraws[given.node] == 0)
            m->nodeDraws[given.node] = m->draws.count;
        m->weightCount += mesh.morphTargetCount;
        if (given.skin != kPrModelSkinNone) {
            uint32_t joints = info.skins[given.skin].jointCount;
            mesh.jointCount = joints > mesh.jointCount ? joints : mesh.jointCount;
            m->jointCount = joints > m->jointCount ? joints : m->jointCount;
        }
    }
    m->instanceLocal = allocArray<PrMatrix4>(m->instanceCount);
    prMemCopy(m->instanceLocal, locals.data, size_t(locals.count) * sizeof(PrMatrix4));
    locals.reset();
    prMemoryFree(targeted);
    prMemoryFree(weighted);
    prMemoryFree(meshWeighted);
    prMemoryFree(animated);
}

void placementDescribe(ModelPlacement* p);
void enterScene(ModelPlacement* p);
void populateResources(Model* m);

// Reads the source's description into the model, which has none: its meshes, parts, images, materials and scene, and
// makes the arrays of its placements. A model in a renderer creates its resources in it.
void describe(Model* m) {
    const PrModelSourceInfo& info = *prModelSourceInfo(m->source);
    m->info = &info;
    for (uint32_t i = 0; i < info.diagnosticCount; ++i)
        modelDiagnose(m, info.diagnostics[i].feature, info.diagnostics[i].detail);
    m->parts = allocArray<ModelPart>(info.partCount);
    m->meshes = allocArray<ModelMesh>(info.meshCount);
    for (uint32_t i = 0; i < info.meshCount; ++i) {
        m->meshes[i].firstPart = info.meshes[i].firstPart;
        m->meshes[i].partCount = info.meshes[i].partCount;
        m->meshes[i].morphTargetCount = info.meshes[i].morphTargetCount;
    }
    m->images = allocArray<ModelImage>(info.imageCount);
    m->materials = allocArray<PrRenderMaterial>(info.materialCount);
    m->bindings = allocArray<ModelBinding>(info.materialCount);
    m->partMaterials = allocArray<PrRenderMaterial>(info.partCount);
    m->inScene = allocArray<bool>(info.nodeCount);
    m->nodeDraws = allocArray<uint32_t>(info.nodeCount);
    readScene(m);
    readJointBoxes(m);
    modelBindDescribe(m);
    for (uint32_t i = 0; i < info.meshCount; ++i)
        setLoad(&m->meshes[i].state, m->meshes[i].partCount != 0 ? PrModelLoad_Pending : PrModelLoad_None, PrStatus_Ok);
    for (uint32_t i = 0; i < info.imageCount; ++i)
        setLoad(&m->images[i].state, m->images[i].used ? PrModelLoad_Pending : PrModelLoad_None, PrStatus_Ok);
    m->described = true;
    setLoad(&m->sourceState, PrModelLoad_Ready, PrStatus_Ok);
    if (m->render != nullptr)
        populateResources(m);
    for (uint32_t i = 0; i < m->placements.count; ++i) {
        placementDescribe(m->placements.data[i]);
        enterScene(m->placements.data[i]);
    }
    m->revision.fetchAdd(1);
}

// Describes the model if its source has arrived since the last call.
void takeSource(Model* m) {
    if (m->described)
        return;
    prMutexLock(&m->lock);
    PrModelSource* opened = m->opened;
    m->opened = nullptr;
    prMutexUnlock(&m->lock);
    if (opened == nullptr)
        return;
    m->source = opened;
    describe(m);
}

// ---------------------------------------------------------------------------------------------------------------------
// The renderer's resources

// The plan of the attributes of a mesh's parts in the model's layouts: by part, layout, attribute and component, where
// each component comes from.
struct PartPlan {
    RawArray<ModelComponentSource> components;
};

// Writes the attributes of the prepared vertices of each part in each of the model's layouts to `bytes`, one stream for
// each layout, from the values of their streams as the part's plan maps them.
PrStatus fillStreams(const Model* m, const ModelMeshPrepared& prepared, const PartPlan& plan, RawArray<uint8_t>* bytes,
        RawArray<PrRenderMeshStream>* streams) {
    uint32_t count = prepared.vertexCount;
    size_t size = 0;
    uint32_t perPart = 0;
    for (uint32_t l = 0; l < m->layouts.count; ++l) {
        size += size_t(m->layouts.data[l]->stride) * count;
        perPart += m->layouts.data[l]->attributeCount * 4;
    }
    bytes->resize(uint32_t(size));
    RawArray<float> values;
    PrRenderAttributeSource sources[16];
    size_t offset = 0;
    uint32_t planAt = 0;
    for (uint32_t l = 0; l < m->layouts.count; ++l) {
        const PrRenderVertexLayout* layout = m->layouts.data[l];
        uint8_t* stream = bytes->data + offset;
        for (uint32_t k = 0; k < prepared.parts.count; ++k) {
            uint32_t first = prepared.partFirstVertex.data[k];
            uint32_t vertices = prepared.partVertexCount.data[k];
            const ModelComponentSource* components = plan.components.data + size_t(k) * perPart + planAt;
            uint32_t sourceCount = 0;
            values.resize(layout->attributeCount * 4 * (vertices != 0 ? vertices : 1));
            for (uint32_t a = 0; a < layout->attributeCount && a < 16; ++a) {
                const PrRenderVertexAttribute& attribute = layout->attributes[a];
                const ModelComponentSource* source = components + a * 4;
                bool any = false;
                for (uint32_t c = 0; c < 4; ++c)
                    any = any || source[c].stream != kModelStreamNone;
                if (!any)
                    continue;
                float* out = values.data + size_t(a) * 4 * vertices;
                bool color = attribute.semantic == PrRenderSemantic_Color;
                const float fallback[4] = {
                    attribute.fallback.x, attribute.fallback.y, attribute.fallback.z,
                    attribute.fallback.w,
                };
                for (uint32_t v = 0; v < vertices; ++v) {
                    const float* raw = reinterpret_cast<const float*>(prepared.vertices.data +
                        size_t(first + v) * prepared.stride + sizeof(PrRenderGeometryVertex));
                    for (uint32_t c = 0; c < 4; ++c) {
                        float value = fallback[c];
                        uint32_t s = source[c].stream;
                        bool present = s != kModelStreamNone && (prepared.partStreams.data[k] & (1u << s)) != 0;
                        if (present) {
                            const float* given = raw + prepared.streamOffsets.data[s];
                            uint32_t width = prepared.streamWidths.data[s];
                            value = source[c].offset;
                            for (uint32_t j = 0; j < 4; ++j) {
                                // A channel that the stream lacks is zero, and its alpha one.
                                float channel = j < width ? given[j] : (j == 3 ? 1.0f : 0.0f);
                                value += source[c].weights[j] * channel;
                            }
                        }
                        if (color)
                            value = prF32Min(prF32Max(value, 0), 1);
                        out[size_t(v) * 4 + c] = value;
                    }
                }
                sources[sourceCount++] = PrRenderAttributeSource{
                    attribute.semantic,
                    attribute.semanticIndex,
                    attribute.name,
                    {out, vertices, 4 * sizeof(float), prMeshFormatFloat32(4)},
                };
            }
            PrStatus status = prRenderVertexLayoutFill(layout, sourceCount != 0 ? sources : nullptr, sourceCount,
                vertices, stream + size_t(first) * layout->stride, nullptr);
            if (status != PrStatus_Ok) {
                values.reset();
                return status;
            }
        }
        planAt += layout->attributeCount * 4;
        streams->push(PrRenderMeshStream{layout, stream});
        offset += size_t(layout->stride) * count;
    }
    values.reset();
    return PrStatus_Ok;
}

// Reads and prepares a mesh of the source as `prepared`, with the plan of its parts' attributes in the model's layouts
// where `attributes` asks for them. `keepsOrder` keeps the order of every part's triangles.
PrStatus prepareMesh(const Model* m, PrModelMeshIndex index, bool keepsOrder, bool attributes,
        ModelMeshPrepared* prepared, PartPlan* plan) {
    const ModelMesh& mesh = m->meshes[index];
    ModelStreamNames names{};
    RawArray<ModelPartOptions> options;
    for (uint32_t k = 0; k < mesh.partCount; ++k) {
        PrModelPartIndex part = mesh.firstPart + k;
        uint32_t tangent = attributes ? modelBindTangentStream(m, part, &names) : kModelStreamNone;
        options.push(ModelPartOptions{
            .wantsTangents = attributes && m->parts[part].wantsTangents,
            .tangentStream = tangent,
            .keepsTriangleOrder = keepsOrder || m->parts[part].blends,
        });
        for (uint32_t l = 0; attributes && l < m->layouts.count; ++l) {
            const PrRenderVertexLayout* layout = m->layouts.data[l];
            for (uint32_t a = 0; a < layout->attributeCount; ++a) {
                ModelComponentSource components[4];
                modelBindAttribute(m, part, layout->attributes[a], &names, components);
                plan->components.append(components, 4);
            }
        }
    }
    PrModelMeshRequest request{
        .streams = names.names.data,
        .streamCount = names.names.count,
        .skin = mesh.jointCount != 0,
        .morphs = mesh.morphTargetCount != 0,
    };
    PrModelMeshData data{};
    PrStatus status = prModelSourceMeshRead(m->source, index, &request, &data);
    if (status == PrStatus_Ok && data.partCount != mesh.partCount)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        RawArray<uint32_t> widths;
        for (uint32_t i = 0; i < request.streamCount; ++i) {
            uint32_t width = data.streamWidths != nullptr ? data.streamWidths[i] : 0;
            widths.push(width < 4 ? width : 4);
        }
        modelMeshPreparedInit(prepared, widths.data, widths.count);
        widths.reset();
        ModelMeshOptions meshOptions{
            .parts = options.data,
            .skinned = mesh.jointCount != 0,
            .morphTargetCount = mesh.morphTargetCount,
        };
        status = modelMeshPrepare(data, meshOptions, prepared);
    }
    prModelMeshDataReset(&data);
    options.reset();
    names.names.reset();
    return status;
}

// Prepares a mesh of the source, and gives the renderer's mesh its data.
PrStatus loadMesh(Model* m, PrModelMeshIndex index) {
    const ModelMesh& loaded = m->meshes[index];
    ModelMeshPrepared prepared{};
    PartPlan plan{};
    PrStatus status = prepareMesh(m, index, false, true, &prepared, &plan);
    RawArray<PrRenderGeometryVertex> geometry;
    RawArray<uint8_t> bytes;
    RawArray<PrRenderMeshStream> streams;
    if (status == PrStatus_Ok && prepared.parts.count == 0)
        status = PrStatus_InvalidData;
    if (status == PrStatus_Ok) {
        geometry.resize(prepared.vertexCount);
        for (uint32_t v = 0; v < prepared.vertexCount; ++v)
            geometry.data[v] = *modelMeshVertex(prepared, v);
        status = fillStreams(m, prepared, plan, &bytes, &streams);
    }
    if (status == PrStatus_Ok) {
        PrRenderMeshData meshData{
            .geometry = geometry.data,
            .vertexCount = prepared.vertexCount,
            .streams = streams.data,
            .streamCount = streams.count,
            .indices = prepared.indices.data,
            .indexCount = prepared.indices.count,
            .parts = prepared.parts.data,
            .partCount = prepared.parts.count,
            .skin = loaded.jointCount != 0 ? prepared.skin.data : nullptr,
            .jointCount = loaded.jointCount,
            .morphs = loaded.morphTargetCount != 0 ? prepared.morphs.data : nullptr,
            .morphTargetCount = loaded.morphTargetCount,
        };
        status = prRenderMeshSetData(m->render, loaded.mesh, &meshData);
    } else if (status != PrStatus_Io && status != PrStatus_Limit) {
        status = PrStatus_InvalidData;
    }
    geometry.reset();
    bytes.reset();
    streams.reset();
    plan.components.reset();
    modelMeshPreparedReset(&prepared);
    return status;
}

// Runs a job: loads its resource into the model's renderer, and records how that went. An image's job finishes when its
// last step completes.
void runJob(ModelJob* job) {
    Model* m = job->model;
    if (job->kind == ModelJobKind_Image) {
        modelBindLoadImage(job);
        return;
    }
    if (!m->cancelled.load()) {
        finishLoad(&m->meshes[job->index].state, loadMesh(m, job->index));
        m->revision.fetchAdd(1);
    }
    modelJobFinish(m);
}

void postedJob(void* PR_NULLABLE context) {
    runJob(static_cast<ModelJob*>(context));
}

// Runs the jobs that are left without a pool, and waits for the jobs that a pool is running and for the steps that
// complete later.
void finishJobs(Model* m) {
    if (m->options.pool == nullptr) {
        while (m->jobsNext < m->jobs.count)
            runJob(&m->jobs.data[m->jobsNext++]);
    }
    prMutexLock(&m->lock);
    while (m->jobsPending != 0)
        prConditionWait(&m->idle, &m->lock);
    prMutexUnlock(&m->lock);
}

// Creates the renderer's resources of a described model in its renderer, and starts the jobs that load them.
void populateResources(Model* m) {
    PrRender* render = m->render;
    const PrModelSourceInfo& info = *m->info;
    modelBindPopulate(m);
    for (uint32_t i = 0; i < info.meshCount; ++i) {
        ModelMesh& mesh = m->meshes[i];
        if (mesh.partCount == 0)
            continue;
        mesh.mesh = prRenderMeshCreate(render);
        for (uint32_t k = 0; k < mesh.partCount; ++k) {
            PrModelMaterialIndex material = info.parts[mesh.firstPart + k].material;
            if (material != kPrModelMaterialNone) {
                m->partMaterials[mesh.firstPart + k] = m->materials[material];
                continue;
            }
            // A part without a material draws with the default material.
            if (m->defaultMaterial.id == 0) {
                m->defaultMaterial = prRenderMaterialCreate(render);
                modelBindMaterial(m, m->defaultMaterial, kPrModelMaterialNone, nullptr);
            }
            m->partMaterials[mesh.firstPart + k] = m->defaultMaterial;
        }
        // Meshes load before images, so that a model shows its shape first.
        m->jobs.push(ModelJob{m, ModelJobKind_Mesh, i});
    }
    for (uint32_t i = 0; i < info.imageCount; ++i) {
        if (m->images[i].used)
            m->jobs.push(ModelJob{m, ModelJobKind_Image, i});
    }
    prMutexLock(&m->lock);
    m->jobsPending += m->jobs.count;
    prMutexUnlock(&m->lock);
    for (uint32_t i = 0; m->options.pool != nullptr && i < m->jobs.count; ++i)
        prTaskPoolPost(m->options.pool, postedJob, &m->jobs.data[i]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Poses

// Returns `seconds` brought into an animation of `duration` seconds.
float wrapTime(float seconds, float duration, PrModelLoop loop) {
    if (!(duration > 0) || !prF32IsFinite(seconds))
        return 0;
    if (loop == PrModelLoop_Once)
        return prF32Clamp(seconds, 0, duration);
    float wrapped = prF32Fmod(seconds, duration);
    return wrapped < 0 ? wrapped + duration : wrapped;
}

float durationOf(const Model* m, PrModelAnimationIndex animation) {
    if (animation == kPrModelAnimationNone || !m->described)
        return 0;
    return m->info->animations[animation].duration;
}

// Writes to `weights` the morph weights that the source gives a draw: its node's, or else its mesh's, or else zeros.
void restWeights(const Model* m, const ModelDraw& draw, float* weights) {
    const PrModelSourceInfo& info = *m->info;
    const PrModelSourceNode& node = info.nodes[draw.node];
    const PrModelSourceMesh& mesh = info.meshes[draw.mesh];
    uint32_t first = node.weightCount != 0 ? node.firstWeight : mesh.firstWeight;
    uint32_t count = node.weightCount != 0 ? node.weightCount : mesh.weightCount;
    for (uint32_t t = 0; t < draw.weightCount; ++t)
        weights[t] = t < count ? info.weights[first + t] : 0;
}

// The state of a pose that an animation sets: by node, its transform and whether it is visible itself; the morph
// weights of every draw; by light and by camera of the source, the light and the camera; and the inputs of materials.
// `scratch` holds the value of any one track.
struct ModelPose {
    ModelTrs* trs;
    bool* visible;
    float* weights;
    PrModelSourceLight* lights;
    PrModelSourceCamera* PR_NULLABLE cameras;
    PrModelSourceInput* inputs;
    bool* materialPlayed;
    float* scratch;
};

// Applies the value of a track to a light of the source.
void setLight(PrModelSourceLight* light, PrModelTrackTarget target, const float* value) {
    switch (target) {
        case PrModelTrackTarget_LightColor:
            light->color = PrVec3{value[0], value[1], value[2]};
            break;
        case PrModelTrackTarget_LightIntensity:
            light->intensity = prF32Max(value[0], 0);
            break;
        case PrModelTrackTarget_LightRange:
            light->range = value[0] > 0 ? value[0] : light->range;
            break;
        case PrModelTrackTarget_LightInnerConeAngle:
            light->innerConeAngle = value[0];
            break;
        default:
            light->outerConeAngle = value[0];
            break;
    }
}

// Applies the value of a track to a camera of the source.
void setCamera(PrModelSourceCamera* camera, PrModelTrackTarget target, float value) {
    switch (target) {
        case PrModelTrackTarget_CameraYfov:
            camera->yfov = value;
            break;
        case PrModelTrackTarget_CameraAspectRatio:
            camera->aspectRatio = value;
            break;
        case PrModelTrackTarget_CameraXmag:
            camera->xmag = value;
            break;
        case PrModelTrackTarget_CameraYmag:
            camera->ymag = value;
            break;
        case PrModelTrackTarget_CameraZnear:
            camera->znear = value;
            break;
        default:
            camera->zfar = value;
            break;
    }
}

// Applies the value of a track to an input of a material.
void setInput(PrModelSourceInput* input, PrModelTrackTarget target, uint32_t width, const float* value) {
    switch (target) {
        case PrModelTrackTarget_TextureOffset:
            prMemCopy(input->offset, value, 2 * sizeof(float));
            break;
        case PrModelTrackTarget_TextureRotation:
            input->rotation = value[0];
            break;
        case PrModelTrackTarget_TextureScale:
            prMemCopy(input->scale, value, 2 * sizeof(float));
            break;
        default:
            prMemCopy(input->value, value, (width < 4 ? width : 4) * sizeof(float));
            break;
    }
}

// Writes weights of a track to the draws that they reach: a mesh's to every draw of the mesh whose node gives none of
// its own, and a node's to every draw of the node.
void setWeights(const Model* m, const PrModelSourceTrack& track, const float* value, float* weights) {
    const PrModelSourceInfo& info = *m->info;
    bool ofMesh = track.target == PrModelTrackTarget_MeshWeights || track.target == PrModelTrackTarget_MeshWeight;
    bool one = track.target == PrModelTrackTarget_MeshWeight || track.target == PrModelTrackTarget_Weight;
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelDraw& draw = m->draws.data[i];
        bool reaches = draw.node == track.object;
        if (ofMesh)
            reaches = draw.mesh == track.object && info.nodes[draw.node].weightCount == 0;
        if (!reaches)
            continue;
        if (one) {
            if (track.element < draw.weightCount)
                weights[draw.firstWeight + track.element] = value[0];
            continue;
        }
        for (uint32_t w = 0; w < draw.weightCount && w < track.width; ++w)
            weights[draw.firstWeight + w] = value[w];
    }
}

// Writes to `pose` what the source gives each node of the scene, each draw, each light, each camera and each animated
// input, and over it what an animation gives the properties that it has tracks for. The weights of a mesh apply first,
// so that those of a node replace them.
void evaluatePlayback(const Model* m, ModelPlayback playback, const ModelPose& pose) {
    const PrModelSourceInfo& info = *m->info;
    for (uint32_t i = 0; i < m->order.count; ++i) {
        PrModelNodeIndex index = m->order.data[i];
        const PrModelSourceNode& node = info.nodes[index];
        ModelTrs& trs = pose.trs[index];
        prMemCopy(trs.translation, node.translation, sizeof(trs.translation));
        prMemCopy(trs.rotation, node.rotation, sizeof(trs.rotation));
        prMemCopy(trs.scale, node.scale, sizeof(trs.scale));
        trs.hasMatrix = (node.flags & PrModelNodeFlag_Trs) == 0;
        trs.matrix = node.matrix;
        pose.visible[index] = (node.flags & PrModelNodeFlag_Hidden) == 0;
    }
    for (uint32_t i = 0; i < m->draws.count; ++i)
        restWeights(m, m->draws.data[i], pose.weights + m->draws.data[i].firstWeight);
    prMemCopy(pose.lights, info.lights, size_t(info.lightCount) * sizeof(PrModelSourceLight));
    if (pose.cameras != nullptr)
        prMemCopy(pose.cameras, info.cameras, size_t(info.cameraCount) * sizeof(PrModelSourceCamera));
    if (m->animatesMaterials) {
        prMemCopy(pose.inputs, info.inputs, size_t(info.inputCount) * sizeof(PrModelSourceInput));
        prMemZero(pose.materialPlayed, info.materialCount * sizeof(bool));
    }
    if (playback.animation == kPrModelAnimationNone)
        return;
    const PrModelSourceAnimation& animation = info.animations[playback.animation];
    float* value = pose.scratch;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t t = 0; t < animation.trackCount; ++t) {
            const PrModelSourceTrack& track = info.tracks[animation.firstTrack + t];
            bool ofMesh =
                track.target == PrModelTrackTarget_MeshWeights || track.target == PrModelTrackTarget_MeshWeight;
            if ((pass == 0) != ofMesh || track.keyCount == 0)
                continue;
            evaluateTrack(info, track, playback.seconds, value);
            if (ofMesh) {
                setWeights(m, track, value, pose.weights);
                continue;
            }
            if (ofMaterial(track.target)) {
                if (track.object < info.materialCount && track.element < info.materials[track.object].inputCount) {
                    uint32_t input = info.materials[track.object].firstInput + track.element;
                    setInput(&pose.inputs[input], track.target, track.width, value);
                    pose.materialPlayed[track.object] = true;
                }
                continue;
            }
            if (track.target >= PrModelTrackTarget_LightColor && track.target <= PrModelTrackTarget_LightOuterConeAngle) {
                if (track.object < info.lightCount)
                    setLight(&pose.lights[track.object], track.target, value);
                continue;
            }
            if (track.target >= PrModelTrackTarget_CameraYfov && track.target <= PrModelTrackTarget_CameraZfar) {
                if (pose.cameras != nullptr && track.object < info.cameraCount)
                    setCamera(&pose.cameras[track.object], track.target, value[0]);
                continue;
            }
            if (track.object >= info.nodeCount || !m->inScene[track.object])
                continue;
            if (track.target == PrModelTrackTarget_Weights || track.target == PrModelTrackTarget_Weight) {
                setWeights(m, track, value, pose.weights);
                continue;
            }
            ModelTrs& trs = pose.trs[track.object];
            if (track.target == PrModelTrackTarget_Translation) {
                prMemCopy(trs.translation, value, sizeof(trs.translation));
            } else if (track.target == PrModelTrackTarget_Rotation) {
                prMemCopy(trs.rotation, value, sizeof(trs.rotation));
            } else if (track.target == PrModelTrackTarget_Scale) {
                prMemCopy(trs.scale, value, sizeof(trs.scale));
            } else if (track.target == PrModelTrackTarget_Matrix && track.width == 16) {
                prMemCopy(trs.matrix.m, value, sizeof(trs.matrix.m));
                trs.hasMatrix = true;
            } else if (track.target == PrModelTrackTarget_Visible) {
                pose.visible[track.object] = value[0] != 0;
            }
        }
    }
}

// Brings the placement's world matrices up to date with its root, its animations and their times.
void updatePose(ModelPlacement* p) {
    if (!p->poseDirty || !p->described)
        return;
    const Model* m = p->model;
    const PrModelSourceInfo& info = *m->info;
    p->poseDirty = false;
    p->sceneDirty = true;
    evaluatePlayback(m, p->playback, ModelPose{p->pose, p->visible, p->weights, p->lights, p->cameras, p->inputPose,
        p->materialPlayed, p->trackValues});
    if (p->blend.animation != kPrModelAnimationNone) {
        // The blend's tracks of materials and cameras are not played: they are those of the animation that it blends
        // into, whatever the weight.
        ModelPose blend = {
            p->blendPose, p->blendVisible, p->blendWeights, p->blendLights, nullptr, p->blendInputs,
            p->blendMaterialPlayed, p->trackValues,
        };
        evaluatePlayback(m, p->blend, blend);
        float t = p->blendWeight;
        for (uint32_t i = 0; i < m->weightCount; ++i)
            p->weights[i] = prF32Lerp(p->weights[i], p->blendWeights[i], t);
        // Visibility does not mix, so the animation of the greater weight decides it, and lights mix linearly.
        for (uint32_t i = 0; t >= 0.5f && i < m->order.count; ++i)
            p->visible[m->order.data[i]] = p->blendVisible[m->order.data[i]];
        for (uint32_t i = 0; i < info.lightCount; ++i) {
            PrModelSourceLight& a = p->lights[i];
            const PrModelSourceLight& b = p->blendLights[i];
            a.color = PrVec3{
                prF32Lerp(a.color.x, b.color.x, t), prF32Lerp(a.color.y, b.color.y, t),
                prF32Lerp(a.color.z, b.color.z, t),
            };
            a.intensity = prF32Lerp(a.intensity, b.intensity, t);
            a.innerConeAngle = prF32Lerp(a.innerConeAngle, b.innerConeAngle, t);
            a.outerConeAngle = prF32Lerp(a.outerConeAngle, b.outerConeAngle, t);
        }
        for (uint32_t i = 0; i < m->order.count; ++i) {
            ModelTrs& a = p->pose[m->order.data[i]];
            const ModelTrs& b = p->blendPose[m->order.data[i]];
            for (uint32_t k = 0; k < 3; ++k) {
                a.translation[k] = prF32Lerp(a.translation[k], b.translation[k], t);
                a.scale[k] = prF32Lerp(a.scale[k], b.scale[k], t);
            }
            float rotation[4];
            slerp(a.rotation, b.rotation, t, rotation);
            prMemCopy(a.rotation, rotation, sizeof(rotation));
            for (uint32_t r = 0; a.hasMatrix && b.hasMatrix && r < 4; ++r) {
                for (uint32_t c = 0; c < 4; ++c)
                    a.matrix.m[r][c] = prF32Lerp(a.matrix.m[r][c], b.matrix.m[r][c], t);
            }
        }
    }
    for (uint32_t i = 0; i < m->order.count; ++i) {
        PrModelNodeIndex index = m->order.data[i];
        const PrModelSourceNode& node = info.nodes[index];
        const ModelTrs& trs = p->pose[index];
        PrMatrix4 local = trs.hasMatrix ? trs.matrix : matrixFromTrs(trs.translation, trs.rotation, trs.scale);
        // A root of the scene may have a parent outside it, which the scene does not draw under. A node is visible if
        // it and its ancestors are.
        bool hasParent = node.parent != kPrModelNodeNone && m->inScene[node.parent];
        bool resets = (node.flags & PrModelNodeFlag_ResetsTransform) != 0;
        p->scenes[index] = hasParent && !resets ? worldOf(local, p->scenes[node.parent]) : local;
        p->visible[index] = p->visible[index] && (!hasParent || p->visible[node.parent]);
        p->worlds[index] = worldOf(p->scenes[index], p->desc.root);
    }
}

// Returns the matrix that maps a draw's mesh to the world: its node's, or, for a skinned mesh, the placement's root,
// since a skinned mesh follows its joints and not its node.
const PrMatrix4& drawWorld(const ModelPlacement* p, const ModelDraw& draw) {
    return draw.skin != kPrModelSkinNone ? p->desc.root : p->worlds[draw.node];
}

// Returns the matrix that maps the mesh of instance `instance` of a draw to the world: the instance's transform within
// its node, then the draw's.
PrMatrix4 instanceWorld(const ModelPlacement* p, const ModelDraw& draw, uint32_t instance) {
    return prMatrix4Concat(&p->model->instanceLocal[draw.firstInstance + instance], &drawWorld(p, draw));
}

// Returns the visibility mask of a draw's instances: the placement's, or none while the draw's node is hidden.
uint32_t drawMask(const ModelPlacement* p, const ModelDraw& draw) {
    return p->visible[draw.node] ? p->desc.visibility : 0;
}

PrRenderMobilityFlags drawMobility(const ModelDraw& draw) {
    return draw.animated ? PrRenderMobilityFlag_Dynamic : PrRenderMobilityFlag_Static;
}

// Returns the renderer's material that the placement draws a material of the source with: its own, if it has made one
// for an animation, and else the model's.
PrRenderMaterial placementMaterial(const ModelPlacement* p, PrModelMaterialIndex material) {
    if (p->model->animatesMaterials && p->ownMaterials[material].id != 0)
        return p->ownMaterials[material];
    return p->model->materials[material];
}

// Sets the placement's own materials to the materials of its pose: each material that its animation plays, which it
// makes in the renderer the first time, and each that it has made before, which may be at rest again.
void poseMaterials(ModelPlacement* p) {
    Model* m = p->model;
    if (!m->animatesMaterials)
        return;
    for (uint32_t i = 0; i < m->info->materialCount; ++i) {
        if (!p->materialPlayed[i] && p->ownMaterials[i].id == 0)
            continue;
        if (p->ownMaterials[i].id == 0) {
            p->ownMaterials[i] = prRenderMaterialCreate(m->render);
            p->materialsDirty = true;
        }
        modelBindMaterial(m, p->ownMaterials[i], i, p->inputPose);
    }
}

// Sets the renderer's material of each part of the model, as the placement's variant chooses it: the material that the
// variant maps the part to, or else the part's own.
void chooseMaterials(ModelPlacement* p) {
    const Model* m = p->model;
    const PrModelSourceInfo& info = *m->info;
    for (uint32_t part = 0; part < info.partCount; ++part) {
        const PrModelSourcePart& given = info.parts[part];
        PrModelMaterialIndex material = given.material;
        for (uint32_t k = 0; p->variant != kPrModelVariantNone && k < given.variantCount; ++k) {
            const PrModelSourceVariantMaterial& mapped = info.variantMaterials[given.firstVariant + k];
            if (mapped.variant == p->variant)
                material = mapped.material;
        }
        p->partMaterials[part] =
            material != kPrModelMaterialNone ? placementMaterial(p, material) : m->partMaterials[part];
    }
    p->materialsDirty = false;
}

// Returns the matrix that a joint of a draw's skin applies to the mesh as it was bound, in the source's scene: the
// joint's inverse bind matrix, and then the joint's node. A joint outside the model's scene stays where it was bound.
PrMatrix4 jointMatrix(const ModelPlacement* p, const ModelDraw& draw, uint32_t joint) {
    const PrModelSourceInfo& info = *p->model->info;
    uint32_t at = info.skins[draw.skin].firstJoint + joint;
    PrModelNodeIndex node = info.joints[at];
    if (node >= info.nodeCount || !p->model->inScene[node])
        return prMatrix4Identity();
    return prMatrix4Concat(&info.inverseBinds[at], &p->scenes[node]);
}

uint32_t skinJointCount(const Model* m, const ModelDraw& draw) {
    return draw.skin != kPrModelSkinNone ? m->info->skins[draw.skin].jointCount : 0;
}

// Returns what picks test of a mesh of the source, preparing it at the first pick that reaches the mesh.
ModelPickMesh& pickMesh(Model* m, PrModelMeshIndex index) {
    if (m->picks == nullptr)
        m->picks = allocArray<ModelPickMesh>(m->info->meshCount);
    ModelPickMesh& pick = m->picks[index];
    if (pick.prepared)
        return pick;
    pick.prepared = true;
    const ModelMesh& loaded = m->meshes[index];
    // The mesh is prepared as its renderer's mesh is, so that its vertices deform as the drawn ones do.
    ModelMeshPrepared prepared{};
    PartPlan plan{};
    PrStatus status = prepareMesh(m, index, true, false, &prepared, &plan);
    if (status == PrStatus_Ok && prepared.parts.count == loaded.partCount) {
        for (uint32_t k = 0; k < prepared.parts.count; ++k) {
            pick.partFirstIndex.push(prepared.parts.data[k].firstIndex);
            pick.partSources.push(loaded.firstPart + k);
        }
        for (uint32_t v = 0; v < prepared.vertexCount; ++v)
            pick.positions.append(modelMeshVertex(prepared, v)->position, 3);
        pick.indices.append(prepared.indices.span());
        pick.deforms = loaded.jointCount != 0 || loaded.morphTargetCount != 0;
        if (loaded.jointCount != 0)
            pick.skin.append(prepared.skin.span());
        pick.morphTargetCount = loaded.morphTargetCount;
        for (uint32_t i = 0; i < prepared.morphs.count; ++i)
            pick.morphs.append(prepared.morphs.data[i].position, 3);
        PrMeshAttributeView positions{
            pick.positions.data, prepared.vertexCount, 3 * sizeof(float),
            prMeshFormatFloat32(3),
        };
        PrMeshIndexView indices{pick.indices.data, pick.indices.count, PrMeshIndexType_U32};
        pick.nodes.resize(prMeshBvhMaxNodes(pick.indices.count / 3));
        pick.triangles.resize(pick.indices.count / 3);
        if (prMeshBvhBuild(positions, indices, pick.nodes.data, pick.triangles.data, &pick.bvh) != PrStatus_Ok)
            pick.bvh = PrMeshBvhView{};
        pick.builtCost = prMeshBvhCost(pick.bvh);
    }
    plan.components.reset();
    modelMeshPreparedReset(&prepared);
    return pick;
}

// Frees what picks test of every mesh.
void resetPicks(Model* m) {
    if (m->picks == nullptr)
        return;
    for (uint32_t i = 0; i < m->info->meshCount; ++i) {
        ModelPickMesh& pick = m->picks[i];
        pick.positions.reset();
        pick.indices.reset();
        pick.skin.reset();
        pick.morphs.reset();
        pick.partFirstIndex.reset();
        pick.partSources.reset();
        pick.nodes.reset();
        pick.triangles.reset();
    }
    prMemoryFree(m->picks);
    m->picks = nullptr;
    m->pickScratch.positions.reset();
    m->pickScratch.nodes.reset();
    m->pickScratch.joints.reset();
}

// Returns the ray in the space that `world` maps to the world. A point at t on the ray maps to the point at t on the
// returned ray.
PrMeshRay rayInto(const PrMatrix4& world, PrMeshRay ray) {
    PrMatrix4 inverse{};
    if (!prMatrix4Invert(&world, &inverse))
        return PrMeshRay{.tMin = 1, .tMax = 0};
    PrAffine3 into = prAffine3FromMatrix4(&inverse);
    PrVec3 origin = prAffine3MapPoint(&into, ray.origin);
    PrVec3 ahead = prAffine3MapPoint(&into, ray.origin + ray.direction);
    return PrMeshRay{.origin = origin, .tMin = ray.tMin, .direction = ahead - origin, .tMax = ray.tMax};
}

// Returns the source's part that holds triangle `triangle` of a mesh.
PrModelPartIndex partOfTriangle(const ModelPickMesh& pick, uint32_t triangle) {
    // The parts' first indices ascend from zero, so the part is the last whose first index is at most the triangle's.
    uint32_t part = upperBound(pick.partFirstIndex.span(), triangle * 3);
    return pick.partSources.data[part - 1];
}

// A refitted hierarchy that costs this many times what its build did is built again over the shape. A deformation that
// keeps neighbors near, such as a pose or a blend of targets, refits close to the build's cost, and one that tears the
// mesh apart costs more at each query than a build does once.
constexpr float kPickRebuildCost = 2.0f;

// Writes to the model's scratch the positions of a draw's mesh in the shape that the placement's pose gives it, as the
// renderer deforms its vertices: each morph target's displacement at the draw's weight added to the position as the
// mesh was bound, and then, for a draw with a skin, the joints' blend in the pose. Returns them.
PrMeshAttributeView deformedPositions(ModelPlacement* p, const ModelDraw& draw, const ModelPickMesh& pick) {
    Model* m = p->model;
    ModelPickScratch& scratch = m->pickScratch;
    uint32_t vertexCount = pick.positions.count / 3;
    uint32_t targetCount = pick.morphTargetCount < draw.weightCount ? pick.morphTargetCount : draw.weightCount;
    const float* weights = p->weights + draw.firstWeight;
    uint32_t jointCount = skinJointCount(m, draw);
    scratch.joints.resize(jointCount);
    for (uint32_t j = 0; j < jointCount; ++j) {
        PrMatrix4 joint = jointMatrix(p, draw, j);
        scratch.joints.data[j] = prAffine3FromMatrix4(&joint);
    }
    scratch.positions.resize(pick.positions.count);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        const float* at = pick.positions.data + 3 * v;
        PrVec3 position{at[0], at[1], at[2]};
        const float* deltas = pick.morphs.data + size_t(v) * pick.morphTargetCount * 3;
        for (uint32_t t = 0; t < targetCount; ++t) {
            const float* delta = deltas + 3 * t;
            position = position + PrVec3{delta[0], delta[1], delta[2]} * weights[t];
        }
        if (jointCount != 0) {
            const PrRenderSkinVertex& skin = pick.skin.data[v];
            PrVec3 sum{};
            float total = 0;
            for (uint32_t k = 0; k < kPrRenderSkinInfluences; ++k) {
                float weight = float(skin.weights[k]) / 65535.0f;
                if (weight == 0 || skin.joints[k] >= jointCount)
                    continue;
                sum = sum + prAffine3MapPoint(&scratch.joints.data[skin.joints[k]], position) * weight;
                total += weight;
            }
            // A vertex that no joint moves stays where it was bound.
            if (total > 0)
                position = sum;
        }
        scratch.positions.data[3 * v] = position.x;
        scratch.positions.data[3 * v + 1] = position.y;
        scratch.positions.data[3 * v + 2] = position.z;
    }
    return PrMeshAttributeView{scratch.positions.data, vertexCount, 3 * sizeof(float), prMeshFormatFloat32(3)};
}

// Returns the hierarchy of a mesh that deforms, over its triangles at `positions`: the mesh's tree refitted into the
// model's scratch, or the tree built again over the shape if the refit costs too much more than the last build.
PrMeshBvhView deformedHierarchy(Model* m, ModelPickMesh& pick, PrMeshAttributeView positions) {
    PrMeshIndexView indices{pick.indices.data, pick.indices.count, PrMeshIndexType_U32};
    ModelPickScratch& scratch = m->pickScratch;
    scratch.nodes.resize(pick.bvh.nodeCount);
    PrMeshBvhView refitted = pick.bvh;
    refitted.nodes = scratch.nodes.data;
    if (prMeshBvhRefit(pick.bvh, positions, indices, scratch.nodes.data) == PrStatus_Ok &&
            !(prMeshBvhCost(refitted) > pick.builtCost * kPickRebuildCost))
        return refitted;
    if (prMeshBvhBuild(positions, indices, pick.nodes.data, pick.triangles.data, &pick.bvh) != PrStatus_Ok)
        pick.bvh = PrMeshBvhView{};
    pick.builtCost = prMeshBvhCost(pick.bvh);
    return pick.bvh;
}

// Tests the instances of a draw against `ray`, in the world, with the draw's mesh at `positions` under `bvh`, in the
// space that each instance maps to the world, and writes a hit nearer than `*out` to it.
void pickInstances(const ModelPlacement* p, const ModelDraw& draw, const ModelPickMesh& pick, PrMeshBvhView bvh,
        PrMeshAttributeView positions, PrMeshRay ray, PrModelPick* out, bool* found) {
    PrMeshIndexView indices{pick.indices.data, pick.indices.count, PrMeshIndexType_U32};
    for (uint32_t n = 0; n < draw.instanceCount; ++n) {
        PrMeshRayHit hit{};
        PrMeshRay local = rayInto(instanceWorld(p, draw, n), ray);
        if (!prMeshBvhIntersect(bvh, positions, indices, local, &hit) || (*found && hit.t >= out->t))
            continue;
        *found = true;
        *out = PrModelPick{
            .node = draw.node,
            .mesh = draw.mesh,
            .part = partOfTriangle(pick, hit.triangle),
            .instance = n,
            .t = hit.t,
            .position = ray.origin + ray.direction * hit.t,
        };
    }
}

// Writes the joints and the morph weights of a draw's instances, in the placement's pose, to `update`.
void writeDeformation(ModelPlacement* p, PrRenderUpdate* update, uint32_t index) {
    const Model* m = p->model;
    const ModelDraw& draw = m->draws.data[index];
    uint32_t count = skinJointCount(m, draw);
    for (uint32_t j = 0; j < count; ++j) {
        PrMatrix4 joint = jointMatrix(p, draw, j);
        p->joints[j] = prAffine3FromMatrix4(&joint);
    }
    for (uint32_t i = 0; i < draw.instanceCount; ++i) {
        PrRenderInstance instance = p->instances[draw.firstInstance + i];
        if (count != 0)
            prRenderInstanceSetJoints(update, instance, p->joints, count);
        if (draw.weightCount != 0)
            prRenderInstanceSetMorphWeights(update, instance, p->weights + draw.firstWeight, draw.weightCount);
    }
}

// A box of the world that grows to hold other boxes.
struct BoxUnion {
    PrBox3 box;
    bool found;
};

// Grows a box to hold the corners of `box` under `matrix`. A corner that is not finite is left out.
void addBox(BoxUnion* bounds, const PrBox3& box, const PrMatrix4& matrix) {
    PrAffine3 transform = prAffine3FromMatrix4(&matrix);
    for (uint32_t corner = 0; corner < 8; ++corner) {
        PrVec3 point = prAffine3MapPoint(&transform, PrVec3{
            (corner & 1) != 0 ? box.max.x : box.min.x,
            (corner & 2) != 0 ? box.max.y : box.min.y,
            (corner & 4) != 0 ? box.max.z : box.min.z,
        });
        if (!prF32IsFinite(point.x + point.y + point.z))
            continue;
        if (!bounds->found)
            bounds->box = PrBox3{point, point};
        bounds->found = true;
        bounds->box = prBox3Include(bounds->box, point);
    }
}

// Returns the box of what a part's morph targets add to its positions at a draw's weights in a placement: along each
// axis, the least and the greatest that each target adds at its weight, summed.
PrBox3 morphBox(const ModelPlacement* p, const ModelDraw& draw, const PrModelSourcePart& part) {
    const PrModelSourceInfo& info = *p->model->info;
    PrBox3 moved{};
    for (uint32_t t = 0; t < part.morphTargetCount && t < draw.weightCount; ++t) {
        float weight = p->weights[draw.firstWeight + t];
        PrVec3 a = info.morphBoxes[part.firstMorphBox + t].min * weight;
        PrVec3 b = info.morphBoxes[part.firstMorphBox + t].max * weight;
        moved.min = moved.min + PrVec3{prF32Min(a.x, b.x), prF32Min(a.y, b.y), prF32Min(a.z, b.z)};
        moved.max = moved.max + PrVec3{prF32Max(a.x, b.x), prF32Max(a.y, b.y), prF32Max(a.z, b.z)};
    }
    return moved;
}

// Returns the model's light `index` as the renderer takes it, in its pose and at the placement's scale of it, which
// shines with no intensity while its node is hidden. A light of an area is the punctual light of its intensity along
// its axis: a sphere or a cylinder is a point, and a disk or a rectangle, which emits from one side and falls with the
// cosine, is a spot of a half-space, and a shaping cone makes either a spot of the cone.
PrRenderLightDesc lightDesc(const ModelPlacement* p, uint32_t index) {
    const Model* m = p->model;
    PrModelNodeIndex node = m->lights.data[index];
    const PrModelSourceLight& light = p->lights[m->info->nodes[node].light];
    const PrMatrix4& world = p->worlds[node];
    PrRenderLightDesc desc = prRenderLightDescDefault();
    desc.kind = PrRenderLightKind_Point;
    float intensity = light.intensity;
    float inner = light.innerConeAngle;
    float outer = light.outerConeAngle;
    float radius = light.radius;
    bool area = light.kind >= PrModelLightKind_Sphere;
    switch (light.kind) {
        case PrModelLightKind_Directional:
            desc.kind = PrRenderLightKind_Directional;
            break;
        case PrModelLightKind_Spot:
            desc.kind = PrRenderLightKind_Spot;
            break;
        case PrModelLightKind_Sphere:
            // A sphere seen from afar is a disc of its radius.
            intensity = light.normalized || light.treatAsPoint ? light.intensity / 4 : light.intensity * kPi * radius * radius;
            break;
        case PrModelLightKind_Cylinder:
            intensity = light.normalized ? light.intensity / kPi : light.intensity * 2 * radius * light.length;
            break;
        case PrModelLightKind_Disk:
        case PrModelLightKind_Rect: {
            // A flat emitter's intensity along its axis is its radiance times its area, and falls with the cosine.
            float size = light.kind == PrModelLightKind_Disk ? kPi * radius * radius : light.width * light.height;
            intensity = light.normalized ? light.intensity : light.intensity * size;
            desc.kind = PrRenderLightKind_Spot;
            if (!(outer < kPi / 2)) {
                inner = 0;
                outer = kPi / 2;
            }
            break;
        }
        default:
            break;
    }
    if (area && outer < kPi / 2)
        desc.kind = PrRenderLightKind_Spot;
    if (area && desc.kind == PrRenderLightKind_Point) {
        inner = 0;
        outer = kPi / 4;
    }
    desc.color = light.color;
    desc.intensity = p->visible[node] ? intensity * p->lightScales[index] : 0;
    desc.position = columnOf(world, 3);
    desc.direction = unitOr(-columnOf(world, 2), PrVec3{0, 0, -1});
    desc.range = prF32IsFinite(light.range) ? light.range : 0;
    desc.innerConeAngle = inner;
    desc.outerConeAngle = outer;
    desc.castsShadows = false;
    return desc;
}

// ---------------------------------------------------------------------------------------------------------------------
// A placement's instances

// Returns true if the placement draws a node with its highlight: the highlighted node, or a node under it.
bool highlighted(const ModelPlacement* p, PrModelNodeIndex node) {
    if (p->highlightNode == kPrModelNodeNone || p->highlight.id == 0)
        return false;
    const PrModelSourceInfo& info = *p->model->info;
    for (uint32_t depth = 0; node != kPrModelNodeNone && depth < info.nodeCount; ++depth) {
        if (node == p->highlightNode)
            return true;
        node = info.nodes[node].parent;
    }
    return false;
}

// Makes the arrays of a placement of a model whose description has arrived, in its rest pose.
void placementDescribe(ModelPlacement* p) {
    const Model* m = p->model;
    const PrModelSourceInfo& info = *m->info;
    uint32_t nodeCount = info.nodeCount;
    p->pose = allocArray<ModelTrs>(nodeCount);
    p->blendPose = allocArray<ModelTrs>(nodeCount);
    p->scenes = allocArray<PrMatrix4>(nodeCount);
    p->worlds = allocArray<PrMatrix4>(nodeCount);
    p->visible = allocArray<bool>(nodeCount);
    p->blendVisible = allocArray<bool>(nodeCount);
    p->lights = allocArray<PrModelSourceLight>(info.lightCount);
    p->blendLights = allocArray<PrModelSourceLight>(info.lightCount);
    p->cameras = allocArray<PrModelSourceCamera>(info.cameraCount);
    uint32_t inputCount = m->animatesMaterials ? info.inputCount : 0;
    uint32_t materialCount = m->animatesMaterials ? info.materialCount : 0;
    p->inputPose = allocArray<PrModelSourceInput>(inputCount);
    p->blendInputs = allocArray<PrModelSourceInput>(inputCount);
    p->materialPlayed = allocArray<bool>(materialCount);
    p->blendMaterialPlayed = allocArray<bool>(materialCount);
    p->ownMaterials = allocArray<PrRenderMaterial>(materialCount);
    p->partMaterials = allocArray<PrRenderMaterial>(info.partCount);
    p->weights = allocArray<float>(m->weightCount);
    p->blendWeights = allocArray<float>(m->weightCount);
    p->trackValues = allocArray<float>(m->trackWidth > 16 ? m->trackWidth : 16);
    p->transforms = allocArray<PrAffine3>(m->instanceCount);
    p->joints = allocArray<PrAffine3>(m->jointCount);
    p->instances = allocArray<PrRenderInstance>(m->instanceCount);
    p->shown = allocArray<bool>(m->draws.count);
    p->sceneLights = allocArray<PrRenderLight>(m->lights.count);
    p->lightScales = allocArray<float>(m->lights.count);
    for (uint32_t i = 0; i < m->lights.count; ++i)
        p->lightScales[i] = 1;
    p->described = true;
    p->poseDirty = true;
    p->materialsDirty = true;
}

// Adds the placement's instances and lights to its scene, in its pose. A placement waits for its model's description.
void enterScene(ModelPlacement* p) {
    Model* m = p->model;
    if (p->desc.scene.id == 0 || !p->described || m->render == nullptr)
        return;
    updatePose(p);
    poseMaterials(p);
    chooseMaterials(p);
    PrRenderUpdate* update = prRenderUpdateBegin(m->render, p->desc.scene);
    // The parts of a highlighted draw take the highlight, as many as its mesh has.
    RawArray<PrRenderMaterial> highlights;
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelDraw& draw = m->draws.data[i];
        const ModelMesh& mesh = m->meshes[draw.mesh];
        PrRenderInstanceDesc desc = prRenderInstanceDescDefault(mesh.mesh);
        desc.materials = p->partMaterials + mesh.firstPart;
        if (highlighted(p, draw.node)) {
            highlights.clear();
            for (uint32_t part = 0; part < mesh.partCount; ++part)
                highlights.push(p->highlight);
            desc.materials = highlights.data;
        }
        desc.materialCount = mesh.partCount;
        desc.mobility = drawMobility(draw);
        desc.visibility = drawMask(p, draw);
        p->shown[i] = p->visible[draw.node];
        for (uint32_t k = 0; k < draw.instanceCount; ++k) {
            PrMatrix4 world = instanceWorld(p, draw, k);
            desc.world = prAffine3FromMatrix4(&world);
            p->instances[draw.firstInstance + k] = prRenderInstanceAdd(update, &desc);
        }
        writeDeformation(p, update, i);
    }
    for (uint32_t i = 0; p->desc.addsLights && i < m->lights.count; ++i) {
        PrRenderLightDesc desc = lightDesc(p, i);
        p->sceneLights[i] = prRenderLightAdd(update, &desc);
    }
    prRenderUpdateCommit(update);
    highlights.reset();
    p->sceneDirty = false;
}

// Removes the placement's instances and lights from its scene, and leaves the placement in no scene.
void leaveScene(ModelPlacement* p) {
    Model* m = p->model;
    if (p->desc.scene.id == 0)
        return;
    if (!p->described || m->render == nullptr) {
        p->desc.scene = PrRenderScene{};
        return;
    }
    PrRenderUpdate* update = prRenderUpdateBegin(m->render, p->desc.scene);
    for (uint32_t i = 0; i < m->instanceCount; ++i) {
        if (p->instances[i].id != 0)
            prRenderInstanceRemove(update, p->instances[i]);
        p->instances[i] = PrRenderInstance{};
    }
    for (uint32_t i = 0; i < m->lights.count; ++i) {
        if (p->sceneLights[i].id != 0)
            prRenderLightRemove(update, p->sceneLights[i]);
        p->sceneLights[i] = PrRenderLight{};
    }
    prRenderUpdateCommit(update);
    // The renderer frees a material once a frame has applied the removal of the instances that draw it.
    for (uint32_t i = 0; m->animatesMaterials && i < m->info->materialCount; ++i) {
        if (p->ownMaterials[i].id != 0)
            prRenderMaterialDestroy(m->render, p->ownMaterials[i]);
        p->ownMaterials[i] = PrRenderMaterial{};
    }
    p->desc.scene = PrRenderScene{};
}

// Writes the placement's pose to its instances and lights, in `update`: their transforms, their deformations, whether
// each draw is shown, and the materials of its variant and its highlight.
void writeScene(ModelPlacement* p, PrRenderUpdate* update) {
    const Model* m = p->model;
    poseMaterials(p);
    bool materials = p->materialsDirty;
    if (materials)
        chooseMaterials(p);
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelDraw& draw = m->draws.data[i];
        for (uint32_t k = 0; k < draw.instanceCount; ++k) {
            PrMatrix4 world = instanceWorld(p, draw, k);
            p->transforms[draw.firstInstance + k] = prAffine3FromMatrix4(&world);
        }
        writeDeformation(p, update, i);
        bool shown = p->visible[draw.node];
        for (uint32_t k = 0; shown != p->shown[i] && k < draw.instanceCount; ++k)
            prRenderInstanceSetMasks(update, p->instances[draw.firstInstance + k], drawMobility(draw), drawMask(p, draw));
        p->shown[i] = shown;
        const ModelMesh& mesh = m->meshes[draw.mesh];
        bool lit = materials && highlighted(p, draw.node);
        for (uint32_t k = 0; materials && k < draw.instanceCount; ++k) {
            for (uint32_t part = 0; part < mesh.partCount; ++part) {
                PrRenderMaterial material = lit ? p->highlight : p->partMaterials[mesh.firstPart + part];
                prRenderInstanceSetMaterial(update, p->instances[draw.firstInstance + k], part, material);
            }
        }
    }
    prRenderInstanceSetTransforms(update, p->instances, p->transforms, m->instanceCount);
    for (uint32_t i = 0; i < m->lights.count; ++i) {
        if (p->sceneLights[i].id == 0)
            continue;
        PrRenderLightDesc desc = lightDesc(p, i);
        prRenderLightSet(update, p->sceneLights[i], &desc);
    }
    p->sceneDirty = false;
}

void setPlayback(ModelPlacement* p, ModelPlayback* playback, float seconds) {
    playback->seconds = wrapTime(seconds, durationOf(p->model, playback->animation), p->loop);
    p->poseDirty = true;
}

// Ends the model's use of its renderer: its jobs, its placements' instances and its resources.
void depopulate(Model* m) {
    if (m->render == nullptr)
        return;
    m->cancelled.store(true);
    finishJobs(m);
    m->cancelled.store(false);
    m->jobs.clear();
    m->jobsNext = 0;
    for (uint32_t i = 0; i < m->placements.count; ++i)
        leaveScene(m->placements.data[i]);
    if (m->described) {
        const PrModelSourceInfo& info = *m->info;
        for (uint32_t i = 0; i < info.meshCount; ++i) {
            if (m->meshes[i].mesh.id != 0)
                prRenderMeshDestroy(m->render, m->meshes[i].mesh);
            m->meshes[i].mesh = PrRenderMesh{};
        }
        for (uint32_t i = 0; i < info.materialCount; ++i) {
            if (m->materials[i].id != 0)
                prRenderMaterialDestroy(m->render, m->materials[i]);
            m->materials[i] = PrRenderMaterial{};
        }
    }
    if (m->defaultMaterial.id != 0)
        prRenderMaterialDestroy(m->render, m->defaultMaterial);
    m->defaultMaterial = PrRenderMaterial{};
    for (uint32_t i = 0; i < m->textures.count; ++i) {
        if (m->textures.data[i].texture.id != 0)
            prRenderTextureDestroy(m->render, m->textures.data[i].texture);
        m->textures.data[i].texture = PrRenderTexture{};
    }
    if (m->environment.environment.id != 0)
        prRenderEnvironmentDestroy(m->render, m->environment.environment);
    m->environment.environment = PrRenderEnvironment{};
    m->render = nullptr;
}

// Sets every resource that the model loads to pending, or to none for those that it does not load.
void resetLoads(Model* m) {
    if (!m->described)
        return;
    for (uint32_t i = 0; i < m->info->imageCount; ++i)
        setLoad(&m->images[i].state, m->images[i].used ? PrModelLoad_Pending : PrModelLoad_None, PrStatus_Ok);
    for (uint32_t i = 0; i < m->info->meshCount; ++i)
        setLoad(&m->meshes[i].state, m->meshes[i].partCount != 0 ? PrModelLoad_Pending : PrModelLoad_None, PrStatus_Ok);
}

// The opening of a model from bytes: the model, the bytes, which it retains, their location, their media type once they
// are classified, and the next of the model's formats to try.
struct ModelOpen {
    Model* model;
    const PrModelResolver* resolver;
    const PrBlobSource* bytes;
    PrRawString location;
    PrStringView type;
    uint32_t next;
};

void openNext(ModelOpen* open);

// Ends the opening with the source that a format made, or with its failure. A format that refuses the bytes as
// NotSupported leaves them to the next.
void sourceOpened(void* PR_NULLABLE context, PrStatus status, PrModelSource* PR_NULLABLE source) {
    ModelOpen* open = static_cast<ModelOpen*>(context);
    if (status == PrStatus_NotSupported && source == nullptr && open->next < open->model->formats.count) {
        openNext(open);
        return;
    }
    Model* m = open->model;
    prMutexLock(&m->lock);
    if (status == PrStatus_Ok && source != nullptr) {
        m->opened = source;
    } else {
        prModelSourceDestroy(source);
        setLoad(&m->sourceState, PrModelLoad_Failed, status != PrStatus_Ok ? status : PrStatus_InvalidData);
    }
    prMutexUnlock(&m->lock);
    prBlobSourceRelease(open->bytes);
    prRawStringReset(&open->location);
    prMemoryFree(open);
    m->revision.fetchAdd(1);
    modelJobFinish(m);
}

// Tries the next of the model's formats that supports the bytes' media type, or fails as NotSupported.
void openNext(ModelOpen* open) {
    Model* m = open->model;
    while (open->next < m->formats.count) {
        const PrModelFormat& format = m->formats.data[open->next++];
        if (!format.supportsTypeFn(format.context, open->type))
            continue;
        format.openFn(format.context, open->bytes, prRawStringView(&open->location), open->resolver, sourceOpened, open);
        return;
    }
    sourceOpened(open, PrStatus_NotSupported, nullptr);
}

// Classifies the bytes by their first bytes and their location, and opens them.
void prefixRead(void* PR_NULLABLE context, PrBlobRead* read) {
    ModelOpen* open = static_cast<ModelOpen*>(context);
    PrStatus status = prBlobReadStatus(read);
    PrByteView prefix = status == PrStatus_Ok ? prBlobBytes(prBlobReadBlob(read)) : PrByteView{};
    open->type = prMimeFormatInfo(prMimeSniff(prefix, prRawStringView(&open->location))).type;
    prBlobReadDestroy(read);
    if (status != PrStatus_Ok) {
        open->next = open->model->formats.count;
        sourceOpened(open, status, nullptr);
        return;
    }
    openNext(open);
}

// Readies a new model without a source, whose description arrives later, or never.
void modelInit(Model* m, const PrModelOptions* PR_NULLABLE options) {
    m->options = options != nullptr ? *options : prModelOptionsDefault();
    prMutexInit(&m->lock);
    prConditionInit(&m->idle);
    m->cancelled.store(false);
    setLoad(&m->sourceState, PrModelLoad_Pending, PrStatus_Ok);
}

} // namespace

void modelJobFinish(Model* m) {
    prMutexLock(&m->lock);
    if (--m->jobsPending == 0)
        prConditionBroadcast(&m->idle);
    prMutexUnlock(&m->lock);
}

void modelDiagnose(Model* m, PrStringView feature, PrStringView detail) {
    prMutexLock(&m->lock);
    bool known = false;
    for (uint32_t i = 0; !known && i < m->diagnostics.count; ++i)
        known = m->diagnostics.data[i].feature == feature;
    if (!known)
        m->diagnostics.push(PrModelDiagnostic{feature, detail});
    prMutexUnlock(&m->lock);
}

const PrModelSourceInput* PR_NULLABLE modelMaterialInput(const Model* m, PrModelMaterialIndex material,
        const PrModelSourceInput* inputs, PrStringView name) {
    if (material == kPrModelMaterialNone)
        return nullptr;
    const PrModelSourceMaterial& found = m->info->materials[material];
    for (uint32_t i = 0; i < found.inputCount; ++i) {
        if (inputs[found.firstInput + i].name == name)
            return &inputs[found.firstInput + i];
    }
    return nullptr;
}

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrModel : Model {};
struct PrModelPlacement : ModelPlacement {};

// ---------------------------------------------------------------------------------------------------------------------
// Models

PrModelOptions prModelOptionsDefault(void) {
    return PrModelOptions{};
}

PrModel* prModelCreate(PrModelSource* source, const PrModelOptions* PR_NULLABLE options) {
    PrModel* model = allocZeroed<PrModel>();
    modelInit(model, options);
    model->source = source;
    describe(model);
    return model;
}

PrModel* prModelOpen(const PrBlobSource* bytes, PrStringView location, const PrModelResolver* resolver,
        const PrModelFormat* PR_NULLABLE formats, uint32_t formatCount, const PrModelOptions* PR_NULLABLE options) {
    PrModel* model = allocZeroed<PrModel>();
    Model* m = model;
    modelInit(m, options);
    m->formats.append(formats, formatCount);
    ModelOpen* open = allocZeroed<ModelOpen>();
    open->model = m;
    open->resolver = resolver;
    open->bytes = bytes;
    prBlobSourceRetain(bytes);
    open->location = prRawStringCopy(location);
    // The opening counts as a job, so that a wait and a destruction wait for it.
    m->jobsPending = 1;
    PrBlobReadDesc read = prBlobReadDescDefault();
    read.range = PrBlobRange{0, kPrMimeSniffBytes};
    read.done = prefixRead;
    read.context = open;
    prBlobReadCreate(bytes, &read);
    return model;
}

void prModelDestroy(PrModel* PR_NULLABLE model) {
    if (model == nullptr)
        return;
    Model* m = model;
    depopulate(m);
    // The opening of the source, which may still be reading.
    finishJobs(m);
    while (m->placements.count != 0)
        prModelPlacementDestroy(static_cast<PrModelPlacement*>(m->placements.last()));
    prModelSourceDestroy(m->opened);
    prMutexReset(&m->lock);
    prConditionReset(&m->idle);
    if (m->described) {
        for (uint32_t i = 0; i < m->info->meshCount; ++i)
            prMemoryFree(m->meshes[i].jointBoxes);
        resetPicks(m);
    }
    prMemoryFree(m->parts);
    prMemoryFree(m->meshes);
    prMemoryFree(m->images);
    prMemoryFree(m->textureImages);
    prMemoryFree(m->inputTextures);
    prMemoryFree(m->materials);
    prMemoryFree(m->bindings);
    prMemoryFree(m->partMaterials);
    prMemoryFree(m->inScene);
    prMemoryFree(m->nodeDraws);
    prMemoryFree(m->instanceLocal);
    m->textures.reset();
    m->order.reset();
    m->draws.reset();
    m->cameras.reset();
    m->lights.reset();
    m->placements.reset();
    m->layouts.reset();
    m->jobs.reset();
    m->formats.reset();
    m->diagnostics.reset();
    prModelSourceDestroy(m->source);
    prMemoryFree(model);
}

const PrModelSource* PR_NULLABLE prModelSource(const PrModel* model) {
    return model->described ? model->source : nullptr;
}

uint64_t prModelRevision(const PrModel* model) {
    return model->revision.load();
}

uint32_t prModelNodeCount(const PrModel* model) {
    return model->described ? model->info->nodeCount : 0;
}

PrModelNodeIndex prModelNodeParent(const PrModel* model, PrModelNodeIndex node) {
    PR_ASSERT(node < prModelNodeCount(model));
    return model->info->nodes[node].parent;
}

PrStringView prModelNodeName(const PrModel* model, PrModelNodeIndex node) {
    PR_ASSERT(node < prModelNodeCount(model));
    return model->info->nodes[node].name;
}

void prModelNodeAddress(const PrModel* model, PrModelNodeIndex node, PrTextBuffer* out) {
    PR_ASSERT(node < prModelNodeCount(model));
    prModelSourceNodeAddress(model->source, node, out);
}

PrModelNodeIndex prModelNodeFromAddress(const PrModel* model, PrStringView address) {
    return model->described ? prModelSourceNodeFromAddress(model->source, address) : kPrModelNodeNone;
}

uint32_t prModelAnimationCount(const PrModel* model) {
    return model->described ? model->info->animationCount : 0;
}

PrStringView prModelAnimationName(const PrModel* model, PrModelAnimationIndex animation) {
    PR_ASSERT(animation < prModelAnimationCount(model));
    return model->info->animations[animation].name;
}

float prModelAnimationDuration(const PrModel* model, PrModelAnimationIndex animation) {
    return durationOf(model, animation);
}

void prModelPopulate(PrModel* model, PrRender* PR_NULLABLE render) {
    Model* m = model;
    depopulate(m);
    resetLoads(m);
    if (render == nullptr)
        return;
    m->render = render;
    m->revision.fetchAdd(1);
    if (m->described)
        populateResources(m);
}

PrRender* PR_NULLABLE prModelRender(const PrModel* model) {
    return model->render;
}

void prModelSetEmission(PrModel* model, PrRenderEmission emission) {
    Model* m = model;
    if (m->options.emission == emission)
        return;
    m->options.emission = emission;
    if (m->render == nullptr || !m->described)
        return;
    m->unsampledTextures = 0;
    for (uint32_t i = 0; i < m->info->materialCount; ++i)
        m->unsampledTextures += modelBindMaterial(m, m->materials[i], i, m->info->inputs);
    if (m->defaultMaterial.id != 0)
        modelBindMaterial(m, m->defaultMaterial, kPrModelMaterialNone, nullptr);
    // A placement's own materials hold an animation's pose of the source's.
    for (uint32_t k = 0; m->animatesMaterials && k < m->placements.count; ++k) {
        ModelPlacement* p = m->placements.data[k];
        for (uint32_t i = 0; p->described && i < m->info->materialCount; ++i) {
            if (p->ownMaterials[i].id != 0)
                modelBindMaterial(m, p->ownMaterials[i], i, p->inputPose);
        }
    }
}

PrRenderEmission prModelEmission(const PrModel* model) {
    return model->options.emission;
}

PrModelLightingFlags prModelLighting(const PrModel* model) {
    const Model* m = model;
    PrModelLightingFlags flags = 0;
    if (!m->described)
        return flags;
    if (m->lights.count != 0)
        flags |= PrModelLightingFlag_Lights;
    if (m->info->environment.node != kPrModelNodeNone)
        flags |= PrModelLightingFlag_Environment;
    // The surfaces are the parts of the meshes that the scene's draws draw.
    bool any = false;
    bool lit = false;
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelMesh& mesh = m->meshes[m->draws.data[i].mesh];
        for (uint32_t k = 0; k < mesh.partCount; ++k) {
            PrModelMaterialIndex material = m->info->parts[mesh.firstPart + k].material;
            any = true;
            if (modelBindIsUnlit(m, material))
                continue;
            lit = true;
            if (modelBindEmits(m, material))
                flags |= PrModelLightingFlag_Emission;
        }
    }
    if (any && !lit)
        flags |= PrModelLightingFlag_Unlit;
    return flags;
}

void prModelUpdate(PrModel* model) {
    Model* m = model;
    takeSource(m);
    if (m->options.pool == nullptr && m->jobsNext < m->jobs.count)
        runJob(&m->jobs.data[m->jobsNext++]);
    for (uint32_t i = 0; m->render != nullptr && i < m->placements.count; ++i) {
        ModelPlacement* first = m->placements.data[i];
        if (first->desc.scene.id == 0 || !first->described)
            continue;
        updatePose(first);
        if (!first->sceneDirty)
            continue;
        // One update holds the changes of every placement in this scene.
        PrRenderUpdate* update = prRenderUpdateBegin(m->render, first->desc.scene);
        for (uint32_t k = i; k < m->placements.count; ++k) {
            ModelPlacement* p = m->placements.data[k];
            if (p->desc.scene.id != first->desc.scene.id || !p->described)
                continue;
            updatePose(p);
            if (p->sceneDirty)
                writeScene(p, update);
        }
        prRenderUpdateCommit(update);
    }
}

void prModelWait(PrModel* model) {
    Model* m = model;
    // The source first, whose description makes the jobs that the wait then finishes.
    prMutexLock(&m->lock);
    while (m->opened == nullptr && m->sourceState.load.load() == PrModelLoad_Pending && !m->described)
        prConditionWait(&m->idle, &m->lock);
    prMutexUnlock(&m->lock);
    takeSource(m);
    finishJobs(m);
}

// ---------------------------------------------------------------------------------------------------------------------
// Loading

PrModelProgress prModelProgress(const PrModel* model) {
    PrModelProgress progress{
        .source = model->sourceState.load.load(),
        .sourceStatus = model->sourceState.status.load(),
    };
    if (!model->described)
        return progress;
    for (uint32_t i = 0; i < model->info->imageCount; ++i) {
        PrModelLoad load = model->images[i].state.load.load();
        progress.imageCount += load != PrModelLoad_None;
        progress.imagesReady += load == PrModelLoad_Ready;
        progress.imagesFailed += load == PrModelLoad_Failed;
        progress.textureBytes += model->images[i].textureBytes.load();
    }
    for (uint32_t i = 0; i < model->info->meshCount; ++i) {
        PrModelLoad load = model->meshes[i].state.load.load();
        progress.meshCount += load != PrModelLoad_None;
        progress.meshesReady += load == PrModelLoad_Ready;
        progress.meshesFailed += load == PrModelLoad_Failed;
    }
    return progress;
}

bool prModelIsLoaded(const PrModel* model) {
    PrModelProgress progress = prModelProgress(model);
    if (progress.source == PrModelLoad_Failed)
        return true;
    return model->described && model->render != nullptr &&
        progress.imagesReady + progress.imagesFailed == progress.imageCount &&
        progress.meshesReady + progress.meshesFailed == progress.meshCount;
}

PrModelResourceInfo prModelImageInfo(const PrModel* model, PrModelImageIndex image) {
    PR_ASSERT(model->described && image < model->info->imageCount);
    PrModelResourceInfo info = infoOf(model->images[image].state);
    info.colorMismatch = model->images[image].colorMismatch.load() != 0;
    return info;
}

PrModelResourceInfo prModelMeshInfo(const PrModel* model, PrModelMeshIndex mesh) {
    PR_ASSERT(model->described && mesh < model->info->meshCount);
    return infoOf(model->meshes[mesh].state);
}

uint32_t prModelDiagnosticCount(const PrModel* model) {
    PrModel* m = const_cast<PrModel*>(model);
    prMutexLock(&m->lock);
    uint32_t count = m->diagnostics.count;
    prMutexUnlock(&m->lock);
    return count;
}

PrModelDiagnostic prModelDiagnostic(const PrModel* model, uint32_t index) {
    PrModel* m = const_cast<PrModel*>(model);
    prMutexLock(&m->lock);
    PR_ASSERT(index < m->diagnostics.count);
    PrModelDiagnostic diagnostic = m->diagnostics.data[index];
    prMutexUnlock(&m->lock);
    return diagnostic;
}

bool prModelHasDiagnostic(const PrModel* model, PrStringView feature) {
    for (uint32_t i = 0; i < prModelDiagnosticCount(model); ++i) {
        if (prModelDiagnostic(model, i).feature == feature)
            return true;
    }
    return false;
}

uint32_t prModelUnsampledTextureCount(const PrModel* model) {
    return model->unsampledTextures;
}

PrRenderMesh prModelMesh(const PrModel* model, PrModelMeshIndex mesh) {
    PR_ASSERT(model->described && mesh < model->info->meshCount);
    return model->meshes[mesh].mesh;
}

PrRenderMaterial prModelMaterial(const PrModel* model, PrModelMaterialIndex material) {
    PR_ASSERT(model->described && material < model->info->materialCount);
    return model->materials[material];
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

uint32_t prModelCameraCount(const PrModel* model) {
    return model->cameras.count;
}

PrModelNodeIndex prModelCameraNode(const PrModel* model, uint32_t index) {
    PR_ASSERT(index < model->cameras.count);
    return model->cameras.data[index];
}

uint32_t prModelLightCount(const PrModel* model) {
    return model->lights.count;
}

PrModelNodeIndex prModelLightNode(const PrModel* model, uint32_t index) {
    PR_ASSERT(index < model->lights.count);
    return model->lights.data[index];
}

PrModelNodeIndex prModelEnvironmentNode(const PrModel* model) {
    return model->described ? model->info->environment.node : kPrModelNodeNone;
}

// ---------------------------------------------------------------------------------------------------------------------
// Placements

PrModelPlacementDesc prModelPlacementDescDefault(PrRenderScene scene) {
    return PrModelPlacementDesc{
        .scene = scene,
        .root = prMatrix4Identity(),
        .visibility = 0xffffffffu,
        .addsLights = true,
    };
}

PrModelPlacement* prModelPlacementCreate(PrModel* model, const PrModelPlacementDesc* desc) {
    Model* m = model;
    PrModelPlacement* placement = allocZeroed<PrModelPlacement>();
    ModelPlacement* p = placement;
    p->model = m;
    p->desc = *desc;
    p->loop = PrModelLoop_Once;
    p->playback = ModelPlayback{kPrModelAnimationNone, 0};
    p->blend = ModelPlayback{kPrModelAnimationNone, 0};
    p->variant = kPrModelVariantNone;
    p->highlightNode = kPrModelNodeNone;
    p->poseDirty = true;
    m->placements.push(p);
    if (m->described)
        placementDescribe(p);
    enterScene(p);
    return placement;
}

void prModelPlacementDestroy(PrModelPlacement* PR_NULLABLE placement) {
    if (placement == nullptr)
        return;
    ModelPlacement* p = placement;
    leaveScene(p);
    p->model->placements.removeFirst(p);
    prMemoryFree(p->pose);
    prMemoryFree(p->blendPose);
    prMemoryFree(p->scenes);
    prMemoryFree(p->worlds);
    prMemoryFree(p->visible);
    prMemoryFree(p->blendVisible);
    prMemoryFree(p->lights);
    prMemoryFree(p->blendLights);
    prMemoryFree(p->cameras);
    prMemoryFree(p->inputPose);
    prMemoryFree(p->blendInputs);
    prMemoryFree(p->materialPlayed);
    prMemoryFree(p->blendMaterialPlayed);
    prMemoryFree(p->ownMaterials);
    prMemoryFree(p->partMaterials);
    prMemoryFree(p->shown);
    prMemoryFree(p->weights);
    prMemoryFree(p->blendWeights);
    prMemoryFree(p->trackValues);
    prMemoryFree(p->transforms);
    prMemoryFree(p->joints);
    prMemoryFree(p->instances);
    prMemoryFree(p->sceneLights);
    prMemoryFree(p->lightScales);
    prMemoryFree(placement);
}

void prModelPlacementSetScene(PrModelPlacement* placement, PrRenderScene scene) {
    ModelPlacement* p = placement;
    if (p->desc.scene.id == scene.id)
        return;
    leaveScene(p);
    p->desc.scene = scene;
    enterScene(p);
}

void prModelPlacementSetRoot(PrModelPlacement* placement, const PrMatrix4* root) {
    placement->desc.root = *root;
    placement->poseDirty = true;
}

void prModelPlacementSetVariant(PrModelPlacement* placement, PrModelVariantIndex variant) {
    PR_ASSERT(variant == kPrModelVariantNone ||
        (placement->model->described && variant < placement->model->info->variantCount));
    if (placement->variant == variant)
        return;
    placement->variant = variant;
    placement->materialsDirty = true;
    placement->sceneDirty = true;
}

PrModelVariantIndex prModelPlacementVariant(const PrModelPlacement* placement) {
    return placement->variant;
}

void prModelPlacementSetHighlight(PrModelPlacement* placement, PrModelNodeIndex node, PrRenderMaterial material) {
    PR_ASSERT(node == kPrModelNodeNone || node < prModelNodeCount(static_cast<const PrModel*>(placement->model)));
    if (placement->highlightNode == node && placement->highlight.id == material.id)
        return;
    placement->highlightNode = node;
    placement->highlight = material;
    placement->materialsDirty = true;
    placement->sceneDirty = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Animation

void prModelPlacementSetAnimation(PrModelPlacement* placement, PrModelAnimationIndex animation, PrModelLoop loop) {
    PR_ASSERT(animation == kPrModelAnimationNone ||
        animation < prModelAnimationCount(static_cast<const PrModel*>(placement->model)));
    placement->playback = ModelPlayback{animation, 0};
    placement->blend = ModelPlayback{kPrModelAnimationNone, 0};
    placement->loop = loop;
    placement->poseDirty = true;
}

PrModelAnimationIndex prModelPlacementAnimation(const PrModelPlacement* placement) {
    return placement->playback.animation;
}

float prModelPlacementTime(const PrModelPlacement* placement) {
    return placement->playback.seconds;
}

void prModelPlacementSetTime(PrModelPlacement* placement, float seconds) {
    setPlayback(placement, &placement->playback, seconds);
}

void prModelPlacementAdvance(PrModelPlacement* placement, float delta) {
    setPlayback(placement, &placement->playback, placement->playback.seconds + delta);
}

void prModelPlacementSetBlend(PrModelPlacement* placement, PrModelAnimationIndex animation, float seconds, float weight) {
    PR_ASSERT(animation == kPrModelAnimationNone ||
        animation < prModelAnimationCount(static_cast<const PrModel*>(placement->model)));
    placement->blend.animation = animation;
    placement->blendWeight = prF32Clamp(weight, 0, 1);
    setPlayback(placement, &placement->blend, seconds);
}

// ---------------------------------------------------------------------------------------------------------------------
// What a placement holds in the world

PrMatrix4 prModelPlacementNodeWorld(PrModelPlacement* placement, PrModelNodeIndex node) {
    PR_ASSERT(node < prModelNodeCount(static_cast<const PrModel*>(placement->model)));
    updatePose(placement);
    // A node outside the model's scene has no place in the world.
    return placement->model->inScene[node] ? placement->worlds[node] : placement->desc.root;
}

bool prModelPlacementBounds(PrModelPlacement* placement, PrBox3* out) {
    ModelPlacement* p = placement;
    const Model* m = p->model;
    *out = PrBox3{};
    if (!p->described)
        return false;
    const PrModelSourceInfo& info = *m->info;
    updatePose(p);
    BoxUnion bounds{};
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelDraw& draw = m->draws.data[i];
        // A hidden node draws nothing.
        if (!p->visible[draw.node])
            continue;
        const ModelMesh& mesh = m->meshes[draw.mesh];
        // The box of the mesh's parts as they were bound, and the box of what the morph targets add to a vertex of any
        // of them at the draw's weights.
        PrBox3 whole{{1, 1, 1}, {-1, -1, -1}};
        PrBox3 reach{};
        for (uint32_t k = 0; k < mesh.partCount; ++k) {
            const PrModelSourcePart& part = info.parts[mesh.firstPart + k];
            PrBox3 moved = morphBox(p, draw, part);
            reach = prBox3Union(reach, moved);
            for (uint32_t n = 0; draw.skin == kPrModelSkinNone && n < draw.instanceCount; ++n) {
                PrMatrix4 world = instanceWorld(p, draw, n);
                addBox(&bounds, PrBox3{part.bounds.min + moved.min, part.bounds.max + moved.max}, world);
            }
            if (draw.skin == kPrModelSkinNone)
                continue;
            if (whole.min.x > whole.max.x)
                whole = part.bounds;
            else
                whole = prBox3Union(whole, part.bounds);
        }
        if (draw.skin == kPrModelSkinNone)
            continue;
        // A skinned vertex lies among the places that each of its joints would put it alone, so the boxes of what each
        // joint moves, under that joint, hold the mesh. If the source does not say what a joint moves, each joint
        // counts as moving all of the mesh, which holds it too, with room to spare.
        uint32_t jointCount = skinJointCount(m, draw);
        for (uint32_t j = 0; j < jointCount; ++j) {
            PrBox3 box = mesh.jointBoxesRead && j < mesh.jointCount ? mesh.jointBoxes[j] : whole;
            if (box.min.x > box.max.x)
                continue;
            PrMatrix4 joint = jointMatrix(p, draw, j);
            for (uint32_t n = 0; n < draw.instanceCount; ++n) {
                PrMatrix4 world = instanceWorld(p, draw, n);
                world = prMatrix4Concat(&joint, &world);
                addBox(&bounds, PrBox3{box.min + reach.min, box.max + reach.max}, world);
            }
        }
    }
    *out = bounds.box;
    return bounds.found;
}

bool prModelPlacementPick(PrModelPlacement* placement, PrMeshRay ray, PrModelPick* out) {
    ModelPlacement* p = placement;
    Model* m = p->model;
    if (!p->described)
        return false;
    updatePose(p);
    bool found = false;
    for (uint32_t i = 0; i < m->draws.count; ++i) {
        const ModelDraw& draw = m->draws.data[i];
        if (!p->visible[draw.node])
            continue;
        ModelPickMesh& pick = pickMesh(m, draw.mesh);
        if (pick.indices.count == 0)
            continue;
        if (!pick.deforms) {
            PrMeshAttributeView positions{
                pick.positions.data,
                pick.positions.count / 3,
                3 * sizeof(float),
                prMeshFormatFloat32(3),
            };
            pickInstances(p, draw, pick, pick.bvh, positions, ray, out, &found);
            continue;
        }
        // A draw's instances share its shape: its skin's joints and its weights are the draw's.
        PrMeshAttributeView positions = deformedPositions(p, draw, pick);
        PrMeshBvhView bvh = deformedHierarchy(m, pick, positions);
        pickInstances(p, draw, pick, bvh, positions, ray, out, &found);
    }
    return found;
}

PrRenderCamera prModelPlacementCamera(PrModelPlacement* placement, uint32_t index, float aspect) {
    ModelPlacement* p = placement;
    const Model* m = p->model;
    PR_ASSERT(index < m->cameras.count);
    updatePose(p);
    PrModelNodeIndex node = m->cameras.data[index];
    const PrModelSourceCamera& camera = p->cameras[m->info->nodes[node].camera];
    // The view turns and moves the world and does not scale it, so the node's axes are taken at unit length.
    const PrMatrix4& world = p->worlds[node];
    PrVec3 right = unitOr(columnOf(world, 0), PrVec3{1, 0, 0});
    PrVec3 up = unitOr(columnOf(world, 1), PrVec3{0, 1, 0});
    PrVec3 back = unitOr(columnOf(world, 2), PrVec3{0, 0, 1});
    PrVec3 eye = columnOf(world, 3);
    PrRenderCamera result{};
    result.view = PrMatrix4{
        {
            {right.x, right.y, right.z, -prVec3Dot(right, eye)},
            {up.x, up.y, up.z, -prVec3Dot(up, eye)},
            {back.x, back.y, back.z, -prVec3Dot(back, eye)},
            {0, 0, 0, 1},
        },
    };
    if (!(aspect > 0))
        aspect = camera.aspectRatio > 0 ? camera.aspectRatio : 1;
    if (camera.projection == PrModelProjection_Perspective) {
        result.projection = prMatrix4PerspectiveProjection(camera.yfov, aspect, camera.znear);
    } else {
        result.projection = prMatrix4OrthographicProjection(PrVec2{camera.ymag * aspect, camera.ymag}, camera.znear,
            camera.zfar);
    }
    return result;
}

PrRenderLightDesc prModelPlacementLight(PrModelPlacement* placement, uint32_t index) {
    ModelPlacement* p = placement;
    PR_ASSERT(index < p->model->lights.count);
    updatePose(p);
    return lightDesc(p, index);
}

void prModelPlacementSetLightScale(PrModelPlacement* placement, uint32_t index, float scale) {
    ModelPlacement* p = placement;
    PR_ASSERT(index < p->model->lights.count);
    if (p->lightScales[index] == scale)
        return;
    p->lightScales[index] = scale;
    p->sceneDirty = true;
}

float prModelPlacementLightScale(const PrModelPlacement* placement, uint32_t index) {
    PR_ASSERT(index < placement->model->lights.count);
    return placement->lightScales[index];
}

bool prModelPlacementEnvironment(PrModelPlacement* placement, PrRenderSceneEnvironment* out) {
    ModelPlacement* p = placement;
    const Model* m = p->model;
    *out = prRenderSceneEnvironmentDefault(PrRenderEnvironment{});
    if (!p->described || m->info->environment.node == kPrModelNodeNone || m->environment.environment.id == 0)
        return false;
    const PrModelSourceEnvironment& environment = m->info->environment;
    updatePose(p);
    // The node's axes without their scale, made a rotation, then the frame within the node.
    const PrMatrix4& world = p->worlds[environment.node];
    PrVec3 x = unitOr(columnOf(world, 0), PrVec3{1, 0, 0});
    PrVec3 y = unitOr(columnOf(world, 1) - x * prVec3Dot(columnOf(world, 1), x), PrVec3{0, 1, 0});
    PrVec3 z = prVec3Cross(x, y);
    PrVec3 axes[3] = {x, y, z};
    out->environment = m->environment.environment;
    out->rotation = prAffine3Identity();
    for (uint32_t column = 0; column < 3; ++column) {
        PrVec3 turned = axes[0] * environment.frame.m[0][column] + axes[1] * environment.frame.m[1][column] +
            axes[2] * environment.frame.m[2][column];
        out->rotation.m[0][column] = turned.x;
        out->rotation.m[1][column] = turned.y;
        out->rotation.m[2][column] = turned.z;
    }
    out->intensity = p->visible[environment.node] ? environment.intensity : 0;
    return true;
}
