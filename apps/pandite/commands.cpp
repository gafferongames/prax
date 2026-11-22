// Pandite's commands.

#include "apps/pandite/commands.h"

#include "pr/construct.h"
#include "pr/count_of.h"

namespace pandite {

using namespace pr;

namespace {

// The definition of a command: its id, the title that the shortcuts window shows, its chords, its flags, and the
// contexts where it works. If a layout may need Shift to type a printable key, the chord of that key is bound with
// Shift too.
struct CommandSpec {
    const char* id;
    const char* title;
    const char* chords[4];
    PrCommandFlags flags;
    CommandWhere where;
};

// The title of the command that moves files to the platform's trash.
#if defined(_WIN32)
constexpr const char* kTrashTitle = "Move to Recycle Bin";
#else
constexpr const char* kTrashTitle = "Move to Trash";
#endif

// The commands, indexed by Command. The commands of crop mode come last, so that their chords replace the chords of
// other commands in the crop context.
constexpr CommandSpec kSpecs[] = {
    {"file.open", "Open...", {"Cmd O"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.saveAs", "Save as...", {"Cmd S", "Cmd Shift S"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.copyImage", "Copy image", {"Cmd C"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.copyPath", "Copy file path", {"Cmd Shift C"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"select.image", "Select image", {"Space"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"select.clear", "Clear selection", {"Backspace"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"select.previous", "Select and go to previous image", {"Shift Left"}, 0, CommandWhere_Viewer},
    {"select.next", "Select and go to next image", {"Shift Right"}, 0, CommandWhere_Viewer},
    {"file.moveTo", "Move to...", {"M"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.copyTo", "Copy to...", {"Shift M"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.trash", kTrashTitle, {"Delete"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"file.delete", "Delete permanently...", {"Shift Delete"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"view.compare", "Compare selected images", {"D"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"view.peek", "Pixel peek", {"P"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"view.info", "Image information", {"I"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"edit.undo", "Undo edit", {"Cmd Z"}, 0, CommandWhere_Viewer},
    {"edit.rotateLeft", "Rotate left", {"L", "Shift R"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"edit.rotateRight", "Rotate right", {"R"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"edit.flipHorizontal", "Flip horizontally", {"H"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"edit.flipVertical", "Flip vertically", {"V"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"edit.crop", "Crop", {"C"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"edit.resize", "Resize...", {"Cmd R"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"view.previous", "Previous image", {"Left"}, 0, CommandWhere_Viewer},
    {"view.next", "Next image", {"Right"}, 0, CommandWhere_Viewer},
    {"view.first", "First image", {"Home"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"view.last", "Last image", {"End"}, PrCommandFlag_Once, CommandWhere_Viewer},
    {"gallery.pageBack", "Back one gallery page", {"PageUp"}, 0, CommandWhere_Viewer},
    {"gallery.pageForward", "Forward one gallery page", {"PageDown"}, 0, CommandWhere_Viewer},
    {"zoom.fit", "Fit to window", {"F", "0", "Shift 0"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"zoom.actualSize", "Actual size (1:1)", {"1", "Shift 1"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"zoom.in", "Zoom in", {"+", "=", "Shift +"}, 0, CommandWhere_Looks},
    {"zoom.out", "Zoom out", {"-", "Shift -"}, 0, CommandWhere_Looks},
    {"pan.up", "Pan up", {"Up", "Cmd Up"}, 0, CommandWhere_Looks},
    {"pan.down", "Pan down", {"Down", "Cmd Down"}, 0, CommandWhere_Looks},
    {"pan.left", "Pan left", {"Cmd Left"}, 0, CommandWhere_Looks},
    {"pan.right", "Pan right", {"Cmd Right"}, 0, CommandWhere_Looks},
    {"gallery.toggle", "Gallery strip", {"G"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"gallery.larger", "Larger thumbnails", {"Cmd +", "Cmd =", "Cmd Shift +"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"gallery.smaller", "Smaller thumbnails", {"Cmd -"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"window.fullscreen", "Full screen", {"F11", "Alt Enter"}, PrCommandFlag_Once, CommandWhere_Everywhere},
    {"app.contextMenu", "Context menu", {"ContextMenu", "Shift F10"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"app.shortcuts", "Keyboard shortcuts", {"F1", "?", "Shift ?"}, PrCommandFlag_Once, CommandWhere_Looks},
    {"app.quit", "Quit", {"Q", "Cmd Q", "Cmd W", "Escape"}, PrCommandFlag_Once, CommandWhere_Everywhere},
    {"crop.apply", "Apply crop", {"Enter"}, PrCommandFlag_Once, CommandWhere_Crop},
    {"crop.selectAll", "Select whole image", {"Cmd A"}, PrCommandFlag_Once, CommandWhere_Crop},
    {"crop.cancel", "Cancel crop", {"C", "Escape"}, PrCommandFlag_Once, CommandWhere_Crop},
};
static_assert(countOf(kSpecs) == Command_Count, "kSpecs has one entry for each Command");

void run(void* PR_NULLABLE context, const PrCommandArgs*) {
    const CommandEntry& entry = *static_cast<const CommandEntry*>(context);
    entry.commands->handler.run(entry.commands->handler.context, entry.command);
}

PrCommandStateFlags state(void* PR_NULLABLE context) {
    const CommandEntry& entry = *static_cast<const CommandEntry*>(context);
    return entry.commands->handler.state(entry.commands->handler.context, entry.command);
}

// Returns true if a chord binds a letter key without the primary modifier. The text field of a dialog takes such a key.
bool isLetter(PrCommandChord chord) {
    return chord.key >= 'A' && chord.key <= 'Z' && (chord.modifiers & kPrCommandPrimaryModifier) == 0;
}

} // namespace

Commands* commandsCreate(const CommandHandler& handler) {
    Commands* commands = create<Commands>();
    commands->registry = prCommandRegistryCreate();
    commands->keymap = prCommandKeymapCreate();
    commands->handler = handler;
    for (uint32_t i = 0; i < Command_Count; ++i) {
        const CommandSpec& spec = kSpecs[i];
        commands->entries[i] = CommandEntry{.commands = commands, .command = Command(i)};
        PrCommandDesc desc{
            .id = spec.id,
            .title = spec.title,
            .flags = spec.flags,
            .run = run,
            .state = state,
            .context = &commands->entries[i],
        };
        prCommandRegistryAdd(commands->registry, &desc);
        for (const char* text : spec.chords) {
            PrCommandChord chord{};
            if (text == nullptr || !prCommandChordParse(PrStringView(text), &chord))
                continue;
            if ((spec.where & CommandWhere_Viewer) != 0)
                prCommandKeymapBind(commands->keymap, chord, spec.id, "", PrCommandBindingKind_Normal);
            if ((spec.where & CommandWhere_Crop) != 0)
                prCommandKeymapBind(commands->keymap, chord, spec.id, kCropContext, PrCommandBindingKind_Normal);
            if ((spec.where & CommandWhere_Dialog) != 0 && !isLetter(chord) && chord.key != PrAppKey_Escape)
                prCommandKeymapBind(commands->keymap, chord, spec.id, kDialogContext, PrCommandBindingKind_Normal);
        }
    }
    return commands;
}

void commandsDestroy(Commands* PR_NULLABLE commands) {
    if (commands == nullptr)
        return;
    prCommandKeymapDestroy(commands->keymap);
    prCommandRegistryDestroy(commands->registry);
    destroy(commands);
}

PrStringView commandId(Command command) {
    return PrStringView(kSpecs[command].id);
}

PrStringView commandTitle(Command command) {
    return PrStringView(kSpecs[command].title);
}

CommandWhere commandWhere(Command command) {
    return kSpecs[command].where;
}

PrStringView commandContext(Command command) {
    return kSpecs[command].where == CommandWhere_Crop ? kCropContext : PrStringView("");
}

} // namespace pandite
