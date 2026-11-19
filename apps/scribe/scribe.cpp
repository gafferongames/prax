// Scribe's interface: the editor's documents and commands, Scribe's own commands, the menu bar, the palette and the
// status line.

#include "apps/scribe/scribe.h"

#include "pr/color.h"
#include "pr/command_kit.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/hash.h"
#if defined(SCRIBE_HAS_TREESITTER)
#include "pr/syntax_treesitter_languages.h"
#endif
#include "pr/kit_menu.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/url_file.h"

#include "apps/scribe/workspace.h"

namespace scribe {

using namespace pr;

namespace {

// The height of the status line, in px.
constexpr float kStatusHeight = 24;

Scribe& scribeOf(void* context) {
    return *static_cast<Scribe*>(context);
}

// Adds one of Scribe's own commands in `category`, bound to `chord` if it is not empty. An Override binding takes
// precedence over the view that has focus. A terminal needs that, because it binds every chord that it can send to its
// program.
void addCommand(Scribe& s, const char* id, const char* title, const char* chord, PrCommandRunFn run,
        const char* category = "Scribe", PrCommandBindingKind kind = PrCommandBindingKind_Normal) {
    PrCommandDesc desc{.id = id, .title = title, .category = category, .run = run, .context = &s};
    prCommandRegistryAdd(s.commands, &desc);
    PrCommandChord parsed{};
    if (chord[0] != 0 && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(s.keymap, parsed, id, "", kind);
}

// The terminal that shows in the panel, or null if the panel has no terminals.
PrTerminal* shownTerminal(Scribe& s) {
    return s.terminal < s.terminals.count() ? s.terminals[s.terminal] : nullptr;
}

// Returns true if the terminal that shows had focus in the last frame.
bool terminalHasFocus(Scribe& s) {
    PrTerminal* terminal = shownTerminal(s);
    return terminal != nullptr && prTerminalHasFocus(terminal);
}

// Gives focus to the editor's text, when the panel hides or its last terminal closes.
void focusText(Scribe& s) {
    PrLayoutNode text = prTextEditorTextNode(s.editor);
    if (text != kPrLayoutNodeNone)
        prUiSetFocus(s.ui, text);
}

// Closes the terminal that shows, and shows the next one, or the previous one if it was last. Hides the panel if no
// terminals remain.
void closeTerminal(Scribe& s) {
    PrTerminal* terminal = shownTerminal(s);
    if (terminal == nullptr)
        return;
    prTerminalDestroy(terminal);
    s.terminals.erase(s.terminal);
    if (s.terminals.isEmpty()) {
        s.terminalShown = false;
        s.terminal = 0;
        focusText(s);
        return;
    }
    if (s.terminal >= s.terminals.count())
        s.terminal = s.terminals.count() - 1;
    prTerminalFocus(s.terminals[s.terminal]);
}

// Shows the terminal `step` places after the one that shows, wrapping around.
void cycleTerminal(Scribe& s, int32_t step) {
    uint32_t count = s.terminals.count();
    if (count == 0)
        return;
    s.terminal = uint32_t((int64_t(s.terminal) + step + count) % count);
    s.terminalShown = true;
    prTerminalFocus(s.terminals[s.terminal]);
}

// Shows the file named by a terminal's link at the link's line and column. Opens the file if no document holds it.
void openTerminalFile(void* context, const PrTerminalFileLink* link) {
    Scribe& s = scribeOf(context);
    PrTextEditorDocument* document = prTextEditorDocumentFromPath(s.editor, link->path);
    if (document == nullptr) {
        PrTextEditorDocumentDesc desc{.path = link->path};
        if (prTextEditorDocumentCreate(s.editor, &desc, &document) != PrStatus_Ok)
            return;
    }
    // The link counts from 1, and the editor from 0.
    uint32_t line = link->line > 0 ? link->line - 1 : 0;
    uint32_t column = link->column > 0 ? link->column - 1 : 0;
    prTextEditorShowAt(s.editor, document, line, column, true);
}

// Adds the commands of the terminal panel.
void addTerminalCommands(Scribe& s) {
    const char* category = "Terminal";
    PrCommandBindingKind override = PrCommandBindingKind_Override;
    addCommand(s, "terminal.toggle", "Toggle Terminal", "Ctrl `", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        if (s.terminals.isEmpty()) {
            openTerminal(s);
        } else if (s.terminalShown && terminalHasFocus(s)) {
            s.terminalShown = false;
            focusText(s);
        } else {
            s.terminalShown = true;
            prTerminalFocus(shownTerminal(s));
        }
    }, category, override);
    addCommand(s, "terminal.new", "New Terminal", "Ctrl Shift `", [](void* c, const PrCommandArgs*) {
        openTerminal(scribeOf(c));
    }, category, override);
    addCommand(s, "terminal.close", "Close Terminal", "", [](void* c, const PrCommandArgs*) {
        closeTerminal(scribeOf(c));
    }, category);
    addCommand(s, "terminal.next", "Next Terminal", "Ctrl PageDown", [](void* c, const PrCommandArgs*) {
        cycleTerminal(scribeOf(c), 1);
    }, category, override);
    addCommand(s, "terminal.previous", "Previous Terminal", "Ctrl PageUp", [](void* c, const PrCommandArgs*) {
        cycleTerminal(scribeOf(c), -1);
    }, category, override);

    // Commands of the shell integration, which act on the terminal that shows. While the terminal has focus, its own
    // keys scroll to its prompts.
    addCommand(s, "terminal.copyCommandOutput", "Copy Last Command Output", "", [](void* c, const PrCommandArgs*) {
        if (PrTerminal* terminal = shownTerminal(scribeOf(c)))
            prTerminalCopyCommandOutput(terminal);
    }, category);
    addCommand(s, "terminal.selectCommandOutput", "Select Command Output", "", [](void* c, const PrCommandArgs*) {
        if (PrTerminal* terminal = shownTerminal(scribeOf(c)))
            prTerminalSelectCommandOutput(terminal);
    }, category);
    addCommand(s, "terminal.previousPrompt", "Scroll to Previous Prompt", "", [](void* c, const PrCommandArgs*) {
        if (PrTerminal* terminal = shownTerminal(scribeOf(c)))
            prTerminalScrollToPrompt(terminal, PrTerminalDirection_Previous);
    }, category);
    addCommand(s, "terminal.nextPrompt", "Scroll to Next Prompt", "", [](void* c, const PrCommandArgs*) {
        if (PrTerminal* terminal = shownTerminal(scribeOf(c)))
            prTerminalScrollToPrompt(terminal, PrTerminalDirection_Next);
    }, category);
}

// Returns the number of terminals with an alert that the user has not seen: a bell, or a notification. The terminal
// that shows in the panel is not counted, unless the panel is hidden.
uint32_t alertingTerminals(Scribe& s) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < s.terminals.count(); ++i) {
        bool shown = s.terminalShown && i == s.terminal;
        if (!shown && prTerminalAlerts(s.terminals[i]) != 0)
            ++count;
    }
    return count;
}

// Declares the terminal panel: a header line and the terminal that shows below it. The header names the terminal and
// counts the other terminals that need attention. The panel takes its preferred height, but leaves at least a third of
// the window to the text.
void buildTerminalPanel(Scribe& s, PrLayoutNode root) {
    PrTerminal* terminal = shownTerminal(s);
    if (!s.terminalShown || terminal == nullptr)
        return;
    String header = format("Terminal {} of {}: {}", s.terminal + 1, s.terminals.count(), prTerminalTitle(terminal));
    PrProcessInfo info = prTerminalProcessInfo(terminal);
    PrVtermShellInfo shell = prTerminalShellInfo(terminal);
    if (prTerminalLaunchStatus(terminal) != PrStatus_Ok)
        header.append("  (cannot start)");
    else if (info.exited)
        header.append("  (exited)");
    else if (shell.state != PrVtermShellState_Running && shell.lastStatus == PrVtermCommandStatus_Failed)
        header.append(format("  (last command exited with {})", shell.lastExitCode));
    uint32_t alerting = alertingTerminals(s);
    if (alerting != 0)
        formatTo(header, "   {} more {} attention", alerting, alerting == 1 ? "needs" : "need");
    PrKitBoxStyle panelStyle{
        .place = {
            .width = prKitPercent(100),
            .height = prKitPx(s.terminalHeight),
            .maxHeight = prKitPercent(66),
            .flex = PrKitFlex_Fixed,
        },
    };
    PrLayoutNode panel = prKitBox(s.kit, root, {}, panelStyle);
    prKitLabel(s.kit, panel, {.text = header}, {.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted});
    // The terminal's view paints its own background, below the header, in the rest of the panel.
    PrKitBoxStyle screen{.place = {.width = prKitPercent(100), .flex = PrKitFlex_Fill}};
    PrLayoutNode box = prKitBox(s.kit, panel, {}, screen);
    prTerminalFrame(terminal, s.kit, box, PrTerminalBackground_View);
}

// A menu of `items`.
template <uint32_t N>
Menu menuOf(PrStringView title, const PrStringView (&items)[N]) {
    return Menu{title, Array<PrStringView>(items)};
}

// Scribe's menus.
Array<Menu> scribeMenus() {
    Array<Menu> menus;
    menus.push(menuOf("File", {
        "file.openFolder", "file.openFile", "", "file.save", "", "tab.close", "", "view.palette", "", "app.quit",
    }));
    menus.push(menuOf("Edit", {
        "edit.undo", "edit.redo", "", "edit.cut", "edit.copy", "edit.paste", "", "edit.selectAll",
    }));
    menus.push(menuOf("Selection", {"cursor.addAbove", "cursor.addBelow", "cursor.single"}));
    menus.push(menuOf("View", {
        "view.toggleProject", "view.toggleOutline", "", "view.toggleWrap", "view.lineNumbers", "", "view.directionLtr",
        "view.directionRtl", "view.directionAuto", "", "view.horizontal", "view.verticalRl", "view.verticalLr",
    }));
    menus.push(menuOf("Go", {
        "go.back", "go.forward", "", "language.definition", "language.hover", "", "language.nextProblem",
        "language.previousProblem",
    }));
    menus.push(menuOf("Compare", {
        "diff.compareSaved", "diff.nextChange", "diff.previousChange", "", "diff.toggleSync", "diff.clearSyncs", "",
        "diff.close",
    }));
    menus.push(menuOf("Debug", {
        "debug.continue", "debug.stop", "", "debug.stepOver", "debug.stepInto", "debug.stepOut", "",
        "debug.toggleBreakpoint",
    }));
    return menus;
}

// Declares the menu bar: the menus, and the buttons of the history after them. Returns true if a menu is open.
bool buildMenus(Scribe& s, PrLayoutNode root) {
    PrLayoutNode menuBar = prKitMenuBar(s.kit, root, {});
    bool open = false;
    for (const Menu& menu : s.menus) {
        PrLayoutNode title = prKitMenuBarItem(s.kit, menuBar, {.text = menu.title});
        PrLayoutNode popover = prKitPopover(s.kit, title, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        open = true;
        PrLayoutNode list = prKitMenu(s.kit, popover, {}, {.minWidth = 240});
        for (PrStringView id : menu.items) {
            if (id.isEmpty())
                prKitMenuSeparator(s.kit, list, {});
            else
                prCommandMenuItem(s.kit, list, s.commands, s.keymap, id, {});
        }
    }
    // The bar stretches its children to its height, and the row centers the buttons in it.
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .padding = prKitEdgesXY(0, 4)};
    PrLayoutNode history = prKitBox(s.kit, menuBar, {}, row);
    PrKitButtonStyle ghost{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    prCommandButton(s.kit, history, s.commands, s.keymap, "go.back", {.icon = PrStudioIconKind_ArrowLeft}, ghost);
    prCommandButton(s.kit, history, s.commands, s.keymap, "go.forward", {.icon = PrStudioIconKind_ArrowRight}, ghost);
    return open;
}

// The status line: the document's file and state, the caret, and what the editor last had to say.
String statusLine(Scribe& s) {
    // A tab that does not show its text says what it shows.
    ViewTab* tab = shownTab(s);
    if (tab != nullptr && tab->mode != ViewMode_Source) {
        String line = format("{}   {}", displayAddress(s, tab->address), prMimeFormatInfo(tab->format).name);
        if (!tab->selection.isEmpty())
            formatTo(line, "   selected {}", tab->selection);
        return line;
    }
    PrTextEditorDocument* document = prTextEditorShown(s.editor);
    if (document == nullptr)
        return "no document";
    PrTextEditorDocumentInfo info = prTextEditorDocumentInfo(document);
    PrTextViewInfo view = prTextViewInfo(prTextEditorDocumentView(document));
    PrTextEditorOptions options = prTextEditorOptions(s.editor);
    String state(info.edited ? "edited" : "saved");
    if (info.loading)
        state = format("loading {}%", int(info.loaded * 100));
    String language("plain text");
    if (const PrSyntaxLanguage* named = prTextEditorDocumentLanguage(document)) {
        language = named->name;
        if (info.parsing)
            language.append(", parsing");
    }
    const char* servers[] = {"", ", starting server", "", ", server failed"};
    language.append(servers[info.server]);
    if (info.errors != 0 || info.warnings != 0)
        formatTo(language, "  {} errors, {} warnings", info.errors, info.warnings);
    String path = prTextEditorDocumentPath(document);
    if (path.isEmpty() && tab != nullptr && tab->document == document) {
        // A nested resource's text has no file, and its tab's address says where it is.
        path = displayAddress(s, tab->address);
    } else if (path.isEmpty()) {
        path = "untitled";
    } else {
        String url = String::adopt(prUrlFromFilePath(path));
        String shown = displayAddress(s, url);
        if (shown != url)
            path = shown;
    }
    PrStringView wrap = options.wrap ? "wrap" : "no wrap";
    PrStringView endings = info.crlf ? "  CRLF" : "  LF";
    String line = format("{}   {}  Ln {}, Col {}  {} lines  {}{}  {}", path, state, view.caret.line + 1,
        view.caret.column + 1, view.lineCount, wrap, endings, language);
    PrTextEditorDiff* diff = prTextEditorShownDiff(s.editor);
    if (diff != nullptr && prTextEditorDiffIsMerge(diff)) {
        uint32_t conflicts = prTextEditorDiffConflictCount(diff);
        formatTo(line, "   {} {}", conflicts, conflicts == 1 ? "conflict" : "conflicts");
    }
    PrStringView said = prTextEditorStatus(s.editor);
    if (!said.isEmpty())
        formatTo(line, "   {}", said);
    // While the panel shows, its header reports the terminals that need attention instead.
    uint32_t alerting = s.terminalShown ? 0 : alertingTerminals(s);
    if (alerting != 0)
        formatTo(line, "   {} {} attention", alerting, alerting == 1 ? "terminal needs" : "terminals need");
    return line;
}

} // namespace

void openEditor(Scribe& s) {
    s.commands = prCommandRegistryCreate();
    s.keymap = prCommandKeymapCreate();
    s.languages = prSyntaxRegistryCreate();
#if defined(SCRIBE_HAS_TREESITTER)
    prSyntaxTreeSitterAddLanguages(s.languages);
#endif
    s.menus = scribeMenus();
    s.launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    PrTextEditorDesc desc{
        .kit = s.kit,
        .font = s.font,
        .app = s.app,
        .launcher = s.launcher,
        .languages = s.languages,
        .commands = s.commands,
        .keymap = s.keymap,
    };
    s.editor = prTextEditorCreate(&desc);
    // clangd serves C and C++, wherever the PATH has it. A program that cannot start leaves its documents unserved.
    PrStringView clangdLanguages[] = {"c", "cpp"};
    PrTextEditorServerDesc clangd{.languages = clangdLanguages, .languageCount = 2, .program = "clangd"};
    prTextEditorAddServer(s.editor, &clangd);
    addCommand(s, "view.palette", "Show All Commands", "Cmd Shift P", [](void* c, const PrCommandArgs*) {
        scribeOf(c).paletteRequested = true;
    });
    addCommand(s, "app.quit", "Quit", "Cmd Q", [](void* c, const PrCommandArgs*) {
        if (scribeOf(c).app != nullptr)
            prAppStop(scribeOf(c).app);
    });
    addTerminalCommands(s);
    openWorkspace(s);
}

bool openDocument(Scribe& s, PrStringView path) {
    PrTextEditorDocumentDesc desc{};
    if (!path.isEmpty())
        desc.path = path;
    PrTextEditorDocument* document = nullptr;
    return prTextEditorDocumentCreate(s.editor, &desc, &document) == PrStatus_Ok;
}

void setDebuggee(Scribe& s, PrStringView program) {
    // lldb-dap is part of the toolchain, which xcrun finds on Apple's platforms, and is on the PATH elsewhere.
#if defined(__APPLE__)
    PrStringView adapter = "xcrun";
    PrStringView arguments[] = {"lldb-dap"};
    uint32_t argumentCount = 1;
#else
    PrStringView adapter = "lldb-dap";
    const PrStringView* arguments = nullptr;
    uint32_t argumentCount = 0;
#endif
    String launch("{\"program\":\"");
    for (size_t i = 0; i < program.size; ++i) {
        char c = program.data[i];
        if (c == '"' || c == '\\')
            launch.append('\\');
        launch.append(c);
    }
    launch.append("\"}");
    PrTextEditorDebuggerDesc desc{
        .program = adapter,
        .arguments = arguments,
        .argumentCount = argumentCount,
        .adapterId = "lldb-dap",
        .launch = launch,
    };
    prTextEditorSetDebugger(s.editor, &desc);
}

bool openDiff(Scribe& s, PrStringView oldPath, PrStringView newPath) {
    PrTextEditorDocumentDesc oldDesc{.path = oldPath};
    PrTextEditorDocumentDesc newDesc{.path = newPath};
    PrTextEditorDocument* old = nullptr;
    PrTextEditorDocument* young = nullptr;
    if (prTextEditorDocumentCreate(s.editor, &oldDesc, &old) != PrStatus_Ok ||
            prTextEditorDocumentCreate(s.editor, &newDesc, &young) != PrStatus_Ok)
        return false;
    prTextEditorDiffCreate(s.editor, old, young);
    return true;
}

void openText(Scribe& s, PrStringView text, PrStringView name) {
    PrTextEditorDocumentDesc desc{.text = text, .name = name};
    PrTextEditorDocument* document = nullptr;
    prTextEditorDocumentCreate(s.editor, &desc, &document);
}

void openTerminal(Scribe& s) {
    PR_ASSERT(s.fonts != nullptr);
    PrFontFamily mono{.generic = PrFontGeneric_Monospace};
    PrFontStyle style = prFontStyleDefault();
    style.size = 13;
    // A terminal starts in the directory of the panel's shown terminal, or else in the workspace of the shown document.
    PrRawString directory = prRawStringNull();
    PrTextEditorDocument* document = prTextEditorShown(s.editor);
    if (PrTerminal* shown = shownTerminal(s))
        directory = prTerminalWorkingDirectory(shown);
    else if (document != nullptr)
        directory = prTextEditorDocumentWorkspace(document);
    PrTerminalDesc desc{
        .app = s.app,
        .font = PrTextFontDesc{.set = s.fonts, .families = &mono, .familyCount = 1, .style = style},
        .launcher = s.launcher,
        .directory = prRawStringView(&directory),
        .scrollback = kPrTerminalScrollbackDefault,
        .notificationTag = s.nextTerminalTag,
        .openFile = openTerminalFile,
        .openFileContext = &s,
    };
    PrTerminal* terminal = prTerminalCreate(&desc);
    prRawStringReset(&directory);
    ++s.nextTerminalTag;
    s.terminals.push(terminal);
    s.terminal = s.terminals.count() - 1;
    s.terminalShown = true;
    prTerminalFocus(terminal);
}

String defaultStatePath() {
    PrRawString data = prFileUserDirectory(PrFileUserDirectory_Data);
    String path;
    if (!prRawStringIsEmpty(&data))
        path = format("{}/Prax/Scribe/Last Project", prRawStringView(&data));
    prRawStringReset(&data);
    return path;
}

void closeEditor(Scribe& s) {
    closeWorkspace(s);
    for (PrTerminal* terminal : s.terminals)
        prTerminalDestroy(terminal);
    s.terminals.clear();
    prTextEditorDestroy(s.editor);
    prProcessLauncherDestroy(s.launcher);
    prSyntaxRegistryDestroy(s.languages);
    prCommandKeymapDestroy(s.keymap);
    prCommandRegistryDestroy(s.commands);
    prTextFontRelease(s.font);
}

void buildScribe(Scribe& s, PrLayoutNode root) {
    prCommandScope(s.ui, root, s.commands, s.keymap, "");
    // The terminals' programs see the color scheme that the window's theme follows.
    PrAppColorScheme scheme = prUiUserPreferences(s.ui).colorScheme;
    for (PrTerminal* terminal : s.terminals)
        prTerminalSetColorScheme(terminal, scheme);
    bool menuOpen = buildMenus(s, root);
    buildWorkspace(s, root);
    buildTerminalPanel(s, root);

    if (s.paletteRequested) {
        prKitSetOpen(s.kit, root, true);
        s.paletteRequested = false;
    }
    PrLayoutNode list = prCommandPalette(s.kit, root, s.commands, s.keymap, s.paletteQuery);
    PrStringView query{};
    if (list == kPrLayoutNodeNone) {
        s.paletteQuery.clear();
    } else if (prKitTextChanged(s.kit, list, &query)) {
        s.paletteQuery = query;
    }
    // A command runs on the text, so typing goes on there once what floats over it closes.
    PrLayoutNode textNode = prTextEditorTextNode(s.editor);
    bool closed = (s.paletteOpen && list == kPrLayoutNodeNone) || (s.menuOpen && !menuOpen);
    if (textNode != kPrLayoutNodeNone && closed && !terminalHasFocus(s))
        prUiSetFocus(s.ui, textNode);
    s.paletteOpen = list != kPrLayoutNodeNone;
    s.menuOpen = menuOpen;
    // The status line is a bar of its own, which clips what does not fit.
    PrKitPanelStyle bar{.background = PrKitBackground_Header, .rules = PrKitRuleFlag_Top};
    bar.box.direction = PrKitDirection_Row;
    bar.box.align = PrKitAlign_Center;
    bar.box.overflow = PrKitOverflow_Clip;
    bar.box.padding = prKitEdgesXY(0, 8);
    bar.box.place = {.height = prKitPx(kStatusHeight), .flex = PrKitFlex_Fixed};
    PrLayoutNode status = prKitPanel(s.kit, root, {}, bar);
    PrKitTextStyle muted{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Clip};
    prKitPlace(s.kit, prKitLabel(s.kit, status, {.text = statusLine(s)}, muted), {.flex = PrKitFlex_Fill});
}

bool afterLayout(Scribe& s) {
    return prTextEditorLaidOut(s.editor);
}

bool handleWindowEvent(Scribe& s, const PrAppEvent& event) {
    PrAppNavigation navigation = prAppEventNavigation(&event);
    if (navigation == PrAppNavigation_None)
        return false;
    PrStringView command = navigation == PrAppNavigation_Back ? "go.back" : "go.forward";
    prCommandRegistryRun(s.commands, command, nullptr, nullptr);
    return true;
}

bool handleEvent(Scribe& s, const PrAppEvent& event) {
    if (event.kind == PrAppEventKind_Wake) {
        for (PrTerminal* terminal : s.terminals)
            prTerminalService(terminal, prUiWindow(s.ui));
        return false;
    }
    if (event.kind != PrAppEventKind_Notification)
        return false;
    for (uint32_t i = 0; i < s.terminals.count(); ++i) {
        if (prTerminalNotificationTag(s.terminals[i]) != event.tag)
            continue;
        s.terminal = i;
        s.terminalShown = true;
        prTerminalFocus(s.terminals[i]);
        break;
    }
    return true;
}

} // namespace scribe
