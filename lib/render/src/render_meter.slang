// lib/render's meter: a draw over a target of kPrRenderMeterGrid squared texels, one for each cell of a grid across a
// view's color, which holds what the cell's pixels show of the scene's luminance (PrRenderMeterPush).

#include "pr/gpu.slang"
#include "pr/render_records.h"
#include "pr/render_whole.slang"

PR_GPU_PUSH(PrRenderMeterPush, push);

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(Texture2D<float4>, renderSlotColor, 0);

// Returns the view's color at a texel, as scene-referred linear sRGB times the view's exposure.
float3 viewColor(int2 texel) {
    return renderSlotColor.Load(int3(texel, 0)).rgb;
}

#else

float3 viewColor(int2 texel) {
    return prGpuGet<Texture2D<float4>>(push.color).Load(int3(texel, 0)).rgb;
}

#endif

[shader("vertex")]
float4 meterVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns, in red, the mean of the base-2 logarithm of the luminance in nits of the cell's pixels that have light, and
// in green the share of the cell's pixels that have it. The cell reads kPrRenderMeterSamples squared pixels, at the
// middles of as many equal parts of the cell.
[shader("fragment")]
float4 meterPixel(float4 position : SV_Position) : SV_Target {
    float2 cell = floor(position.xy);
    float2 extent = float2(push.width, push.height);
    float sum = 0;
    float lit = 0;
    for (uint y = 0; y < kPrRenderMeterSamples; ++y) {
        for (uint x = 0; x < kPrRenderMeterSamples; ++x) {
            float2 within = (float2(x, y) + 0.5) / float(kPrRenderMeterSamples);
            int2 texel = int2((cell + within) / float(kPrRenderMeterGrid) * extent);
            float stored = dot(max(viewColor(texel), 0), float3(0.2126, 0.7152, 0.0722));
            if (stored > 0) {
                sum += log2(stored / push.exposure);
                lit += 1;
            }
        }
    }
    float mean = lit > 0 ? sum / lit : 0;
    return float4(mean, lit / float(kPrRenderMeterSamples * kPrRenderMeterSamples), 0, 1);
}
