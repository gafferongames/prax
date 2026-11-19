// lib/render's forward technique: a draw that computes the material of each visible point and shades it in one fragment
// shader. A shader set's source of the pass includes this file, with a vertex shader for each layout of its material
// types' attributes and a fragment shader for each type (render_shaders_check.cmake), each of which shades its type by
// renderForwardPixel.

#include "render_shader_set.slang"

// Returns what a fragment of `material` draws for the radiance and the alpha that its surface was shaded to: the
// radiance as scene-referred linear sRGB times the view's exposure, premultiplied by its alpha. A material that does
// not blend has an alpha of 1. A masked material's fragment is dropped where its alpha is below the cutoff.
float4 renderForwardColor(PrRenderMaterialRecord material, float4 shaded) {
    uint mode = material.flags & kPrRenderMaterialAlphaMode;
    if (mode == kPrRenderMaterialMask && shaded.a < material.alphaCutoff)
        discard;
    float alpha = mode == kPrRenderMaterialBlend ? shaded.a : 1.0;
    return float4(shaded.rgb * (alpha * frame.exposure), alpha);
}

// Returns what a fragment draws of a material of the type M, with the attributes that it interpolated and the textures
// that the build fills for the type: the material's surface, shaded by the type's shading model. It is inlined so that
// a type's entry point is its body.
[ForceInline]
float4 renderForwardPixel<M : IRenderMaterial>(RenderSurfaceInputs inputs, M.Attributes attributes, M.Textures textures) {
    PrRenderMaterialRecord material = renderMaterial(inputs.material);
    return renderForwardColor(material, renderShadeAlpha(renderMaterialSurface<M>(material, inputs, attributes, textures),
        inputs, renderMaterialEmission(material)));
}
