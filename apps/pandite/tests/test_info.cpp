// The information panel's facts and rows: what a file reads as, and how the panel writes sizes, ratios and times.

#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/format.h"
#include "pr/image_select.h"

#include "apps/pandite/edit.h"
#include "apps/pandite/info.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace pandite;
using namespace pr;

namespace {

// Returns the value of the row `label`, or the null string if there is none.
String valueOf(const Array<InfoRow>& rows, const char* label) {
    for (const InfoRow& row : rows) {
        if (row.label == label)
            return row.value;
    }
    return String();
}

} // namespace

TEST_CASE("the panel formats byte counts, megapixels, aspect ratios and exposures for people to read") {
    CHECK(infoBytes(512) == "512 bytes");
    CHECK(infoBytes(2516582) == "2.4 MB");
    CHECK(infoBytes(150 * 1024) == "150 KB");
    CHECK(infoMegapixels(PrUVec2{6000, 4000}) == "24.0 MP");
    CHECK(infoMegapixels(PrUVec2{640, 480}) == "0.31 MP");
    CHECK(infoMegapixels(PrUVec2{10, 10}) == "less than 0.01 MP");
    CHECK(infoAspect(PrUVec2{6000, 4000}) == "3:2 (1.50:1)");
    CHECK(infoAspect(PrUVec2{400, 200}) == "2:1");
    CHECK(infoAspect(PrUVec2{1000, 681}) == "1.47:1");
    CHECK(infoExposure(1, 250) == "1/250 s");
    CHECK(infoExposure(10, 4) == "2.5 s");
    CHECK(infoExposure(3, 1000) == "1/333 s");
    CHECK(infoExposure(0, 1).isEmpty());
}

TEST_CASE("the rows of an image file give its file, pixels and format, and omit facts that the file does not state") {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    Array<uint8_t> pixels(size_t(6) * 4 * 4, 255);
    Bitmap bitmap = bitmapOf(move(pixels), PrUVec2{6, 4});
    String path = format("{}/facts.png", testFolder());
    REQUIRE(bitmapWritePng(images, bitmap, path) == PrStatus_Ok);

    ImageFacts facts{};
    factsRead(images, path, &facts);
    CHECK(facts.opened);
    Array<InfoRow> rows;
    factsRows(facts, &rows);
    CHECK(rows[0].label == "File");
    CHECK(valueOf(rows, "File") == "facts.png");
    CHECK(valueOf(rows, "Pixels") == "6 x 4");
    CHECK(valueOf(rows, "Aspect ratio") == "3:2 (1.50:1)");
    CHECK(valueOf(rows, "Format") == "PNG");
    CHECK(valueOf(rows, "Channels") == "RGBA (4)");
    CHECK(valueOf(rows, "Bits per channel") == "8");
    CHECK(valueOf(rows, "Uncompressed") == "96 bytes");
    // The file has no camera metadata and is not an animation, so those rows are absent.
    CHECK(valueOf(rows, "Camera").isEmpty());
    CHECK(valueOf(rows, "Frames").isEmpty());
    for (const InfoRow& row : rows)
        CHECK(!row.value.isEmpty());

    // A file that is not an image has only the rows of its file.
    String text = format("{}/notes.png", testFolder());
    REQUIRE(prFileWrite(text, PrByteView{reinterpret_cast<const uint8_t*>("hello"), 5}));
    factsRead(images, text, &facts);
    CHECK(!facts.opened);
    rows.clear();
    factsRows(facts, &rows);
    CHECK(valueOf(rows, "File size") == "5 bytes");
    CHECK(valueOf(rows, "Pixels").isEmpty());
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
}
