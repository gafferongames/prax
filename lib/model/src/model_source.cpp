// Model sources: the dispatch over a format's adapter, and the description's helpers.

#include "pr/model_source.h"

#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/model_source_backend.h"

void prModelMeshDataReset(PrModelMeshData* data) {
    prMemoryFree(data->positions);
    prMemoryFree(data->normals);
    prMemoryFree(data->tangents);
    for (uint32_t i = 0; data->streams != nullptr && i < data->streamCount; ++i)
        prMemoryFree(data->streams[i]);
    prMemoryFree(data->streams);
    prMemoryFree(data->streamWidths);
    prMemoryFree(data->joints);
    prMemoryFree(data->weights);
    prMemoryFree(data->morphPositions);
    prMemoryFree(data->morphNormals);
    prMemoryFree(data->morphTangents);
    prMemoryFree(data->triangles);
    prMemoryFree(data->parts);
    *data = PrModelMeshData{};
}

PrModelChannelMap prModelChannelMapIdentity(void) {
    PrModelChannelMap map{};
    for (uint32_t k = 0; k < 4; ++k)
        map.weights[k][k] = 1;
    return map;
}

PrModelSourceInput prModelSourceInputValue(PrStringView name, uint32_t width, const float* value) {
    PrModelSourceInput input{
        .name = name,
        .kind = PrModelInputKind_Value,
        .width = uint8_t(width),
        .texture = kPrModelTextureNone,
        .scale = {1, 1},
        .transform = {.a = 1, .d = 1},
        .channels = prModelChannelMapIdentity(),
    };
    for (uint32_t k = 0; k < width && k < 4; ++k)
        input.value[k] = value[k];
    return input;
}

namespace pr_impl {

namespace {

// Appends the fields of the description's records in their canonical text: integers in decimal, floats as the
// hexadecimal of their bits, and strings as their length and their bytes.
struct InfoWriter {
    PrTextBuffer* out;

    void u(uint64_t value) {
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppendU64(out, value, 10);
    }

    void f(const float* values, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t bits = 0;
            prMemCopy(&bits, &values[i], sizeof(bits));
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppendU64(out, bits, 16);
        }
    }

    void f(float value) { f(&value, 1); }

    void m(const PrMatrix4& matrix) { f(&matrix.m[0][0], 16); }

    void box(const PrBox3& box) {
        const float values[6] = {box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z};
        f(values, 6);
    }

    void s(PrStringView text) {
        u(text.size);
        prTextBufferAppendChar(out, ':');
        prTextBufferAppend(out, text);
    }

    void line(PrStringView kind, uint32_t index) {
        prTextBufferAppendChar(out, '\n');
        prTextBufferAppend(out, kind);
        u(index);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prModelSourceInfoWrite(const PrModelSourceInfo* info, PrTextBuffer* out) {
    InfoWriter w{out};
    for (uint32_t i = 0; i < info->nodeCount; ++i) {
        const PrModelSourceNode& n = info->nodes[i];
        w.line("node", i);
        w.s(n.name);
        w.u(n.parent);
        w.u(n.flags);
        w.u(n.firstChild);
        w.u(n.childCount);
        w.f(n.translation, 3);
        w.f(n.rotation, 4);
        w.f(n.scale, 3);
        w.m(n.matrix);
        w.u(n.camera);
        w.u(n.light);
        w.u(n.firstWeight);
        w.u(n.weightCount);
    }
    for (uint32_t i = 0; i < info->rootCount; ++i) {
        w.line("root", i);
        w.u(info->roots[i]);
    }
    for (uint32_t i = 0; i < info->childCount; ++i) {
        w.line("child", i);
        w.u(info->children[i]);
    }
    for (uint32_t i = 0; i < info->drawCount; ++i) {
        const PrModelSourceDraw& d = info->draws[i];
        w.line("draw", i);
        w.u(d.node);
        w.u(d.mesh);
        w.u(d.skin);
        w.u(d.firstInstance);
        w.u(d.instanceCount);
    }
    for (uint32_t i = 0; i < info->instanceCount; ++i) {
        w.line("instance", i);
        w.m(info->instances[i]);
    }
    for (uint32_t i = 0; i < info->meshCount; ++i) {
        const PrModelSourceMesh& mesh = info->meshes[i];
        w.line("mesh", i);
        w.s(mesh.name);
        w.u(mesh.firstPart);
        w.u(mesh.partCount);
        w.u(mesh.morphTargetCount);
        w.u(mesh.firstWeight);
        w.u(mesh.weightCount);
    }
    for (uint32_t i = 0; i < info->partCount; ++i) {
        const PrModelSourcePart& p = info->parts[i];
        w.line("part", i);
        w.u(p.material);
        w.box(p.bounds);
        w.u(p.firstMorphBox);
        w.u(p.morphTargetCount);
        w.u(p.firstStream);
        w.u(p.streamCount);
        w.u(p.firstVariant);
        w.u(p.variantCount);
    }
    for (uint32_t i = 0; i < info->streamCount; ++i) {
        const PrModelSourceStream& stream = info->streams[i];
        w.line("stream", i);
        w.s(stream.name);
        w.u(stream.semantic);
        w.u(stream.set);
        w.u(stream.format.component);
        w.u(stream.format.count);
        w.u(stream.format.normalized);
    }
    for (uint32_t i = 0; i < info->morphBoxCount; ++i) {
        w.line("morphBox", i);
        w.box(info->morphBoxes[i]);
    }
    for (uint32_t i = 0; i < info->weightCount; ++i) {
        w.line("weight", i);
        w.f(info->weights[i]);
    }
    for (uint32_t i = 0; i < info->skinCount; ++i) {
        w.line("skin", i);
        w.u(info->skins[i].firstJoint);
        w.u(info->skins[i].jointCount);
    }
    for (uint32_t i = 0; i < info->jointCount; ++i) {
        w.line("joint", i);
        w.u(info->joints[i]);
        w.m(info->inverseBinds[i]);
    }
    for (uint32_t i = 0; i < info->materialCount; ++i) {
        const PrModelSourceMaterial& material = info->materials[i];
        w.line("material", i);
        w.s(material.name);
        w.u(material.shading);
        w.u(material.alpha);
        w.f(material.alphaCutoff);
        w.u(material.doubleSided);
        w.u(material.firstInput);
        w.u(material.inputCount);
    }
    for (uint32_t i = 0; i < info->inputCount; ++i) {
        const PrModelSourceInput& input = info->inputs[i];
        w.line("input", i);
        w.s(input.name);
        w.u(input.kind);
        w.u(input.width);
        w.f(input.value, 4);
        w.u(input.texture);
        w.u(input.texCoordSet);
        w.f(input.offset, 2);
        w.f(input.rotation);
        w.f(input.scale, 2);
        const float transform[6] = {
            input.transform.a, input.transform.b, input.transform.c, input.transform.d,
            input.transform.e, input.transform.f,
        };
        w.f(transform, 6);
        w.s(input.attribute);
        w.s(input.tint);
        w.f(&input.channels.weights[0][0], 16);
        w.f(input.channels.offset, 4);
    }
    for (uint32_t i = 0; i < info->textureCount; ++i) {
        const PrModelSourceTexture& texture = info->textures[i];
        w.line("texture", i);
        w.u(texture.firstImage);
        w.u(texture.imageCount);
        w.u(texture.sampler.magnify);
        w.u(texture.sampler.minify);
        w.u(texture.sampler.mip);
        w.u(texture.sampler.wrap[0]);
        w.u(texture.sampler.wrap[1]);
        w.u(texture.color);
    }
    for (uint32_t i = 0; i < info->textureImageCount; ++i) {
        w.line("textureImage", i);
        w.u(info->textureImages[i]);
    }
    for (uint32_t i = 0; i < info->imageCount; ++i) {
        const PrModelSourceImage& image = info->images[i];
        w.line("image", i);
        w.s(image.name);
        w.s(image.mediaType);
        w.s(image.location);
    }
    for (uint32_t i = 0; i < info->variantCount; ++i) {
        w.line("variant", i);
        w.s(info->variants[i]);
    }
    for (uint32_t i = 0; i < info->variantMaterialCount; ++i) {
        w.line("variantMaterial", i);
        w.u(info->variantMaterials[i].variant);
        w.u(info->variantMaterials[i].material);
    }
    for (uint32_t i = 0; i < info->cameraCount; ++i) {
        const PrModelSourceCamera& camera = info->cameras[i];
        w.line("camera", i);
        w.s(camera.name);
        w.u(camera.projection);
        const float values[6] = {camera.yfov, camera.aspectRatio, camera.xmag, camera.ymag, camera.znear, camera.zfar};
        w.f(values, 6);
    }
    for (uint32_t i = 0; i < info->lightCount; ++i) {
        const PrModelSourceLight& light = info->lights[i];
        w.line("light", i);
        w.s(light.name);
        w.u(light.kind);
        w.u(light.unit);
        const float values[11] = {
            light.color.x, light.color.y, light.color.z, light.intensity, light.range,
            light.innerConeAngle, light.outerConeAngle, light.radius, light.width, light.height, light.length,
        };
        w.f(values, 11);
        w.u(light.normalized);
        w.u(light.treatAsPoint);
    }
    const PrModelSourceEnvironment& environment = info->environment;
    w.line("environment", 0);
    w.u(environment.node);
    w.u(environment.image);
    const float radiance[4] = {environment.color.x, environment.color.y, environment.color.z, environment.intensity};
    w.f(radiance, 4);
    w.m(environment.frame);
    for (uint32_t i = 0; i < info->animationCount; ++i) {
        const PrModelSourceAnimation& animation = info->animations[i];
        w.line("animation", i);
        w.s(animation.name);
        w.f(animation.duration);
        w.u(animation.firstTrack);
        w.u(animation.trackCount);
    }
    for (uint32_t i = 0; i < info->trackCount; ++i) {
        const PrModelSourceTrack& track = info->tracks[i];
        w.line("track", i);
        w.u(track.target);
        w.u(track.interpolation);
        w.u(track.object);
        w.u(track.element);
        w.u(track.width);
        w.u(track.firstKey);
        w.u(track.keyCount);
        w.u(track.firstValue);
    }
    w.line("keyTimes", info->keyTimeCount);
    w.f(info->keyTimes, info->keyTimeCount);
    w.line("keyValues", info->keyValueCount);
    w.f(info->keyValues, info->keyValueCount);
    for (uint32_t i = 0; i < info->diagnosticCount; ++i) {
        w.line("diagnostic", i);
        w.s(info->diagnostics[i].feature);
        w.s(info->diagnostics[i].detail);
    }
    prTextBufferAppendChar(out, '\n');
}

void prModelSourceDestroy(PrModelSource* PR_NULLABLE source) {
    if (source != nullptr)
        source->destroyFn(source);
}

const PrModelSourceInfo* prModelSourceInfo(const PrModelSource* source) {
    return source->infoFn(source);
}

PrStatus prModelSourceMeshRead(const PrModelSource* source, PrModelMeshIndex mesh, const PrModelMeshRequest* request,
        PrModelMeshData* out) {
    *out = PrModelMeshData{};
    PrStatus status = source->meshReadFn(source, mesh, request, out);
    if (status != PrStatus_Ok)
        prModelMeshDataReset(out);
    return status;
}

PrStatus prModelSourceImageOpen(const PrModelSource* source, PrModelImageIndex image, PrModelImageData* out) {
    *out = PrModelImageData{};
    if (source->imageOpenFn == nullptr)
        return PrStatus_Io;
    PrStatus status = source->imageOpenFn(source, image, out);
    if (status != PrStatus_Ok) {
        prBlobSourceRelease(out->bytes);
        prTextureSourceRelease(out->source);
        *out = PrModelImageData{};
    }
    return status;
}

void prModelSourceNodeAddress(const PrModelSource* source, PrModelNodeIndex node, PrTextBuffer* out) {
    if (source->nodeAddressFn != nullptr)
        source->nodeAddressFn(source, node, out);
}

PrModelNodeIndex prModelSourceNodeFromAddress(const PrModelSource* source, PrStringView address) {
    if (source->nodeFromAddressFn == nullptr)
        return kPrModelNodeNone;
    return source->nodeFromAddressFn(source, address);
}
