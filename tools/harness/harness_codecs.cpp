#include "tools/harness/harness_codecs.h"

#include "pr/compress_select.h"
#include "pr/image_select.h"

namespace harness {

const PrCompressCodecSet* harnessCompress() {
    // A static local is created once, by whichever thread first calls.
    static PrCompressCodecSet* set = prCompressCodecSetCreateDefault();
    return set;
}

const PrImageCodecSet* harnessImages() {
    static PrImageCodecSet* set = prImageCodecSetCreateDefault(harnessCompress());
    return set;
}

} // namespace harness
