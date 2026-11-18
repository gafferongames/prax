#include "pr/compress_deflate.h"
#include "pr/image_emjc.h"
#include "pr/image_gif.h"
#include "pr/image_ico.h"
#include "pr/image_jpeg.h"
#include "pr/image_png.h"
#include "pr/image_tiff.h"

int prTestImageCodecsCApi(void);

// The codecs' headers compiled as C. An encoded PNG decodes back through a set of the PNG codec and the Deflate codec.
int prTestImageCodecsCApi(void) {
    const uint8_t pixel[4] = {10, 20, 30, 255};
    uint8_t png[128];
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* compress = prCompressCodecSetCreate(&deflate, 1);
    PrImageCodec* codecs[] = {
        prImagePngCreate(compress), prImageJpegCreate(), prImageGifCreate(), prImageIcoCreate(),
        prImageTiffCreate(), prImageEmjcCreate(compress),
    };
    PrImageCodecSet* set = prImageCodecSetCreate(codecs, sizeof(codecs) / sizeof(codecs[0]));
    size_t size = 0;
    int result = prImageCodecSetEncode(set, PR_SV("image/png"), (PrPixelView){pixel, 4, {1, 1}}, png, sizeof(png),
        &size) != PrStatus_Ok || size > sizeof(png);
    if (result == 0) {
        PrImageSource source = {0};
        source.memory.data = png;
        source.memory.size = size;
        PrImageDecoder* decoder = prImageDecoderCreate(set);
        result = prImageDecoderOpen(decoder, source, NULL) != PrStatus_Ok;
        uint8_t out[4] = {0};
        if (result == 0)
            result |= prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity,
                (PrPixels){out, 4, {1, 1}}) != PrStatus_Ok || out[1] != 20;
        prImageDecoderDestroy(decoder);
    }
    prImageCodecSetDestroy(set);
    prCompressCodecSetDestroy(compress);
    return result;
}
