// The viewer's file actions.

#include "apps/pandite/files.h"

#include "pr/file.h"
#include "pr/text_buffer.h"

namespace pandite {

namespace {

// Returns the path of the file `name` in the directory `directory`.
String pathIn(PrStringView directory, PrStringView name) {
    PrTextBuffer path{};
    prTextBufferAppend(&path, directory);
    prFilePathAppend(&path, name);
    return String::adopt(prTextBufferTake(&path));
}

bool contains(Span<const String> names, PrStringView name) {
    for (const String& n : names) {
        if (n == name)
            return true;
    }
    return false;
}

} // namespace

FileOutcome fileAct(const Folder& folder, Span<const String> names, FileAction action, PrStringView to) {
    FileOutcome outcome{};
    bool sameFolder = action != FileAction_Delete && prFileSame(folder.directory, to);
    for (const String& name : names) {
        String from = pathIn(folder.directory, name);
        PrStatus status = PrStatus_NotAllowed;
        if (action == FileAction_Delete)
            status = prFileRemove(from) ? PrStatus_Ok : PrStatus_Io;
        else if (!sameFolder && action == FileAction_Move)
            status = prFileMove(from, pathIn(to, name));
        else if (!sameFolder)
            status = prFileCopy(from, pathIn(to, name));
        if (status == PrStatus_Ok) {
            outcome.done.push(name);
        } else if (outcome.failed.isEmpty()) {
            outcome.failed = name;
            outcome.failure = status;
        } else {
            outcome.otherFailures += 1;
        }
    }
    return outcome;
}

uint32_t folderRemove(Folder& folder, Span<const String> names, uint32_t current) {
    // The first image that stays at or after the one on screen, and the last that stays before it.
    uint32_t after = kNoImage;
    uint32_t before = kNoImage;
    Array<String> kept;
    for (uint32_t i = 0; i < folder.names.count(); ++i) {
        if (contains(names, folder.names[i]))
            continue;
        if (current != kNoImage && i >= current && after == kNoImage)
            after = kept.count();
        if (current != kNoImage && i < current)
            before = kept.count();
        kept.push(move(folder.names[i]));
    }
    folder.names = move(kept);
    if (after != kNoImage)
        return after;
    return before;
}

} // namespace pandite
