#include "pr/texture.h"

int prTestTextureCApi(void);

// The public headers compiled as C. A level's extent halves to one texel, a conversion keeps the channels by default,
// and a stored pixel of four 16-bit channels is 8 bytes.
int prTestTextureCApi(void) {
    PrUVec2 extent = prTextureLevelExtent((PrUVec2){12, 8}, 3);
    int result = extent.x != 1 || extent.y != 1;
    PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba8Srgb);
    result |= convert.channels[3] != 3 || convert.format != PrGpuFormat_Rgba8Srgb;
    result |= prTexturePixelSize(PrTextureChannels_Rgba, PrTextureSample_U16) != 8;
    return result;
}
