// The viewer's file actions: moves and copies that never replace a file, deletes, and the image shown after a removal.

#include "pr/file.h"

#include "apps/pandite/files.h"

#include "tools/harness/test.h"

using namespace pandite;
using namespace pr;

namespace {

String pathOf(PrStringView name) {
    return format("{}/files/{}", PANDITE_TEST_DIR, name);
}

void write(PrStringView path, PrStringView text) {
    REQUIRE(prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size}));
}

Folder folderOf(const char* const* names, uint32_t count) {
    Folder folder{.directory = pathOf("from")};
    for (uint32_t i = 0; i < count; ++i)
        folder.names.push(String(names[i]));
    return folder;
}

} // namespace

TEST_CASE("a move or a copy to another folder does not replace a file there, and a delete removes the files") {
    REQUIRE(prFileCreateDirectory(pathOf("from")));
    REQUIRE(prFileCreateDirectory(pathOf("to")));
    const char* names[] = {"a.png", "b.png", "c.png"};
    for (const char* name : names)
        write(pathOf(format("from/{}", name)), name);
    write(pathOf("to/b.png"), "taken");
    Folder folder = folderOf(names, 3);

    // b.png has a file of its name in the destination, so it stays where it is.
    FileOutcome moved = fileAct(folder, folder.names.span(), FileAction_Move, pathOf("to"));
    CHECK(moved.done == Array<String>({"a.png", "c.png"}));
    CHECK(moved.failed == "b.png");
    CHECK(moved.failure == PrStatus_NotAllowed);
    CHECK(moved.otherFailures == 0);
    PrFileInfo info{};
    CHECK(prFileInfo(pathOf("to/a.png"), &info));
    CHECK(!prFileInfo(pathOf("from/a.png"), &info));
    CHECK(prFileInfo(pathOf("from/b.png"), &info));

    // A copy leaves the original. A copy to the folder that holds the files copies nothing.
    String kept[] = {"b.png"};
    FileOutcome copied = fileAct(folder, Span<const String>(kept), FileAction_Copy, pathOf("to/../from"));
    CHECK(copied.done.count() == 0);
    CHECK(copied.failure == PrStatus_NotAllowed);
    CHECK(prFileRemove(pathOf("to/b.png")));
    copied = fileAct(folder, Span<const String>(kept), FileAction_Copy, pathOf("to"));
    CHECK(copied.done == Array<String>({"b.png"}));
    CHECK(prFileInfo(pathOf("from/b.png"), &info));

    FileOutcome deleted = fileAct(folder, Span<const String>(kept), FileAction_Delete, "");
    CHECK(deleted.done.count() == 1);
    CHECK(!prFileInfo(pathOf("from/b.png"), &info));
    for (const char* name : names)
        CHECK(prFileRemove(pathOf(format("to/{}", name))));
    CHECK(prFileRemove(pathOf("to")));
    CHECK(prFileRemove(pathOf("from")));
}

TEST_CASE("a removal keeps the image on screen if it stays, else shows the next that stays, else the one before") {
    const char* names[] = {"a", "b", "c", "d", "e"};
    String some[] = {"b", "d"};
    Folder folder = folderOf(names, 5);
    // c stays where it was, now at 1.
    CHECK(folderRemove(folder, Span<const String>(some), 2) == 1);
    CHECK(folder.names == Array<String>({"a", "c", "e"}));
    folder = folderOf(names, 5);
    // d goes, and e takes its place.
    CHECK(folderRemove(folder, Span<const String>(some), 3) == 2);
    String tail[] = {"d", "e"};
    folder = folderOf(names, 5);
    // The last two go, and the one before them shows.
    CHECK(folderRemove(folder, Span<const String>(tail), 4) == 2);
    String all[] = {"a", "b", "c", "d", "e"};
    folder = folderOf(names, 5);
    CHECK(folderRemove(folder, Span<const String>(all), 0) == kNoImage);
    CHECK(folder.names.count() == 0);
}
