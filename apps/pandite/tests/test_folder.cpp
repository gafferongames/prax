// A folder's images: the files that decode, in natural order, and where a name stands among them.

#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/image_select.h"

#include "apps/pandite/folder.h"

#include "tools/harness/test.h"

using namespace pandite;
using namespace pr;

namespace {

// The codecs of the build. The names of the folder are checked against them.
struct Codecs {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);

    ~Codecs() {
        prImageCodecSetDestroy(images);
        prCompressCodecSetDestroy(compress);
    }
};

String pathOf(PrStringView name) {
    return format("{}/folder/{}", PANDITE_TEST_DIR, name);
}

} // namespace

TEST_CASE("a folder lists its image files in natural order, and leaves out other files") {
    Codecs codecs;
    REQUIRE(prFileCreateDirectory(pathOf("inner")));
    const char* names[] = {"img10.png", "img2.PNG", "Beach.jpg", "apple.gif", "notes.txt", "noext"};
    for (const char* name : names)
        REQUIRE(prFileWrite(pathOf(name), PrByteView{reinterpret_cast<const uint8_t*>("x"), 1}));
    Folder folder;
    REQUIRE(folderList(pathOf(""), codecs.images, &folder));
    REQUIRE(folder.names.count() == 4);
    CHECK(folder.names[0] == "apple.gif");
    CHECK(folder.names[1] == "Beach.jpg");
    CHECK(folder.names[2] == "img2.PNG");
    CHECK(folder.names[3] == "img10.png");
    CHECK(folderFind(folder, "img2.PNG") == 2);
    CHECK(folderFind(folder, "missing.png") == kNoImage);
    // A name that left the folder stands where it sorted, or at the last image.
    CHECK(folderNearest(folder, "img5.png") == 3);
    CHECK(folderNearest(folder, "zebra.png") == 3);
    CHECK(folderNearest(folder, "aardvark.png") == 0);
    CHECK(folderPath(folder, 1) == pathOf("Beach.jpg"));
    for (const char* name : names)
        CHECK(prFileRemove(pathOf(name)));
    CHECK(prFileRemove(pathOf("inner")));
    CHECK(!folderList(pathOf("missing"), codecs.images, &folder));
    CHECK(folder.names.count() == 0);
}
