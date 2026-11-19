// lib/render's depth: the vertex shader of the draws that write only a view's depth, for the materials that are opaque
// everywhere.

#include "pr/render_scene.slang"

// A mesh's vertex, of which the depth reads the position alone.
struct DepthVertex {
    PR_GPU_ATTRIBUTE(float3, position, 0);
};

// Returns the position of a vertex of the push data's draw in the push data's view.
[shader("vertex")]
float4 depthVertex(DepthVertex vertex) : SV_Position {
    PrRenderDrawRecord draw = renderDraw(push.draw);
    return renderClipPoint(push.view, renderWorldPoint(renderInstance(draw.instance), vertex.position));
}
