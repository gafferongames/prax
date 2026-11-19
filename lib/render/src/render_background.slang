// lib/render's background: a draw over a whole target that shows the scene's environment wherever a view's depth holds
// no surface. The forward technique issues it between its opaque draws and its blended ones.

#include "pr/render_environment.slang"
#include "pr/render_whole.slang"

// The triangle lies at the far plane, where the depth test keeps it only behind every surface.
[shader("vertex")]
float4 backgroundVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns the environment's radiance behind the pixel, as scene-referred linear sRGB times the view's exposure.
[shader("fragment")]
float4 backgroundPixel(float4 position : SV_Position) : SV_Target {
    return float4(renderEnvironmentBackground(position.xy) * frame.exposure, 1);
}
