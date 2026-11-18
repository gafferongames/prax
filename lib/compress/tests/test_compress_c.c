#include "pr/compress_brotli.h"
#include "pr/compress_deflate.h"
#include "pr/compress_lzfse.h"

int prTestCompressCApi(void);

// The public headers compiled as C. A stored Deflate block decodes through a set of the Deflate codec alone.
int prTestCompressCApi(void) {
    const uint8_t stored[] = {0x01, 0x02, 0x00, 0xfd, 0xff, 'h', 'i'};
    uint8_t out[2] = {0};
    PrCompressRead read;
    PrByteView input = {stored, sizeof(stored)};
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* set = prCompressCodecSetCreate(&deflate, 1);
    PrStatus status = prCompressCodecSetDecode(set, PrCompressFormat_Deflate, input, out, sizeof(out), &read);
    int result = status != PrStatus_Ok || read.progress != PrCompressProgress_Done || out[1] != 'i';
    result |= prCompressCodecSetDecodes(set, PrCompressFormat_Brotli);
    prCompressCodecSetDestroy(set);
    prCompressCodecDestroy(NULL);
    prCompressDecoderDestroy(NULL);
    // The other codecs' constructors are C too.
    prCompressCodecDestroy(prCompressBrotliCreate());
    prCompressCodecDestroy(prCompressLzfseCreate());
    return result;
}
