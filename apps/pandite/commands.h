// Pandite's commands. Each action of the viewer is a command with an id in a registry (pr/command.h). The keyboard, the
// toolbar, the keyboard shortcuts window and automation all run commands through the registry. A keymap binds chords to
// the commands.
//
// The keymap binds the chords of each command in the contexts where the command works. There are three contexts: the
// viewer, crop mode and a dialog. Crop mode has commands of its own. It also keeps the viewer commands that neither
// change the image nor leave it. A dialog keeps only full screen and quit. The dialog takes letters and Escape, so it
// does not bind the chords that use them.

#pragma once

#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

// The viewer's commands.
enum : uint8_t {
    Command_Open,
    Command_SaveAs,
    Command_CopyImage,
    Command_CopyPath,
    Command_SelectImage,
    Command_ClearSelection,
    Command_SelectPrevious,
    Command_SelectNext,
    Command_MoveTo,
    Command_CopyTo,
    Command_Trash,
    Command_Delete,
    Command_Compare,
    Command_Peek,
    Command_ToggleInfo,
    Command_Undo,
    Command_RotateLeft,
    Command_RotateRight,
    Command_FlipHorizontal,
    Command_FlipVertical,
    Command_Crop,
    Command_Resize,
    Command_Previous,
    Command_Next,
    Command_First,
    Command_Last,
    Command_PageBack,
    Command_PageForward,
    Command_Fit,
    Command_ActualSize,
    Command_ZoomIn,
    Command_ZoomOut,
    Command_PanUp,
    Command_PanDown,
    Command_PanLeft,
    Command_PanRight,
    Command_ToggleGallery,
    Command_LargerThumbnails,
    Command_SmallerThumbnails,
    Command_Fullscreen,
    Command_ContextMenu,
    Command_Shortcuts,
    Command_Quit,
    Command_CropApply,
    Command_CropSelectAll,
    Command_CropCancel,
    Command_Count,
};
typedef uint8_t Command;

// The names of the keymap contexts for crop mode and for a dialog.
constexpr PrStringView kCropContext = "crop";
constexpr PrStringView kDialogContext = "dialog";

// The contexts where a command works.
enum : uint8_t {
    CommandWhere_Viewer = 1u << 0,
    CommandWhere_Crop = 1u << 1,
    CommandWhere_Dialog = 1u << 2,
    CommandWhere_Looks = CommandWhere_Viewer | CommandWhere_Crop,   // a command that only views the image
    CommandWhere_Everywhere = CommandWhere_Viewer | CommandWhere_Crop | CommandWhere_Dialog,
};
typedef uint8_t CommandWhere;

// The callbacks that the viewer supplies for its commands. `run` runs a command. `state` returns the state of a
// command.
struct CommandHandler {
    void (*run)(void* PR_NULLABLE context, Command command);
    PrCommandStateFlags (*state)(void* PR_NULLABLE context, Command command);
    void* PR_NULLABLE context;
};

struct Commands;

// The context that the registry holds for one command. It names the `Commands` that owns the command, and the command.
struct CommandEntry {
    Commands* commands;
    Command command;
};

// The viewer's commands and their chords.
struct Commands {
    PrCommandRegistry* registry;
    PrCommandKeymap* keymap;
    CommandHandler handler;
    CommandEntry entries[Command_Count];
};

// Creates and destroys the commands. The commands run through `handler`.
Commands* commandsCreate(const CommandHandler& handler);
void commandsDestroy(Commands* PR_NULLABLE commands);

// Returns the id of `command`, and the title that the shortcuts window shows for it.
PrStringView commandId(Command command);
PrStringView commandTitle(Command command);

// Returns the contexts where `command` works.
CommandWhere commandWhere(Command command);

// Returns the context that the shortcuts window and the menus use to find the chords of `command`. It is the crop
// context for a command that works only in crop mode, and the viewer context otherwise.
PrStringView commandContext(Command command);

} // namespace pandite

PR_ASSUME_NONNULL_END
