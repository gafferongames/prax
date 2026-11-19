// A triangle that covers a whole target, for a draw of three vertices without vertex buffers.

#pragma once

// Returns the clip space position of the vertex `id` of the triangle.
float4 renderWholeTriangle(uint id) {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}
