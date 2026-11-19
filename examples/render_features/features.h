// The pages of the render features example: small procedural scenes, each of which shows one feature of lib/render and
// sweeps its parameters. A page is a function that fills a renderer with a world, from generated geometry and texels
// alone, so the example reads no file and runs on the web as it runs natively.

#pragma once

#include "pr/array.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"

PR_ASSUME_NONNULL_BEGIN

namespace render_features {

using namespace pr;

// The time, in seconds, at which a capture and the tests show every page, so that two of them show the same picture.
constexpr double kStillTime = 2.5;

struct World;

// Writes into an update what a page moves with time: the transforms, joints and morph weights of `World::moving` at
// `time` seconds.
typedef void (*WorldMove)(PrRenderUpdate* update, const World& world, double time);

// A page's world as one renderer holds it.
struct World {
    PrRenderScene scene{};
    PrRenderEnvironment environment{};
    Array<PrRenderMesh> meshes;
    Array<PrRenderTexture> textures;
    Array<PrRenderMaterial> materials;
    PrVec3 target{0, 0.5f, 0};      // what the camera looks at
    float distance = 9;             // from where
    Array<PrRenderInstance> moving; // the instances that the page moves with time
    WorldMove PR_NULLABLE move = nullptr;
};

// A page: its name, as --page takes it, what it shows, row by row, and the function that makes its world.
struct Page {
    PrStringView name;
    PrStringView text;
    void (*build)(PrRender* render, World* world);
};

// Returns the pages, in the order that they are shown.
Span<const Page> pages();

// Returns the index of the page of a name, or the number of pages if there is none.
uint32_t pageNamed(PrStringView name);

// Fills `render` with a page's world, and writes what names it to `out`. The renderer draws with the example's shader
// set (RenderFeaturesShaders.h). A renderer on a new device is filled the same way.
void worldCreate(PrRender* render, const Page& page, World* out);

// Shows a world at `time` seconds, in one update: the scene's time, which a material type may read, and what the page
// moves. A frame draws the materials at the time of the transforms.
void worldSetTime(PrRender* render, const World& world, double time);

// Destroys what worldCreate made in `render`.
void worldDestroy(PrRender* render, World* world);

} // namespace render_features

PR_ASSUME_NONNULL_END
