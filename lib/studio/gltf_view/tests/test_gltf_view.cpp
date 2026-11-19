// Studio's glTF structure view: the references among an asset's items, the asset and its collections, an item's
// extensions, a mesh's primitives, an accessor's elements and bytes, and the links that the view follows.

#include "pr/blob.h"
#include "pr/gltf_view.h"
#include "pr/layout_query.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// An asset whose node draws a mesh, whose primitive reads two accessors of one buffer view and a material, and has a
// morph target, and whose base color is a texture of an image in a buffer view. The material has an extension that the
// engine reads and one that it does not.
constexpr PrStringView kAsset = R"({"asset":{"version":"2.0","generator":"test"},"scene":0,)"
    R"("extensionsUsed":["KHR_materials_clearcoat","VENDOR_unknown"],"extensionsRequired":["VENDOR_unknown"],)"
    R"("scenes":[{"nodes":[0]}],"nodes":[{"name":"Car","mesh":0,"children":[1]},{"name":"Wheel"}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0,)"
    R"("targets":[{"POSITION":0}]}]}],)"
    R"("materials":[{"name":"Paint","pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"extensions":{)"
    R"("KHR_materials_clearcoat":{"clearcoatFactor":1},"VENDOR_unknown":{}}}],)"
    R"("textures":[{"source":0}],"images":[{"bufferView":1,"mimeType":"image/png"}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"componentType":5123,"count":3,"type":"SCALAR"}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":42},{"buffer":0,"byteOffset":44,"byteLength":8}],)"
    R"("buffers":[{"byteLength":52}]})";

// An asset whose items name others through extensions: a light of KHR_lights_punctual, a variant of
// KHR_materials_variants, a Draco stream whose attributes are its own ids, levels of detail of MSFT_lod, and an
// animation that targets a material's property through KHR_animation_pointer.
constexpr PrStringView kExtended = R"({"asset":{"version":"2.0"},)"
    R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","name":"Lamp"}]},)"
    R"("KHR_materials_variants":{"variants":[{"name":"Red"}]}},)"
    R"("nodes":[{"mesh":0,"extensions":{"KHR_lights_punctual":{"light":0},"MSFT_lod":{"ids":[1]}}},{"mesh":0}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"extensions":{)"
    R"("KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":5}},)"
    R"("KHR_materials_variants":{"mappings":[{"material":1,"variants":[0]}]}}}]}],)"
    R"("materials":[{},{}],"accessors":[{"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"componentType":5126,"count":3,"type":"VEC3"},{"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"componentType":5126,"count":3,"type":"VEC3"},{"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"componentType":5126,"count":3,"type":"VEC3"}],)"
    R"("animations":[{"samplers":[{"input":1,"output":2}],"channels":[{"sampler":0,"target":{"path":"pointer",)"
    R"("extensions":{"KHR_animation_pointer":{"pointer":"/materials/1/emissiveFactor"}}}}]}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":4}],"buffers":[{"byteLength":4}]})";

// An asset of accessors over one buffer, "bin": three positions of floats; a thousand normalized unsigned shorts, each
// 65 times its index, at an offset within their buffer view; and four floats of zeros of which a sparse substitution
// replaces the second and fourth with 5.5 and 7.25.
constexpr PrStringView kData = R"({"asset":{"version":"2.0"},"accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,2,0]},)"
    R"({"bufferView":1,"byteOffset":4,"componentType":5123,"normalized":true,"count":1000,"type":"SCALAR"},)"
    R"({"componentType":5126,"count":4,"type":"SCALAR","sparse":{"count":2,)"
    R"("indices":{"bufferView":2,"componentType":5121},"values":{"bufferView":2,"byteOffset":4}}}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":2004},)"
    R"({"buffer":0,"byteOffset":2040,"byteLength":12}],"buffers":[{"byteLength":2052,"uri":"bin"}]})";

// The bytes of kData's buffer.
Array<uint8_t> dataBuffer() {
    Array<uint8_t> bytes;
    bytes.resize(2052);
    prMemZero(bytes.begin(), bytes.count());
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 2, 0};
    prMemCopy(bytes.begin(), positions, sizeof(positions));
    for (uint32_t i = 0; i < 1000; ++i) {
        uint16_t value = uint16_t(i * 65);
        prMemCopy(bytes.begin() + 40 + i * 2, &value, sizeof(value));
    }
    bytes[2040] = 1;
    bytes[2041] = 3;
    const float values[] = {5.5f, 7.25f};
    prMemCopy(bytes.begin() + 2044, values, sizeof(values));
    return bytes;
}

// A kit with a view of an asset, which shows `selection`, with the asset's document if it reads.
struct ViewFixture {
    KitFixture f;
    PrJson* json = prJsonCreate();
    PrGltf* PR_NULLABLE gltf = nullptr;
    Array<uint8_t> buffer;
    PrGltfView* view = nullptr;
    String selection;
    String followed;

    static PrBlob* PR_NULLABLE load(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
        const ViewFixture* fixture = static_cast<const ViewFixture*>(context);
        if (uri != "bin")
            return nullptr;
        return prBlobCopy(PrByteView{fixture->buffer.begin(), fixture->buffer.count()});
    }

    explicit ViewFixture(PrStringView asset = kAsset, Array<uint8_t> bytes = {}) :
            buffer(static_cast<Array<uint8_t>&&>(bytes)) {
        REQUIRE(prJsonParse(json, asset, 0, nullptr));
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = load;
        options.loadContext = this;
        PrBlob* blob = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(asset.data), asset.size});
        prGltfCreate(blob, &options, &gltf);
        prBlobRelease(blob);
        view = prGltfViewCreate(f.kit);
        f.build = [this](PrLayoutNode root) {
            PrGltfViewContent content{
                .json = json,
                .gltf = gltf,
                .selection = selection,
                .unshaded = PrGltfMaterialExtensionFlag_Clearcoat,
            };
            prGltfViewFrame(view, root, content);
            PrRawString link = prRawStringNull();
            if (prGltfViewFollowed(view, &link))
                followed = prRawStringView(&link);
            prRawStringReset(&link);
        };
    }
    ~ViewFixture() {
        prGltfViewDestroy(view);
        prGltfDestroy(gltf);
        prJsonDestroy(json);
    }

    // Shows `pointer`, over the two rounds that a table's window takes to reach its rows.
    void show(PrStringView pointer) {
        selection = pointer;
        f.frame();
        f.frame();
    }

    // Returns the innermost node whose text, as the last layout shows it, is `text`, or none.
    PrLayoutNode find(PrStringView text, PrLayoutNode node) {
        for (PrLayoutNode child = prLayoutNodeFirstChild(f.tree, node); child != kPrLayoutNodeNone;
                child = prLayoutNodeNextSibling(f.tree, child)) {
            if (PrLayoutNode found = find(text, child); found != kPrLayoutNodeNone)
                return found;
        }
        String shown = String::adopt(prLayoutNodeRenderedText(f.tree, node, nullptr));
        return shown == text ? node : kPrLayoutNodeNone;
    }
    PrLayoutNode find(PrStringView text) { return find(text, f.root); }

    bool shows(PrStringView text) {
        String shown = String::adopt(prLayoutNodeRenderedText(f.tree, f.root, nullptr));
        return prStringFind(shown, text) != shown.count();
    }

    // Clicks the node whose text is `text`, and returns the pointer that the view followed, or `-` if it followed none.
    String follow(PrStringView text) {
        followed = "-";
        PrLayoutNode node = find(text);
        REQUIRE(node != kPrLayoutNodeNone);
        f.click(f.center(node));
        f.frame();
        return followed;
    }
};

// Returns true if the view indexed a reference of `from` to `to` in the member at `path`.
bool references(const PrGltfView* view, PrStringView from, PrStringView to, PrStringView path) {
    for (uint32_t i = 0; i < prGltfViewReferenceCount(view); ++i) {
        PrGltfViewReference reference = prGltfViewReference(view, i);
        if (reference.from == from && reference.to == to && reference.path == path)
            return true;
    }
    return false;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the view indexes the references among an asset's items by the members that make them") {
    ViewFixture v;
    v.f.frame();
    const PrGltfView* view = v.view;
    CHECK(references(view, "", "/scenes/0", "/scene"));
    CHECK(references(view, "/scenes/0", "/nodes/0", "/nodes/0"));
    CHECK(references(view, "/nodes/0", "/meshes/0", "/mesh"));
    CHECK(references(view, "/nodes/0", "/nodes/1", "/children/0"));
    CHECK(references(view, "/meshes/0", "/accessors/0", "/primitives/0/attributes/POSITION"));
    CHECK(references(view, "/meshes/0", "/accessors/1", "/primitives/0/indices"));
    CHECK(references(view, "/meshes/0", "/materials/0", "/primitives/0/material"));
    CHECK(references(view, "/meshes/0", "/accessors/0", "/primitives/0/targets/0/POSITION"));
    CHECK(references(view, "/materials/0", "/textures/0", "/pbrMetallicRoughness/baseColorTexture/index"));
    CHECK(references(view, "/textures/0", "/images/0", "/source"));
    CHECK(references(view, "/images/0", "/bufferViews/1", "/bufferView"));
    CHECK(references(view, "/accessors/1", "/bufferViews/0", "/bufferView"));
    CHECK(references(view, "/bufferViews/1", "/buffers/0", "/buffer"));
    CHECK(prGltfViewReferenceCount(view) == 15);
}

TEST_CASE("extensions name items by their own rules, and a Draco stream's attributes name no accessors") {
    ViewFixture v(kExtended);
    v.f.frame();
    const PrGltfView* view = v.view;
    CHECK(references(view, "/nodes/0", "/extensions/KHR_lights_punctual/lights/0", "/extensions/KHR_lights_punctual/light"));
    CHECK(references(view, "/nodes/0", "/nodes/1", "/extensions/MSFT_lod/ids/0"));
    CHECK(references(view, "/meshes/0", "/bufferViews/0", "/primitives/0/extensions/KHR_draco_mesh_compression/bufferView"));
    CHECK_FALSE(references(view, "/meshes/0", "/accessors/5",
        "/primitives/0/extensions/KHR_draco_mesh_compression/attributes/POSITION"));
    CHECK(references(view, "/meshes/0", "/extensions/KHR_materials_variants/variants/0",
        "/primitives/0/extensions/KHR_materials_variants/mappings/0/variants/0"));
    CHECK(references(view, "/meshes/0", "/materials/1", "/primitives/0/extensions/KHR_materials_variants/mappings/0/material"));
    CHECK(references(view, "/animations/0", "/materials/1",
        "/channels/0/target/extensions/KHR_animation_pointer/pointer"));

    // An item of an extension's collection shows as any other, with what names it.
    v.show("/extensions/KHR_lights_punctual/lights/0");
    CHECK(v.shows("Lamp"));
    CHECK(v.shows("NAMED BY"));
    CHECK(v.follow("/nodes/0") == "/nodes/0");
}

TEST_CASE("the asset shows its default scene, its collections, which link, and the extensions that it uses") {
    ViewFixture v;
    v.show("");
    CHECK(v.shows("Asset"));
    CHECK(v.shows("generator = \"test\""));
    CHECK(v.find("/scenes/0") != kPrLayoutNodeNone);
    CHECK(v.find("/bufferViews") != kPrLayoutNodeNone);
    CHECK(v.shows("VENDOR_unknown"));
    CHECK(v.shows("required"));
    CHECK(v.follow("/accessors") == "/accessors");

    // A collection lists its items, each with its name and a summary, and links to each.
    v.show("/nodes");
    CHECK(v.shows("2 items"));
    CHECK(v.find("Car") != kPrLayoutNodeNone);
    CHECK(v.find("mesh 0, 1 child") != kPrLayoutNodeNone);
    CHECK(v.follow("/nodes/1") == "/nodes/1");
    v.show("/accessors");
    CHECK(v.find("3 x VEC3 of FLOAT") != kPrLayoutNodeNone);
}

TEST_CASE("an item shows its properties, its extensions and whether each is drawn, and its links, which it follows") {
    ViewFixture v;
    v.show("/materials/0");
    CHECK(v.shows("/pbrMetallicRoughness/baseColorTexture/index"));
    CHECK(v.shows("KHR_materials_clearcoat"));
    CHECK(v.shows("read, not drawn"));
    CHECK(v.shows("VENDOR_unknown"));
    CHECK(v.shows("not read"));
    CHECK(prGltfViewExtensionState("KHR_texture_transform", 0) == PrGltfViewExtension_Drawn);

    // The material names a texture and is named by the mesh, and following a link reports its pointer.
    CHECK(v.follow("/textures/0") == "/textures/0");
    CHECK(v.follow("/meshes/0") == "/meshes/0");

    // A pointer into an item shows the item, and an image links to the buffer view that holds its bytes.
    v.show("/images/0/mimeType");
    CHECK(v.shows("/images/0"));
    REQUIRE(v.find("/bufferViews/1") != kPrLayoutNodeNone);
}

TEST_CASE("a mesh shows each primitive's attributes, indices, mode, material and targets, linked to their items") {
    ViewFixture v;
    v.show("/meshes/0");
    CHECK(v.shows("PRIMITIVES"));
    CHECK(v.find("attributes/POSITION") != kPrLayoutNodeNone);
    CHECK(v.find("indices") != kPrLayoutNodeNone);
    CHECK(v.find("TRIANGLES (the default)") != kPrLayoutNodeNone);
    CHECK(v.find("targets/0/POSITION") != kPrLayoutNodeNone);
    CHECK(v.find("3 x SCALAR of UNSIGNED_SHORT") != kPrLayoutNodeNone);
    CHECK(v.find("Paint: OPAQUE") != kPrLayoutNodeNone);
    // Selecting the row of the material follows its link.
    CHECK(v.follow("Paint: OPAQUE") == "/materials/0");
}

TEST_CASE("an accessor shows its elements, the bytes that hold them, and its values in a window of its table") {
    ViewFixture v(kData, dataBuffer());
    REQUIRE(v.gltf != nullptr);
    v.show("/accessors/0");
    CHECK(v.find("VEC3 of FLOAT (5126), 12 bytes") != kPrLayoutNodeNone);
    CHECK(v.find("[0, 0, 0]") != kPrLayoutNodeNone);
    CHECK(v.find("[1, 2, 0]") != kPrLayoutNodeNone);
    CHECK(v.find("[0, 36) of 36") != kPrLayoutNodeNone);
    CHECK(v.find("2") != kPrLayoutNodeNone);

    // The normalized integers show as stored, with the numbers that they stand for, and the bytes of their buffer view
    // and of its buffer.
    v.show("/accessors/1");
    CHECK(v.find("[4, 2004) of 2004") != kPrLayoutNodeNone);
    CHECK(v.find("[40, 2040) of 2052") != kPrLayoutNodeNone);
    CHECK(v.find("130 (0.0019836728)") != kPrLayoutNodeNone);
    // The table creates the rows of its window, not all thousand.
    CHECK(v.find("12") != kPrLayoutNodeNone);
    CHECK(v.find("500") == kPrLayoutNodeNone);
    CHECK(v.follow("[40, 2040) of 2052") == "/buffers/0");

    // A sparse accessor shows its substitution over its zeros, and the buffer view of its indices and values.
    v.show("/accessors/2");
    CHECK(v.find("2 elements replace the source's, by indices of UNSIGNED_BYTE") != kPrLayoutNodeNone);
    CHECK(v.find("none: the elements are zeros") != kPrLayoutNodeNone);
    CHECK(v.find("sparse/indices") != kPrLayoutNodeNone);
    CHECK(v.find("[4, 12) of 12") != kPrLayoutNodeNone);
    CHECK(v.find("5.5") != kPrLayoutNodeNone);
    CHECK(v.find("7.25") != kPrLayoutNodeNone);

    // A buffer view shows the bytes of its buffer.
    v.show("/bufferViews/2");
    CHECK(v.find("[2040, 2052) of 2052") != kPrLayoutNodeNone);
    CHECK(kitVerified(v.f.tree));
}
