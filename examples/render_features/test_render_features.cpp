// Every page of the render features example, offscreen, as the tests of lib/render compare a scene: a device with the
// slots alone draws the bytes that a device with handles draws, and the visibility technique draws what the forward
// technique draws away from geometric edges. Each page is drawn at the time that a capture shows, on each backend that
// the system has a device for, and on the web in a page, on its WebGPU, which has the slots alone.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/render.h"
#include "pr/render_view.h"

#include "examples/render_features/features.h"
#include "render_shaders/RenderFeaturesShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

namespace {

using namespace harness;
using namespace render_features;

constexpr PrUVec2 kPageExtent{160, 120};

// Returns the view of a page from where the example's camera starts.
PrRenderViewDesc pageView(const World& world) {
    constexpr float kTilt = 0.4f;
    PrVec3 direction{0, prF32Sin(kTilt), prF32Cos(kTilt)};
    PrRenderViewDesc view = prRenderViewDescDefault(world.scene);
    view.camera = prRenderCameraLookAt(direction * world.distance + world.target, world.target, 0.9f,
        float(kPageExtent.x) / float(kPageExtent.y), 0.1f);
    view.extent = kPageExtent;
    return view;
}

// Returns true if a pixel's channels differ between two images by more than a part of their value.
bool differs(const Image& a, const Image& b, uint32_t i, float part) {
    for (uint32_t channel = 0; channel < 3; ++channel) {
        float x = a.texels[i * 4 + channel];
        float y = b.texels[i * 4 + channel];
        if (prF32Abs(x - y) > part * (0.1f + prF32Max(prF32Abs(x), prF32Abs(y))))
            return true;
    }
    return false;
}

// Returns the part of an image whose pixels differ from its first pixel.
float varied(const Image& image) {
    uint32_t pixels = image.extent.x * image.extent.y;
    uint32_t count = 0;
    for (uint32_t i = 1; i < pixels; ++i) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            if (image.texels[i * 4 + channel] != image.texels[channel]) {
                ++count;
                break;
            }
        }
    }
    return float(count) / float(pixels);
}

} // namespace

TEST_SUITE_BEGIN("render features" * doctest::skip(!gpuTests()));

TEST_CASE("every page draws alike with slots and with handles, and by either technique") {
    const Array<TestDevice>& devices = testDevices();
    // The forward images of the last device with handles, by page, which the device of its backend with the slots alone
    // draws again.
    Array<Image> handles;
    PrGpuBackend handlesBackend = PrGpuBackend_Default;
    for (const TestDevice& device : devices) {
        PrStringView backend = prBackendName(prGpuSelectInterface(), device.backend);
        CAPTURE(backend);
        CAPTURE(device.slotsOnly);
        uint32_t errors = validationErrors(device);
        {
            Fixture f(device, renderFeaturesShaderSet());
            bool compared = !f.bindless && device.backend == handlesBackend && handles.count() == pages().count();
            if (f.bindless) {
                handles.clear();
                handlesBackend = device.backend;
            }
            for (uint32_t index = 0; index < pages().count(); ++index) {
                const Page& page = pages()[index];
                CAPTURE(page.name);
                World world;
                worldCreate(f.render, page, &world);
                worldSetTime(f.render, world, kStillTime);
                PrRenderViewDesc view = pageView(world);
                Image image = f.view(view);
                REQUIRE(image.extent == kPageExtent);
                // Every page shows something besides its background.
                CHECK(varied(image) > 0.2f);
                if (compared) {
                    // The tiers round apart where fast math lets each library's compiler (kTierTolerance): on Metal,
                    // openpbr-coat differed by 1.2e-4 at one pixel, and by nothing with fast math off.
                    Difference difference = compare(handles[index], image, kTierTolerance);
                    CAPTURE(difference.largest);
                    CHECK(difference.over == 0);
                }
                if (f.bindless && prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility)) {
                    // A pixel on a geometric edge shows another surface by each technique, which a single-sampled
                    // forward render finds where it differs from the multisampled one. Away from them, the two
                    // techniques shade one surface with one function, and differ where a minified texture is filtered
                    // at another level, by their derivatives.
                    view.samples = 1;
                    Image single = f.view(view);
                    view.technique = PrRenderTechnique_Visibility;
                    Image visibility = f.view(view);
                    uint32_t pixels = kPageExtent.x * kPageExtent.y;
                    uint32_t edges = 0;
                    uint32_t over = 0;
                    for (uint32_t i = 0; i < pixels; ++i) {
                        if (differs(image, single, i, 0.02f))
                            ++edges;
                        else if (differs(single, visibility, i, 0.05f))
                            ++over;
                    }
                    CAPTURE(edges);
                    CAPTURE(over);
                    CHECK(over <= (pixels - edges) / 100);
                }
                if (f.bindless)
                    handles.push(move(image));
                worldDestroy(f.render, &world);
            }
        }
        CHECK(validationErrors(device) == errors);
    }
}

// The last case of the binary, in the order in which the cases run.
TEST_CASE("the devices that the tests share are destroyed") {
    destroyTestDevices();
    CHECK(testDevices().count() == 0);
}

TEST_SUITE_END();
