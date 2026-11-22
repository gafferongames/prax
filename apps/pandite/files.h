// The viewer's file actions on the images of a folder: moving or copying some of them to another folder, and deleting
// them. Also removes from the folder's list the files that left the folder.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/span.h"
#include "pr/status.h"
#include "pr/string.h"

#include "apps/pandite/folder.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The outcome of an action on some files. `done` holds the names that the action took from the folder or copied.
// `failed` is the first name that the action failed on, and `failure` is why. `otherFailures` is the number of other
// names that the action failed on.
struct FileOutcome {
    Array<String> done;
    String failed;
    PrStatus failure;
    uint32_t otherFailures;
};

// What an action does to each file.
enum : uint8_t {
    FileAction_Move,                // to another folder, never replacing a file there
    FileAction_Copy,                // to another folder, never replacing a file there
    FileAction_Delete,              // permanently
};
typedef uint8_t FileAction;

// Runs `action` on the files `names` of `folder`. `to` is the folder that a move or a copy goes to. Each file is
// handled on its own, so a failure does not affect the other files. A move or a copy to the folder that already holds
// the files does nothing and fails each file with NotAllowed.
FileOutcome fileAct(const Folder& folder, Span<const String> names, FileAction action, PrStringView to);

// Removes `names` from the list of `folder`. Returns the index of the image to show in place of the image at `current`.
// That is the same image if it stays. Otherwise it is the next image that stays, or the previous one if no image after
// it stays. Returns kNoImage if no image stays.
uint32_t folderRemove(Folder& folder, Span<const String> names, uint32_t current);

} // namespace pandite

PR_ASSUME_NONNULL_END
