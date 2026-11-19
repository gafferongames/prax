// lib/render's visibility buffer: a technique that first draws which triangle each pixel shows, and then shades each
// pixel once. The shading pass finds the pixel's point on its triangle from the scene's records, with the derivatives
// of what it interpolates there, and calls the materials and the shading models that the forward technique calls. One
// shader shades every material type, by the shader set's dispatch on a material's type, in whose case for a type the
// attributes of the type's layout are pulled and interpolated, where the layout is a fact of the code. A shader set's
// source of the pass includes this file (render_shaders_check.cmake).
//
// Both passes read the meshes' vertices and indices by handle, so the technique needs a device with bindless.

#include "pr/render_whole.slang"

#include "render_shader_set.slang"

// ---------------------------------------------------------------------------------------------------------------------
// The triangle at a pixel

// The corners of a triangle of an instance, in the world, and the indices of their vertices.
struct Triangle {
    float3 position[3];
    float3 normal[3];       // not of unit length
    float4 tangent[3];      // not of unit length, and the handedness in the world in w
    uint index[3];
};

// Returns the triangle `primitive` of a part of an instance's mesh.
Triangle sceneTriangle(PrRenderInstanceRecord instance, PrRenderPartRecord part, uint primitive) {
    float sign = renderWorldSign(instance);
    Triangle triangle;
    for (uint corner = 0; corner < 3; ++corner) {
        uint index = renderIndexPull(part, primitive * 3 + corner);
        RenderVertex vertex = renderVertexPull(part, index);
        triangle.index[corner] = index;
        triangle.position[corner] = renderWorldPoint(instance, vertex.position);
        triangle.normal[corner] = renderWorldNormal(instance, vertex.normal.xyz);
        triangle.tangent[corner] = float4(renderWorldDirection(instance, vertex.tangent.xyz), vertex.tangent.w * sign);
    }
    return triangle;
}

// The weights of a triangle's corners at a pixel, their derivatives across one pixel to the right (`weightsDx`) and one
// pixel down (`weightsDy`), and whether the pixel shows the triangle's front face.
struct TrianglePoint {
    float3 weights;
    float3 weightsDx;
    float3 weightsDy;
    bool isFront;
};

// Returns the weights of a triangle's corners at a pixel of the camera's view, which interpolate a value over the
// triangle in the world, and the derivatives of those weights by the pixel. `pixel` is the pixel's center in the
// target. A triangle faces front if its corners run counterclockwise in the target, with y up, or clockwise for an
// instance whose matrix reverses its winding (`mirrored`).
//
// The weights follow from the corners' positions in clip space. Each corner's weight in the target is linear in the
// pixel, and so is that weight over the corner's w, which is q below. The three q over their sum are the weights that
// are correct for perspective. The change of q across one pixel is therefore one value over the whole triangle.
//
// The derivative of a weight is the exact one at the pixel's center: by the quotient rule, the change of q less the
// weight times the change of the sum, over the sum. A draw's fragment shader takes a difference between two pixels
// instead (prGpuDdxFine), which is another number where perspective bends the weights within one pixel. The technique
// does not imitate that difference. The exact derivative costs less to compute and filters by what the pixel shows.
TrianglePoint trianglePoint(Triangle triangle, float2 pixel, bool mirrored) {
    float4 clip0 = renderClipPoint(kPrRenderViewCamera, triangle.position[0]);
    float4 clip1 = renderClipPoint(kPrRenderViewCamera, triangle.position[1]);
    float4 clip2 = renderClipPoint(kPrRenderViewCamera, triangle.position[2]);
    float3 inverseW = 1 / float3(clip0.w, clip1.w, clip2.w);
    float2 p0 = clip0.xy * inverseW.x;
    float2 p1 = clip1.xy * inverseW.y;
    float2 p2 = clip2.xy * inverseW.z;
    // The target's y runs down, and clip space's runs up.
    float2 p = float2(pixel.x / frame.targetExtent.x * 2 - 1, 1 - pixel.y / frame.targetExtent.y * 2);
    float2 perPixel = float2(2 / frame.targetExtent.x, -2 / frame.targetExtent.y);

    float2 e1 = p1 - p0;
    float2 e2 = p2 - p0;
    float2 d = p - p0;
    float inverseArea = 1 / (e1.x * e2.y - e2.x * e1.y);
    float w1 = (d.x * e2.y - e2.x * d.y) * inverseArea;
    float w2 = (e1.x * d.y - d.x * e1.y) * inverseArea;
    // The gradients of the second and the third corner's weight in the target, by clip space's x and y.
    float2 g1 = float2(e2.y, -e2.x) * inverseArea;
    float2 g2 = float2(-e1.y, e1.x) * inverseArea;
    float3 q = float3(1 - w1 - w2, w1, w2) * inverseW;
    float3 qDx = float3(-g1.x - g2.x, g1.x, g2.x) * inverseW * perPixel.x;
    float3 qDy = float3(-g1.y - g2.y, g1.y, g2.y) * inverseW * perPixel.y;
    float sum = q.x + q.y + q.z;
    float sumDx = qDx.x + qDx.y + qDx.z;
    float sumDy = qDy.x + qDy.y + qDy.z;
    TrianglePoint point;
    point.weights = q / sum;
    point.weightsDx = (qDx - point.weights * sumDx) / sum;
    point.weightsDy = (qDy - point.weights * sumDy) / sum;
    // The sign of this determinant is the winding of the corners in the target, and stays right for a triangle that
    // reaches behind the camera, whose corners there have a negative w.
    float winding = dot(clip0.xyw, cross(clip1.xyw, clip2.xyw));
    point.isFront = (winding > 0) != mirrored;
    return point;
}

// Returns the point of a surface that a pixel at `pixel` shows of the triangle `primitive` of a draw, and writes the
// triangle's corners at that point to `corners`, from which a material's attributes are interpolated with their
// derivatives.
RenderSurfaceInputs trianglePointInputs(PrRenderDrawRecord draw, PrRenderInstanceRecord instance, uint primitive,
    float2 pixel, out RenderCorners corners) {
    Triangle triangle = sceneTriangle(instance, renderPart(draw.part), primitive);
    TrianglePoint point = trianglePoint(triangle, pixel, (draw.flags & kPrRenderDrawMirrored) != 0);
    float3 w = point.weights;
    float facing = point.isFront ? 1.0 : -1.0;

    float4 tangent = triangle.tangent[0] * w.x + triangle.tangent[1] * w.y + triangle.tangent[2] * w.z;
    RenderSurfaceInputs inputs;
    inputs.position = triangle.position[0] * w.x + triangle.position[1] * w.y + triangle.position[2] * w.z;
    inputs.normal =
        normalize(triangle.normal[0] * w.x + triangle.normal[1] * w.y + triangle.normal[2] * w.z) * facing;
    float3 across = cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
    inputs.geometricNormal = normalize(dot(across, inputs.normal) < 0 ? -across : across);
    inputs.tangent = float4(tangent.xyz * facing, triangle.tangent[0].w);
    inputs.instance = draw.instance;
    inputs.material = draw.material;
    corners.draw = draw;
    corners.index = triangle.index;
    corners.weights = w;
    corners.weightsDx = point.weightsDx;
    corners.weightsDy = point.weightsDy;
    return inputs;
}

// ---------------------------------------------------------------------------------------------------------------------
// Entry points

struct VisibilityVaryings {
    RenderVaryings surface;
    nointerpolation uint draw : TEXCOORD6;
    nointerpolation uint primitive : TEXCOORD7;
};

// A vertex of a draw that is not indexed and starts at vertex zero. `id` then counts the part's indices on every
// device, so the shader pulls the vertex itself and knows its triangle without asking the device for it. It reads the
// geometry alone: a masked material's fragment shader finds the attributes that its alpha reads at its pixel.
[shader("vertex")]
VisibilityVaryings visibilityVertex(uint id : SV_VertexID) {
    PrRenderDrawRecord draw = renderDraw(push.draw);
    PrRenderPartRecord part = renderPart(draw.part);
    VisibilityVaryings out;
    out.surface = renderVaryings(renderVertexPull(part, renderIndexPull(part, id)), push.draw, push.view);
    out.draw = push.draw;
    out.primitive = id / 3;
    return out;
}

// Returns what the pixel shows: one more than the draw's index, so that zero is nothing, and the triangle's index in
// the draw's part.
[shader("fragment")]
uint2 visibilityPixel(VisibilityVaryings input) : SV_Target {
    return uint2(input.draw + 1, input.primitive);
}

// Returns what the pixel shows of a masked material, as visibilityPixel does, where its alpha reaches its cutoff. The
// pixel's point and the attributes of its material's type there are found from the triangle, as the shading finds them,
// so the alpha is the one that the shading would compute.
[shader("fragment")]
uint2 visibilityMaskPixel(VisibilityVaryings input) : SV_Target {
    PrRenderDrawRecord draw = renderDraw(input.draw);
    PrRenderInstanceRecord instance = renderInstance(draw.instance);
    RenderCorners corners;
    RenderSurfaceInputs inputs = trianglePointInputs(draw, instance, input.primitive, input.surface.clip.xy, corners);
    PrRenderMaterialRecord material = renderMaterial(draw.material);
    if (renderMaterialAlphaAt(material, inputs, corners) < material.alphaCutoff)
        discard;
    return uint2(input.draw + 1, input.primitive);
}

[shader("vertex")]
float4 shadeVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns the radiance of what the pixel shows, as scene-referred linear sRGB: the environment's where it shows no
// surface.
[shader("fragment")]
float4 shadePixel(float4 position : SV_Position) : SV_Target {
    uint2 shown = prGpuGet<Texture2D<uint2>>(frame.visibility).Load(int3(int2(position.xy), 0));
    if (shown.x == 0)
        return float4(renderEnvironmentBackground(position.xy) * frame.exposure, 1);
    PrRenderDrawRecord draw = renderDraw(shown.x - 1);
    PrRenderInstanceRecord instance = renderInstance(draw.instance);
    RenderCorners corners;
    RenderSurfaceInputs inputs = trianglePointInputs(draw, instance, shown.y, position.xy, corners);
    PrRenderMaterialRecord material = renderMaterial(draw.material);
    return float4(renderMaterialRadiance(material, inputs, corners).rgb * frame.exposure, 1);
}
