#include "pr/compress_deflate.h"
#include "pr/zip.h"

int prTestZipCApi(void);

// The public header compiled as C. Bytes that are not an archive are refused, and leave no entries.
int prTestZipCApi(void) {
    PrCompressCodec* deflate = prCompressDeflateCreate();
    PrCompressCodecSet* compress = prCompressCodecSetCreate(&deflate, 1);
    PrZip* archive = prZipCreate(compress);
    static const uint8_t junk[] = {1, 2, 3, 4, 5};
    PrByteView bytes = {junk, sizeof(junk)};
    int result = prZipOpen(archive, bytes) != PrStatus_InvalidData || prZipEntryCount(archive) != 0;
    prZipDestroy(archive);
    prCompressCodecSetDestroy(compress);
    return result;
}
