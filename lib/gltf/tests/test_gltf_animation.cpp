// Animations and their evaluation.

#include "pr/float.h"

#include "gltf/tests/gltf_fixture.h"

namespace pr_impl {

namespace {

constexpr float kHalfRoot = 0.70710678f;

// Returns the buffer of gltfQuad with keyframes at 0, 1, 2 and 3 seconds, whose values, at byte 96, step from the
// origin along x, then y, then z.
GltfBin keyframeBin() {
    GltfBin bin;
    bin.put<uint16_t>(64, 0, 1, 2, 2, 1, 3);
    bin.put<float>(48, 0, 1, 2, 3);
    bin.put<float>(96, 0, 0, 0, 10, 0, 0, 10, 20, 0, 10, 20, 30);
    return bin;
}

// Returns the JSON of the quad with two nodes and one animation, whose members are `animation`.
String oneAnimation(PrStringView animation) {
    String members = R"("nodes":[{},{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}],"animations":[{)";
    return gltfQuad(members.append(animation).append("}]"));
}

// Returns the JSON of the quad's animation with one sampler and one channel, whose members are `sampler` and `channel`.
String oneChannel(PrStringView sampler, PrStringView channel) {
    String animation = R"("samplers":[{)";
    animation.append(sampler).append(R"(}],"channels":[{)").append(channel).append("}]");
    return oneAnimation(animation);
}

// The value of a channel at a time, as up to four floats.
struct Value {
    float v[4];
};

Value valueAt(const PrGltf* gltf, uint32_t channel, float seconds) {
    Value value = {{-99, -99, -99, -99}};
    CHECK(prGltfAnimationChannelEvaluate(gltf, channel, seconds, value.v));
    return value;
}

// Returns true if the first `count` floats of `value` are `x`, `y`, `z` and `w`, to within the error of a few float
// operations.
bool isNear(const Value& value, uint32_t count, float x, float y, float z = 0, float w = 0) {
    const float expected[4] = {x, y, z, w};
    for (uint32_t i = 0; i < count; ++i) {
        if (!(prF32Abs(value.v[i] - expected[i]) < 1e-5f))
            return false;
    }
    return true;
}

// The accessors of the pointer tests, over a buffer whose values pointerBin writes:
//
//   0  SCALAR floats, 2 at byte 0: the times of two keyframes, 0 and 1, with bounds
//   1  SCALAR floats, 2 at byte 8
//   2  VEC2 floats, 2 at byte 16
//   3  VEC3 floats, 2 at byte 32
//   4  VEC4 floats, 2 at byte 56
//   5  SCALAR unsigned bytes, 2 at byte 88: false, then true
//   6  VEC3 floats, 3 at byte 96: the positions of a triangle, with bounds
//   7  SCALAR floats, 4 at byte 136: the weights of two morph targets at two keyframes
//   8  SCALAR unsigned shorts, 2 at byte 152
constexpr PrStringView kPointerAccessors =
    R"("accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
    R"({"bufferView":0,"byteOffset":8,"componentType":5126,"count":2,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":16,"componentType":5126,"count":2,"type":"VEC2"},)"
    R"({"bufferView":0,"byteOffset":32,"componentType":5126,"count":2,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":56,"componentType":5126,"count":2,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":88,"componentType":5121,"count":2,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":136,"componentType":5126,"count":4,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":152,"componentType":5123,"count":2,"type":"SCALAR"}])";

GltfBin pointerBin() {
    GltfBin bin;
    bin.put<float>(0, 0, 1);
    bin.put<float>(8, 0.25f, 0.75f);
    bin.put<float>(16, 0, 0, 1, 2);
    bin.put<float>(32, 0, 0, 0, 1, 1, 1);
    bin.put<float>(56, 0, 0, 0, 0, 1, 1, 1, 1);
    bin.put<uint8_t>(88, 0, 1);
    bin.put<float>(96, 0, 0, 0, 1, 0, 0, 0, 1, 0);
    bin.put<float>(136, 0, 1, 1, 0);
    bin.put<uint16_t>(152, 2, 4);
    return bin;
}

// The objects that the pointer tests animate: two nodes, the first with a mesh of two morph targets, a mesh, a
// perspective camera with an aspect ratio and no far plane, an orthographic camera, a spot light and a point light, and
// two materials, the first with a clearcoat and a transformed base color texture.
constexpr PrStringView kPointerObjects =
    R"("extensionsUsed":["KHR_animation_pointer","KHR_lights_punctual","KHR_materials_clearcoat",)"
    R"("KHR_texture_transform","KHR_node_visibility"],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":6},"targets":[{"POSITION":6},{"POSITION":6}]}]}],)"
    R"("nodes":[{"mesh":0},{}],)"
    R"("cameras":[{"type":"perspective","perspective":{"yfov":1,"znear":0.1,"aspectRatio":1.5}},)"
    R"({"type":"orthographic","orthographic":{"xmag":1,"ymag":1,"znear":0,"zfar":10}}],)"
    R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"spot","spot":{}},{"type":"point"}]}},)"
    R"("images":[{"uri":"a.png"}],"textures":[{"source":0}],)"
    R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,)"
    R"("extensions":{"KHR_texture_transform":{}}}},"extensions":{"KHR_materials_clearcoat":{}}},{}])";

// Returns an asset of the pointer tests' objects with one animation of one sampler, whose members are `sampler`, and
// one channel for each pointer of `pointers`.
String pointerAsset(PrStringView sampler, Span<const PrStringView> pointers) {
    String members = kPointerAccessors;
    members.append(",").append(kPointerObjects).append(R"(,"animations":[{"samplers":[{)").append(sampler);
    members.append(R"(}],"channels":[)");
    for (uint32_t i = 0; i < pointers.count(); ++i) {
        members.append(i == 0 ? "" : ",").append(R"({"sampler":0,"target":{"path":"pointer","extensions":)");
        members.append(R"({"KHR_animation_pointer":{"pointer":")").append(pointers[i]).append(R"("}}}})");
    }
    return gltfBinAsset(members.append("]}]"));
}

String pointerAsset(PrStringView sampler, PrStringView pointer) {
    return pointerAsset(sampler, Span<const PrStringView>(&pointer, 1));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an animation is channels that apply samplers to the properties of nodes") {
    GltfBin bin = keyframeBin();
    GltfRead read(oneAnimation(R"("name":"move","samplers":[{"input":6,"output":2},)"
        R"({"input":9,"output":5,"interpolation":"STEP"}],"channels":[)"
        R"({"sampler":0,"target":{"node":0,"path":"translation"}},)"
        R"({"sampler":0,"target":{"node":0,"path":"scale"}}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfAnimationCount(read.gltf) == 1);
    const PrGltfAnimation& animation = prGltfAnimations(read.gltf)[0];
    CHECK(animation.name == "move");
    CHECK(animation.samplerFirst == 0);
    CHECK(animation.samplerCount == 2);
    CHECK(animation.channelFirst == 0);
    CHECK(animation.channelCount == 2);
    // The last keyframe that a channel uses is at 3 seconds, as the bounds of the times say.
    CHECK(animation.duration == 3);

    REQUIRE(prGltfAnimationSamplerCount(read.gltf) == 2);
    const PrGltfAnimationSampler* samplers = prGltfAnimationSamplers(read.gltf);
    CHECK(samplers[0].input == 6);
    CHECK(samplers[0].output == 2);
    CHECK(samplers[0].interpolation == PrGltfInterpolation_Linear);
    CHECK(samplers[1].interpolation == PrGltfInterpolation_Step);

    REQUIRE(prGltfAnimationChannelCount(read.gltf) == 2);
    const PrGltfAnimationChannel* channels = prGltfAnimationChannels(read.gltf);
    CHECK(channels[0].sampler == 0);
    CHECK(channels[0].node == 0);
    CHECK(channels[0].path == PrGltfPath_Translation);
    CHECK(channels[0].width == 3);
    CHECK(channels[1].path == PrGltfPath_Scale);
}

TEST_CASE("the samplers of a second animation follow those of the first") {
    GltfBin bin = keyframeBin();
    GltfRead read(gltfQuad(R"("nodes":[{}],"animations":[)"
        R"({"samplers":[{"input":6,"output":2}],"channels":[{"sampler":0,"target":{"node":0,"path":"scale"}}]},)"
        R"({"samplers":[{"input":9,"output":2},{"input":6,"output":2,"interpolation":"STEP"}],)"
        R"("channels":[{"sampler":1,"target":{"node":0,"path":"translation"}}]}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfAnimation& second = prGltfAnimations(read.gltf)[1];
    CHECK(second.samplerFirst == 1);
    CHECK(second.samplerCount == 2);
    CHECK(second.channelFirst == 1);
    // A channel names its sampler among the document's, not the animation's.
    const PrGltfAnimationChannel& channel = prGltfAnimationChannels(read.gltf)[1];
    CHECK(channel.sampler == 2);
    CHECK(prGltfAnimationSamplers(read.gltf)[channel.sampler].interpolation == PrGltfInterpolation_Step);
    // The sampler that no channel uses does not count toward the duration.
    CHECK(second.duration == 3);
}

TEST_CASE("linear interpolation blends two keyframes, and the ends hold their keyframes") {
    GltfBin bin = keyframeBin();
    GltfRead read(oneChannel(R"("input":6,"output":2)", R"("sampler":0,"target":{"node":0,"path":"translation"})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(isNear(valueAt(read.gltf, 0, 0), 3, 0, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 0.5f), 3, 5, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 1), 3, 10, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 1.25f), 3, 10, 5, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 2.5f), 3, 10, 20, 15));
    CHECK(isNear(valueAt(read.gltf, 0, 3), 3, 10, 20, 30));
    // Before the first keyframe and after the last, the value is that keyframe's.
    CHECK(isNear(valueAt(read.gltf, 0, -1), 3, 0, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 7), 3, 10, 20, 30));
    CHECK(isNear(valueAt(read.gltf, 0, kPrF32Infinity), 3, 10, 20, 30));
    CHECK(isNear(valueAt(read.gltf, 0, kPrF32Nan), 3, 0, 0, 0));
    // Only the channel's width is written.
    CHECK(valueAt(read.gltf, 0, 1).v[3] == -99);
}

TEST_CASE("step interpolation holds a keyframe until the next") {
    GltfBin bin = keyframeBin();
    GltfRead read(oneChannel(R"("input":6,"output":2,"interpolation":"STEP")",
        R"("sampler":0,"target":{"node":0,"path":"scale"})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(isNear(valueAt(read.gltf, 0, 0.5f), 3, 0, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 1), 3, 10, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 1.99f), 3, 10, 0, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 2), 3, 10, 20, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 9), 3, 10, 20, 30));
}

TEST_CASE("a rotation interpolates spherically along the shorter arc") {
    GltfBin bin = keyframeBin();
    // No turn, a quarter turn about z, the same turn with its quaternion negated, and a half turn about z.
    bin.put<float>(192, 0, 0, 0, 1, 0, 0, kHalfRoot, kHalfRoot, 0, 0, -kHalfRoot, -kHalfRoot, 0, 0, 1, 0);
    GltfRead read(oneChannel(R"("input":6,"output":5)", R"("sampler":0,"target":{"node":0,"path":"rotation"})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfAnimationChannels(read.gltf)[0].width == 4);
    // Halfway to a quarter turn is an eighth of a turn, not the midpoint of the two quaternions.
    CHECK(isNear(valueAt(read.gltf, 0, 0.5f), 4, 0, 0, 0.38268343f, 0.92387953f));
    // The negated quaternion is the same rotation, so nothing turns on the way to it.
    CHECK(isNear(valueAt(read.gltf, 0, 1.5f), 4, 0, 0, kHalfRoot, kHalfRoot));
    CHECK(isNear(valueAt(read.gltf, 0, 2), 4, 0, 0, -kHalfRoot, -kHalfRoot));
    // From the negated quarter turn, the half turn is an eighth of a turn away by the shorter arc.
    CHECK(isNear(valueAt(read.gltf, 0, 2.5f), 4, 0, 0, -0.92387953f, -0.38268343f));
    CHECK(isNear(valueAt(read.gltf, 0, 3), 4, 0, 0, 1, 0));
}

TEST_CASE("a rotation is a unit quaternion, whatever the keyframes hold") {
    GltfBin bin = keyframeBin();
    bin.put<float>(192, 0, 0, 0, 2, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    GltfRead read(oneChannel(R"("input":6,"output":5,"interpolation":"STEP")",
        R"("sampler":0,"target":{"node":0,"path":"rotation"})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(isNear(valueAt(read.gltf, 0, 0), 4, 0, 0, 0, 1));
    CHECK(isNear(valueAt(read.gltf, 0, 1), 4, 0, 1, 0, 0));
    // A quaternion of no length reads as no rotation.
    CHECK(isNear(valueAt(read.gltf, 0, 2), 4, 0, 0, 0, 1));
}

TEST_CASE("a rotation of normalized integers interpolates as the floats they stand for") {
    GltfBin bin;
    bin.put<float>(0, 0, 1);
    bin.put<int16_t>(8, 0, 0, 0, 32767, 0, 0, 32767, 0);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
        R"({"bufferView":0,"byteOffset":8,"componentType":5122,"normalized":true,"count":2,"type":"VEC4"}],)"
        R"("nodes":[{}],"animations":[{"samplers":[{"input":0,"output":1}],)"
        R"("channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}]}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(isNear(valueAt(read.gltf, 0, 0.5f), 4, 0, 0, kHalfRoot, kHalfRoot));
}

TEST_CASE("a cubic spline follows its keyframes' values and tangents") {
    GltfBin bin;
    // Keyframes at 0 and 2 seconds, each an in-tangent, a value and an out-tangent. The 9s are the tangents that no
    // segment uses.
    bin.put<float>(0, 0, 2);
    bin.put<float>(8, 9, 9, 9, 0, 1, 0, 1, 0, 0);
    bin.put<float>(44, 3, 0, 0, 4, 1, 2, 9, 9, 9);
    // The same for a rotation, from no turn to a half turn about z, with no tangents.
    bin.put<float>(80, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0);
    bin.put<float>(128, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"byteOffset":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},)"
        R"({"bufferView":0,"byteOffset":8,"componentType":5126,"count":6,"type":"VEC3"},)"
        R"({"bufferView":0,"byteOffset":80,"componentType":5126,"count":6,"type":"VEC4"}],)"
        R"("nodes":[{}],"animations":[{"samplers":[{"input":0,"output":1,"interpolation":"CUBICSPLINE"},)"
        R"({"input":0,"output":2,"interpolation":"CUBICSPLINE"}],"channels":[)"
        R"({"sampler":0,"target":{"node":0,"path":"translation"}},)"
        R"({"sampler":1,"target":{"node":0,"path":"rotation"}}]}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfAnimationSamplers(read.gltf)[0].interpolation == PrGltfInterpolation_CubicSpline);
    CHECK(prGltfAnimations(read.gltf)[0].duration == 2);

    // At a keyframe the value is the keyframe's own, whatever its tangents.
    CHECK(isNear(valueAt(read.gltf, 0, 0), 3, 0, 1, 0));
    CHECK(isNear(valueAt(read.gltf, 0, 2), 3, 4, 1, 2));
    CHECK(isNear(valueAt(read.gltf, 0, 5), 3, 4, 1, 2));
    CHECK(isNear(valueAt(read.gltf, 0, -5), 3, 0, 1, 0));
    // Between them it is the Hermite spline, with each tangent scaled by the two seconds of the segment. Halfway, x is
    // 1/8 of the out-tangent 1 times 2, plus half of 4, less 1/8 of the in-tangent 3 times 2.
    CHECK(isNear(valueAt(read.gltf, 0, 1), 3, 1.5f, 1, 1));
    CHECK(isNear(valueAt(read.gltf, 0, 0.5f), 3, 0.625f, 1, 0.3125f));

    // A rotation's spline is normalized: halfway between the two, the components are equal.
    CHECK(isNear(valueAt(read.gltf, 1, 1), 4, 0, 0, kHalfRoot, kHalfRoot));
}

TEST_CASE("weights animate every morph target of the node's mesh") {
    GltfBin bin = keyframeBin();
    // Two keyframes at byte 48, at 0 and 1 seconds, and at the same bytes, as four weights: 0 and 1, then 1 and 0.
    bin.put<float>(48, 0, 1, 1, 0);
    const char* mesh = R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,)"
        R"("targets":[{"POSITION":0},{"POSITION":0}]}]}],"nodes":[{"mesh":0}],)";
    auto asset = [&](PrStringView interpolation) {
        String members = mesh;
        members.append(R"("animations":[{"samplers":[{"input":9,"output":10,"interpolation":")");
        members.append(interpolation);
        members.append(R"("}],"channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}])");
        return gltfQuad(members);
    };
    GltfRead linear(asset("LINEAR"), &bin);
    REQUIRE(linear.status == PrStatus_Ok);
    CHECK(prGltfAnimationChannels(linear.gltf)[0].path == PrGltfPath_Weights);
    CHECK(prGltfAnimationChannels(linear.gltf)[0].width == 2);
    CHECK(isNear(valueAt(linear.gltf, 0, 0), 2, 0, 1));
    CHECK(isNear(valueAt(linear.gltf, 0, 0.25f), 2, 0.25f, 0.75f));
    CHECK(isNear(valueAt(linear.gltf, 0, 1), 2, 1, 0));
    CHECK(valueAt(linear.gltf, 0, 0.25f).v[2] == -99);

    GltfRead step(asset("STEP"), &bin);
    REQUIRE(step.status == PrStatus_Ok);
    CHECK(isNear(valueAt(step.gltf, 0, 0.75f), 2, 0, 1));
}

TEST_CASE("the weights of many morph targets interpolate together") {
    // A mesh with 70 morph targets. The weights of the two keyframes are zeros, except that sparse elements give the
    // last target a weight of 7 and then of 9.
    GltfBin bin = keyframeBin();
    bin.put<float>(48, 0, 1);
    bin.put<uint8_t>(240, 69, 139);
    bin.put<float>(244, 7, 9);
    String members = R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":[)";
    for (uint32_t i = 0; i < 70; ++i)
        members.append(i == 0 ? R"({"POSITION":0})" : R"(,{"POSITION":0})");
    members.append(R"(]}]}],"nodes":[{"mesh":0}],"animations":[{"samplers":[{"input":9,"output":12}],)"
        R"("channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}])");
    String accessors = kGltfQuadAccessors;
    accessors.replace(accessors.count() - 1, 1, R"(,{"componentType":5126,"count":140,"type":"SCALAR","sparse":)"
        R"({"count":2,"indices":{"bufferView":0,"byteOffset":240,"componentType":5121},)"
        R"("values":{"bufferView":0,"byteOffset":244}}}])");
    GltfRead read(gltfBinAsset(accessors.append(",").append(members)), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfAnimationChannels(read.gltf)[0].width == 70);
    float weights[70];
    CHECK(prGltfAnimationChannelEvaluate(read.gltf, 0, 0.5f, weights));
    CHECK(weights[0] == 0);
    CHECK(weights[63] == 0);
    CHECK(weights[64] == 0);
    CHECK(weights[69] == 8);
}

TEST_CASE("a channel with no node animates nothing") {
    GltfBin bin = keyframeBin();
    GltfRead read(oneAnimation(R"("samplers":[{"input":6,"output":2}],"channels":[)"
        R"({"sampler":0,"target":{"path":"pointer"}}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfAnimationChannel& channel = prGltfAnimationChannels(read.gltf)[0];
    CHECK(channel.node == kPrGltfNone);
    CHECK(channel.path == PrGltfPath_None);
    CHECK(channel.width == 0);
    CHECK(prGltfAnimations(read.gltf)[0].duration == 0);
    float untouched = 5;
    CHECK(prGltfAnimationChannelEvaluate(read.gltf, 0, 1, &untouched));
    CHECK(untouched == 5);
}

TEST_CASE("a channel whose buffer has no bytes does not evaluate") {
    GltfRead read(oneChannel(R"("input":6,"output":2)", R"("sampler":0,"target":{"node":0,"path":"translation"})"));
    REQUIRE(read.status == PrStatus_Ok);
    float value[3] = {1, 1, 1};
    CHECK(!prGltfAnimationChannelEvaluate(read.gltf, 0, 1, value));
    CHECK(value[0] == 0);
    CHECK(value[2] == 0);
}

TEST_CASE("an animation's samplers are checked") {
    GltfBin bin = keyframeBin();
    const char* target = R"("sampler":0,"target":{"node":0,"path":"translation"})";
    auto refusal = [&](PrStringView sampler) { return gltfRefusal(oneChannel(sampler, target), &bin); };
    CHECK(gltfRefusal(oneAnimation(""), &bin) == "animations[0] must have channels and samplers");
    CHECK(gltfRefusal(oneAnimation(R"("samplers":[{"input":6,"output":2}])"), &bin) ==
        "animations[0] must have channels and samplers");
    CHECK(refusal(R"("output":2)") == "animations[0].samplers[0].input is required");
    CHECK(refusal(R"("input":6)") == "animations[0].samplers[0].output is required");
    CHECK(refusal(R"("input":6,"output":12)") == "animations[0].samplers[0].output is not the index of an object");
    CHECK(refusal(R"("input":2,"output":2)") == "animations[0].samplers[0].input must be a SCALAR accessor of floats");
    CHECK(refusal(R"("input":7,"output":2)") == "animations[0].samplers[0].input must be a SCALAR accessor of floats");
    CHECK(refusal(R"("input":10,"output":2)") == "animations[0].samplers[0].input must be an accessor with min and max");
    CHECK(refusal(R"("input":6,"output":2,"interpolation":"SMOOTH")") ==
        "animations[0].samplers[0].interpolation is not a value of its enumeration");
    CHECK(refusal(R"("input":11,"output":2,"interpolation":"CUBICSPLINE")") ==
        "animations[0].samplers[0].input must have at least two keyframes for a cubic spline");

    // The times of the keyframes increase strictly from zero or more.
    const char* disordered = "animations[0].samplers[0].input has times that do not increase strictly from zero or more";
    bin.put<float>(48, 0, 1, 1, 3);
    CHECK(refusal(R"("input":6,"output":2)") == disordered);
    bin.put<float>(48, 0, 2, 1, 3);
    CHECK(refusal(R"("input":6,"output":2)") == disordered);
    bin.put<float>(48, -0.5f, 1, 2, 3);
    CHECK(refusal(R"("input":6,"output":2)") == disordered);
    bin.put<float>(48, 0, 1, kPrF32Nan, 3);
    CHECK(refusal(R"("input":6,"output":2)") == disordered);
    bin.put<float>(48, 0, 1, 2, kPrF32Infinity);
    CHECK(refusal(R"("input":6,"output":2)") == disordered);
}

TEST_CASE("an animation's channels are checked against their nodes and samplers") {
    GltfBin bin = keyframeBin();
    auto refusal = [&](PrStringView sampler, PrStringView channel) {
        return gltfRefusal(oneChannel(sampler, channel), &bin);
    };
    const char* vectors = R"("input":6,"output":2)";
    CHECK(refusal(vectors, R"("target":{"node":0,"path":"translation"})") ==
        "animations[0].channels[0].sampler is required");
    CHECK(refusal(vectors, R"("sampler":1,"target":{"node":0,"path":"translation"})") ==
        "animations[0].channels[0].sampler is not the index of an object");
    CHECK(refusal(vectors, R"("sampler":0)") == "animations[0].channels[0].target is required");
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":0})") ==
        "animations[0].channels[0].target.path is required");
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":2,"path":"translation"})") ==
        "animations[0].channels[0].target.node is not the index of an object");
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":0,"path":"color"})") ==
        "animations[0].channels[0].target.path is not a value of its enumeration");

    const char* mistyped = "animations[0].channels[0].sampler has an output accessor of a type that the path does not allow";
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":0,"path":"rotation"})") == mistyped);
    CHECK(refusal(R"("input":6,"output":5)", R"("sampler":0,"target":{"node":0,"path":"translation"})") == mistyped);
    CHECK(refusal(R"("input":6,"output":5)", R"("sampler":0,"target":{"node":0,"path":"scale"})") == mistyped);
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":0,"path":"weights"})") == mistyped);
    // A rotation of integers must be normalized.
    CHECK(refusal(R"("input":6,"output":4)", R"("sampler":0,"target":{"node":0,"path":"rotation"})") == mistyped);

    // Node 1 has a matrix, which no channel animates, and node 0 has no morph targets.
    const char* fixed = "animations[0].channels[0].target names a property that its node cannot animate";
    CHECK(refusal(vectors, R"("sampler":0,"target":{"node":1,"path":"translation"})") == fixed);
    CHECK(refusal(R"("input":6,"output":5)", R"("sampler":0,"target":{"node":1,"path":"rotation"})") == fixed);
    CHECK(refusal(R"("input":6,"output":10)", R"("sampler":0,"target":{"node":0,"path":"weights"})") == fixed);

    const char* miscounted =
        "animations[0].channels[0].sampler has an output accessor whose count does not match its keyframes";
    CHECK(refusal(R"("input":9,"output":2)", R"("sampler":0,"target":{"node":0,"path":"translation"})") == miscounted);
    // A cubic spline holds three values for each keyframe.
    CHECK(refusal(R"("input":6,"output":2,"interpolation":"CUBICSPLINE")",
        R"("sampler":0,"target":{"node":0,"path":"translation"})") == miscounted);

    CHECK(gltfRefusal(oneAnimation(R"("samplers":[{"input":6,"output":2}],"channels":[)"
        R"({"sampler":0,"target":{"node":0,"path":"translation"}},)"
        R"({"sampler":0,"target":{"node":0,"path":"scale"}},)"
        R"({"sampler":0,"target":{"node":0,"path":"translation"}}])"), &bin) ==
        "animations[0].channels animate one property twice");
}

// ---------------------------------------------------------------------------------------------------------------------
// KHR_animation_pointer

TEST_CASE("a pointer to a node's transform or weights reads as the core's target") {
    GltfBin bin = pointerBin();
    const PrStringView pointers[] = {"/nodes/0/translation", "/nodes/0/rotation", "/nodes/1/scale"};
    GltfRead vectors(pointerAsset(R"("input":0,"output":3)", Span<const PrStringView>(&pointers[0], 1)), &bin);
    REQUIRE(vectors.status == PrStatus_Ok);
    const PrGltfAnimationChannel& translation = prGltfAnimationChannels(vectors.gltf)[0];
    CHECK(translation.path == PrGltfPath_Translation);
    CHECK(translation.node == 0);
    CHECK(translation.object == 0);
    CHECK(translation.width == 3);
    CHECK(translation.pointer == "/nodes/0/translation");
    CHECK(isNear(valueAt(vectors.gltf, 0, 0.5f), 3, 0.5f, 0.5f, 0.5f));

    GltfRead rotation(pointerAsset(R"("input":0,"output":4)", "/nodes/0/rotation"), &bin);
    REQUIRE(rotation.status == PrStatus_Ok);
    CHECK(prGltfAnimationChannels(rotation.gltf)[0].path == PrGltfPath_Rotation);
    CHECK(prGltfAnimationChannels(rotation.gltf)[0].width == 4);

    GltfRead weights(pointerAsset(R"("input":0,"output":7)", "/nodes/0/weights"), &bin);
    REQUIRE(weights.status == PrStatus_Ok);
    CHECK(prGltfAnimationChannels(weights.gltf)[0].path == PrGltfPath_Weights);
    CHECK(prGltfAnimationChannels(weights.gltf)[0].width == 2);
    CHECK(isNear(valueAt(weights.gltf, 0, 0.5f), 2, 0.5f, 0.5f));
}

TEST_CASE("a pointer names a property of a node, a mesh, a camera, a light or a material") {
    GltfBin bin = pointerBin();
    struct Case {
        PrStringView pointer;
        PrStringView output;
        PrGltfPath path;
        uint32_t object;
        uint32_t element;
        uint32_t width;
    };
    const Case cases[] = {
        {"/nodes/0/weights/1", "1", PrGltfPath_NodeWeight, 0, 1, 1},
        {"/meshes/0/weights", "7", PrGltfPath_MeshWeights, 0, 0, 2},
        {"/meshes/0/weights/0", "1", PrGltfPath_MeshWeight, 0, 0, 1},
        {"/cameras/0/perspective/aspectRatio", "1", PrGltfPath_CameraAspectRatio, 0, 0, 1},
        {"/cameras/0/perspective/yfov", "1", PrGltfPath_CameraYfov, 0, 0, 1},
        {"/cameras/0/perspective/znear", "1", PrGltfPath_CameraZnear, 0, 0, 1},
        {"/cameras/1/orthographic/xmag", "1", PrGltfPath_CameraXmag, 1, 0, 1},
        {"/cameras/1/orthographic/ymag", "1", PrGltfPath_CameraYmag, 1, 0, 1},
        {"/cameras/1/orthographic/zfar", "1", PrGltfPath_CameraZfar, 1, 0, 1},
        {"/extensions/KHR_lights_punctual/lights/1/color", "3", PrGltfPath_LightColor, 1, 0, 3},
        {"/extensions/KHR_lights_punctual/lights/1/intensity", "1", PrGltfPath_LightIntensity, 1, 0, 1},
        {"/extensions/KHR_lights_punctual/lights/0/spot/innerConeAngle", "1", PrGltfPath_LightInnerConeAngle, 0, 0, 1},
        {"/extensions/KHR_lights_punctual/lights/0/spot/outerConeAngle", "1", PrGltfPath_LightOuterConeAngle, 0, 0, 1},
        {"/materials/1/pbrMetallicRoughness/baseColorFactor", "4", PrGltfPath_MaterialBaseColorFactor, 1, 0, 4},
        {"/materials/1/pbrMetallicRoughness/metallicFactor", "1", PrGltfPath_MaterialMetallicFactor, 1, 0, 1},
        {"/materials/1/pbrMetallicRoughness/roughnessFactor", "1", PrGltfPath_MaterialRoughnessFactor, 1, 0, 1},
        {"/materials/1/alphaCutoff", "1", PrGltfPath_MaterialAlphaCutoff, 1, 0, 1},
        {"/materials/1/emissiveFactor", "3", PrGltfPath_MaterialEmissiveFactor, 1, 0, 3},
        {
            "/materials/0/extensions/KHR_materials_clearcoat/clearcoatFactor", "1", PrGltfPath_MaterialClearcoatFactor,
            0, 0, 1,
        },
        {
            "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset", "2",
            PrGltfPath_TextureOffset, 0, PrGltfMaterialTexture_BaseColor, 2,
        },
        {
            "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/rotation", "1",
            PrGltfPath_TextureRotation, 0, PrGltfMaterialTexture_BaseColor, 1,
        },
        {
            "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/scale", "2",
            PrGltfPath_TextureScale, 0, PrGltfMaterialTexture_BaseColor, 2,
        },
        // An integer of any type converts to a float.
        {"/materials/1/alphaCutoff", "8", PrGltfPath_MaterialAlphaCutoff, 1, 0, 1},
    };
    for (const Case& c : cases) {
        CAPTURE(c.pointer);
        String sampler = R"("input":0,"output":)";
        GltfRead read(pointerAsset(sampler.append(c.output), c.pointer), &bin);
        REQUIRE(read.status == PrStatus_Ok);
        const PrGltfAnimationChannel& channel = prGltfAnimationChannels(read.gltf)[0];
        CHECK(channel.path == c.path);
        CHECK(channel.object == c.object);
        CHECK(channel.element == c.element);
        CHECK(channel.width == c.width);
        CHECK(channel.node == (c.path == PrGltfPath_NodeWeight ? 0 : kPrGltfNone));
        CHECK(prGltfAnimations(read.gltf)[0].duration == 1);
    }

    GltfRead scalar(pointerAsset(R"("input":0,"output":1)", "/materials/1/alphaCutoff"), &bin);
    REQUIRE(scalar.status == PrStatus_Ok);
    CHECK(isNear(valueAt(scalar.gltf, 0, 0.5f), 1, 0.5f, 0));
    GltfRead integers(pointerAsset(R"("input":0,"output":8)", "/materials/1/alphaCutoff"), &bin);
    REQUIRE(integers.status == PrStatus_Ok);
    CHECK(isNear(valueAt(integers.gltf, 0, 0.5f), 1, 3, 0));
}

TEST_CASE("a pointer to a node's visibility steps through unsigned bytes") {
    GltfBin bin = pointerBin();
    GltfRead read(pointerAsset(R"("input":0,"output":5,"interpolation":"STEP")",
        "/nodes/1/extensions/KHR_node_visibility/visible"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfAnimationChannel& channel = prGltfAnimationChannels(read.gltf)[0];
    CHECK(channel.path == PrGltfPath_NodeVisible);
    CHECK(channel.node == 1);
    CHECK(channel.width == 1);
    CHECK(valueAt(read.gltf, 0, 0.5f).v[0] == 0);
    CHECK(valueAt(read.gltf, 0, 1).v[0] == 1);

    const char* mistyped = "animations[0].channels[0].sampler has an output accessor of a type that the path does not allow";
    CHECK(gltfRefusal(pointerAsset(R"("input":0,"output":5)", "/nodes/1/extensions/KHR_node_visibility/visible"),
        &bin) == mistyped);
    CHECK(gltfRefusal(pointerAsset(R"("input":0,"output":1,"interpolation":"STEP")",
        "/nodes/1/extensions/KHR_node_visibility/visible"), &bin) == mistyped);
}

TEST_CASE("a pointer into extras or an extension that the module does not read animates nothing it knows") {
    GltfBin bin = pointerBin();
    const PrStringView pointers[] = {
        "/nodes/0/extras/glow", "/extras/x", "/materials/0/extensions/VENDOR_paint/gloss",
        "/extensions/VENDOR_world/fog",
    };
    for (PrStringView pointer : pointers) {
        CAPTURE(pointer);
        GltfRead read(pointerAsset(R"("input":0,"output":1)", pointer), &bin);
        REQUIRE(read.status == PrStatus_Ok);
        const PrGltfAnimationChannel& channel = prGltfAnimationChannels(read.gltf)[0];
        CHECK(channel.path == PrGltfPath_Pointer);
        CHECK(channel.pointer == pointer);
        CHECK(channel.object == kPrGltfNone);
        CHECK(channel.width == 0);
    }
}

TEST_CASE("a pointer is checked against the properties that the asset defines") {
    GltfBin bin = pointerBin();
    auto refusal = [&](PrStringView output, PrStringView pointer) {
        String sampler = R"("input":0,"output":)";
        return gltfRefusal(pointerAsset(sampler.append(output), pointer), &bin);
    };
    const char* scope = "animations[0].channels[0].target.extensions.KHR_animation_pointer.pointer ";
    auto said = [&](PrStringView message) {
        String sentence = scope;
        return sentence.append(message);
    };
    CHECK(refusal("1", "nodes/0/rotation") == said("is not a JSON pointer"));
    CHECK(refusal("1", "/accessors/0/count") == said("names no property that an animation can target"));
    CHECK(refusal("1", "/nodes/x/rotation") == said("names no object by its index"));
    CHECK(refusal("1", "/nodes/01/rotation") == said("names no object by its index"));
    CHECK(refusal("1", "/nodes/2/rotation") == said("names an object that the asset does not have"));
    CHECK(refusal("1", "/nodes/0/matrix") == said("names no property that an animation can target"));
    CHECK(refusal("1", "/nodes/0/weights/2") == said("names a weight of a morph target that the mesh does not have"));
    CHECK(refusal("7", "/nodes/1/weights") == said("names a property that the asset does not define"));
    CHECK(refusal("1", "/nodes/1/weights/0") == said("names a weight of a morph target that the mesh does not have"));
    // A perspective camera's far plane, a point light's cone, and a light's range have no default.
    CHECK(refusal("1", "/cameras/0/perspective/zfar") == said("names a property that the asset does not define"));
    CHECK(refusal("1", "/cameras/0/orthographic/xmag") == said("names a property that the asset does not define"));
    CHECK(refusal("1", "/extensions/KHR_lights_punctual/lights/1/spot/innerConeAngle") ==
        said("names a property that the asset does not define"));
    CHECK(refusal("1", "/extensions/KHR_lights_punctual/lights/0/range") ==
        said("names a property that the asset does not define"));
    // A material's extension, a texture and a transform that the material does not have.
    CHECK(refusal("1", "/materials/1/extensions/KHR_materials_clearcoat/clearcoatFactor") ==
        said("names a property that the asset does not define"));
    CHECK(refusal("1", "/materials/1/normalTexture/scale") == said("names a property that the asset does not define"));
    CHECK(refusal("2", "/materials/1/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset") ==
        said("names a property that the asset does not define"));
    CHECK(refusal("2", "/materials/0/emissiveTexture/extensions/KHR_texture_transform/offset") ==
        said("names a property that the asset does not define"));

    const char* mistyped = "animations[0].channels[0].sampler has an output accessor of a type that the path does not allow";
    CHECK(refusal("3", "/materials/1/pbrMetallicRoughness/baseColorFactor") == mistyped);
    CHECK(refusal("1", "/extensions/KHR_lights_punctual/lights/1/color") == mistyped);
    CHECK(refusal("3", "/meshes/0/weights") == mistyped);
    CHECK(refusal("1", "/meshes/0/weights") ==
        "animations[0].channels[0].sampler has an output accessor whose count does not match its keyframes");

    // A channel of a pointer names no node.
    CHECK(gltfRefusal(gltfBinAsset(String(kPointerAccessors).append(",").append(kPointerObjects).append(
        R"(,"animations":[{"samplers":[{"input":0,"output":1}],"channels":[{"sampler":0,"target":{"node":0,)"
        R"("path":"pointer","extensions":{"KHR_animation_pointer":{"pointer":"/materials/1/alphaCutoff"}}}}]}])")),
        &bin) == "animations[0].channels[0].target.extensions.KHR_animation_pointer.node must not be defined with a pointer");
}

TEST_CASE("an animation animates one property with one channel, however its pointers name it") {
    GltfBin bin = pointerBin();
    auto refuses = [&](PrStringView output, PrStringView first, PrStringView second) {
        const PrStringView pointers[] = {first, second};
        String sampler = R"("input":0,"output":)";
        return gltfRefusal(pointerAsset(sampler.append(output), Span<const PrStringView>(pointers)), &bin) ==
            "animations[0].channels animate one property twice";
    };
    CHECK(refuses("1", "/materials/1/alphaCutoff", "/materials/1/alphaCutoff"));
    CHECK(refuses("1", "/nodes/0/weights/1", "/nodes/0/weights/1"));
    // An array and one of its elements, in either order, and a core target and the pointer to it.
    auto refusesBoth = [&](PrStringView first, PrStringView second) {
        String members = kPointerAccessors;
        members.append(",").append(kPointerObjects).append(
            R"(,"animations":[{"samplers":[{"input":0,"output":7},{"input":0,"output":1}],"channels":[)");
        members.append(first).append(",").append(second).append("]}]");
        return gltfRefusal(gltfBinAsset(members), &bin) == "animations[0].channels animate one property twice";
    };
    const char* meshWeights =
        R"({"sampler":0,"target":{"path":"pointer","extensions":{"KHR_animation_pointer":{"pointer":"/meshes/0/weights"}}}})";
    const char* meshWeight = R"({"sampler":1,"target":{"path":"pointer","extensions":)"
        R"({"KHR_animation_pointer":{"pointer":"/meshes/0/weights/1"}}}})";
    const char* nodeWeights = R"({"sampler":0,"target":{"node":0,"path":"weights"}})";
    const char* nodeWeight = R"({"sampler":1,"target":{"path":"pointer","extensions":)"
        R"({"KHR_animation_pointer":{"pointer":"/nodes/0/weights/0"}}}})";
    const char* pointedWeights = R"({"sampler":0,"target":{"path":"pointer","extensions":)"
        R"({"KHR_animation_pointer":{"pointer":"/nodes/0/weights"}}}})";
    CHECK(refusesBoth(meshWeights, meshWeight));
    CHECK(refusesBoth(meshWeight, meshWeights));
    CHECK(refusesBoth(nodeWeight, nodeWeights));
    CHECK(refusesBoth(nodeWeights, pointedWeights));

    // Different objects and different elements are different properties.
    const PrStringView apart[] = {
        "/materials/0/alphaCutoff", "/materials/1/alphaCutoff", "/nodes/0/weights/0",
        "/nodes/0/weights/1", "/meshes/0/weights/1", "/cameras/0/perspective/yfov",
    };
    GltfRead read(pointerAsset(R"("input":0,"output":1)", Span<const PrStringView>(apart)), &bin);
    CHECK(read.status == PrStatus_Ok);
}
