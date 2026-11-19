// A material type of the consumer's own, which its shader set adds to lib/render's (CMakeLists.txt): stripes of two
// colors along an attribute of the mesh's vertices that only the type knows, the distance along the stripes.

#pragma once

#include "pr/render_material.slang"
#include "pr/render_shading.slang"

#include "consumer_materials.h"

struct ConsumerStripes : IRenderMaterial {
    typedef ConsumerStripesParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {};

    // The distance along the stripes, which a vertex without it has at zero.
    struct Attributes {
        [RenderVertex("custom", "float", 0, 0, 0, 0)] float along;
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = frac(attributes.along * parameters.count) < 0.5 ? parameters.first.rgb : parameters.second.rgb;
        surface.specular = 1;
        return surface;
    }
}
