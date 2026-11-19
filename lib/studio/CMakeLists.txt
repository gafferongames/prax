# Studio's icons: Lucide's glyphs as SVG path data, which Studio Kit and lib/ui's elements draw.
pr_library(studio_icons INCLUDE icons/include
    icons/src/studio_icon.cpp
)
target_link_libraries(pr_studio_icons PUBLIC prax::core)

if (PR_DEVELOP)
    pr_module_tests(studio_icons icons/tests/test_studio_icon.cpp)
    target_link_libraries(studio_icons_tests PRIVATE prax::path)
endif()

# Studio Kit: the widgets of the Studio design system, over lib/ui and lib/layout.
pr_library(studio_kit INCLUDE kit/include
    kit/src/kit.cpp
    kit/src/kit_box.cpp
    kit/src/kit_button.cpp
    kit/src/kit_choice.cpp
    kit/src/kit_collection.cpp
    kit/src/kit_color.cpp
    kit/src/kit_data.cpp
    kit/src/kit_image.cpp
    kit/src/kit_timeline.cpp
    kit/src/kit_dock.cpp
    kit/src/kit_graph.cpp
    kit/src/kit_history.cpp
    kit/src/kit_icon.cpp
    kit/src/kit_input.cpp
    kit/src/kit_menu.cpp
    kit/src/kit_panel.cpp
    kit/src/kit_text.cpp
    kit/src/kit_theme.cpp
    kit/src/kit_title_bar.cpp
)
target_link_libraries(pr_studio_kit PUBLIC prax::studio_icons prax::texture prax::app prax::ui prax::layout prax::path
                                           prax::draw
                                           prax::text prax::text_edit
                                           prax::geom prax::font prax::color
                                    PRIVATE prax::core_cpp prax::ui_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_kit
        kit/tests/test_kit.cpp
        kit/tests/test_kit_c.c
        kit/tests/test_kit_choice.cpp
        kit/tests/test_kit_collection.cpp
        kit/tests/test_kit_color.cpp
        kit/tests/test_kit_data.cpp
        kit/tests/test_kit_image.cpp
        kit/tests/test_kit_timeline.cpp
        kit/tests/test_kit_dock.cpp
        kit/tests/test_kit_graph.cpp
        kit/tests/test_kit_history.cpp
        kit/tests/test_kit_input.cpp
        kit/tests/test_kit_text.cpp
        kit/tests/test_kit_menu.cpp
        kit/tests/test_kit_title_bar.cpp
    )
    target_link_libraries(studio_kit_tests PRIVATE prax::gpu_select prax::texture kit_fixture prax::path prax::app
                                                   prax::text_edit
                                                   prax::text
                                                   prax::draw
                                                   prax::layout prax::ui prax::ui_cpp prax::color prax::gpu)
endif()

# Studio's commands: an application's verbs, their key bindings, and the scopes, menu items and command palette that
# run them.
pr_library(studio_command INCLUDE command/include
    command/src/command.cpp
    command/src/command_keymap.cpp
    command/src/command_kit.cpp
)
target_link_libraries(pr_studio_command PUBLIC prax::json prax::app prax::ui prax::studio_kit
                                        PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_command
        command/tests/test_command.cpp
        command/tests/test_command_c.c
        command/tests/test_command_kit.cpp
    )
    target_link_libraries(studio_command_tests PRIVATE kit_fixture prax::json prax::studio_kit)
endif()

# Studio's text editor: documents of files in windowed views, colored by their syntax, with the commands of a code
# editor and the language servers of their languages.
pr_library(studio_text_editor INCLUDE text_editor/include
    text_editor/src/text_editor.cpp
    text_editor/src/text_editor_debug.cpp
    text_editor/src/text_editor_diff.cpp
    text_editor/src/text_editor_server.cpp
    text_editor/src/text_editor_style.cpp
)
target_link_libraries(pr_studio_text_editor PUBLIC prax::app prax::layout prax::process prax::studio_command
                                                   prax::studio_kit prax::syntax prax::text prax::text_diff
                                                   prax::text_edit prax::text_view
                                            PRIVATE prax::core_cpp prax::color prax::dap prax::lsp prax::mime prax::ui)

if (PR_DEVELOP)
    pr_module_tests(studio_text_editor
        text_editor/tests/test_text_editor.cpp
        text_editor/tests/test_text_editor_c.c
    )
    target_link_libraries(studio_text_editor_tests PRIVATE kit_fixture prax::layout prax::text prax::ui
                                                           prax::studio_command prax::studio_kit prax::syntax prax::process)
    target_compile_definitions(studio_text_editor_tests PRIVATE
        TEXT_EDITOR_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/text_editor/tests"
        TEXT_EDITOR_TESTS_PYTHON="${Python3_EXECUTABLE}")
    if (PR_SYNTAX_TREESITTER)
        target_compile_definitions(studio_text_editor_tests PRIVATE TEXT_EDITOR_TESTS_HAVE_TREESITTER)
    endif()
endif()

# Studio's terminal: runs a program on a pseudoterminal, and draws its screen in cells of a monospaced font, with
# input, selection and scrollback.
pr_library(studio_terminal INCLUDE terminal/include
    terminal/src/terminal.cpp
    terminal/src/terminal_draw.cpp
    terminal/src/terminal_link.cpp
    terminal/src/terminal_shell.cpp
)
pr_embed(pr_studio_terminal terminal/shell VARIABLE kTerminalShellScripts FILES
    zsh/.zshenv
    zsh/prax.zsh
    bash/prax.bash
    fish/vendor_conf.d/prax-shell-integration.fish
)
target_link_libraries(pr_studio_terminal PUBLIC prax::texture prax::app prax::color prax::layout prax::process
                                                prax::studio_kit
                                                prax::text prax::vterm
                                         PRIVATE prax::core_cpp prax::compress prax::draw prax::image prax::ui
                                                 prax::url)
# The version that the terminal sets in TERM_PROGRAM_VERSION.
target_compile_definitions(pr_studio_terminal PRIVATE PR_TERMINAL_VERSION="${PROJECT_VERSION}")

# Its tests run programs, so they build where lib/process starts them.
if (PR_DEVELOP AND (PR_PROCESS_POSIX OR PR_PROCESS_WINDOWS))
    pr_module_tests(studio_terminal
        terminal/tests/test_terminal.cpp
        terminal/tests/test_terminal_c.c
    )
    target_link_libraries(studio_terminal_tests PRIVATE kit_fixture prax::app prax::draw prax::layout prax::text
                                                        prax::ui
                                                prax::process prax::url prax::vterm prax::studio_kit)
endif()

# Studio's image view: an encoded image of any format that lib/image decodes, with its frames, mip levels and
# animation.
pr_library(studio_image_view INCLUDE image_view/include
    image_view/src/image_view.cpp
)
target_link_libraries(pr_studio_image_view PUBLIC prax::image prax::studio_kit PRIVATE prax::core_cpp prax::texture)

if (PR_DEVELOP)
    pr_module_tests(studio_image_view
        image_view/tests/test_image_view.cpp
        image_view/tests/test_image_view_c.c
    )
    target_link_libraries(studio_image_view_tests PRIVATE kit_fixture)
endif()

# Studio's USD structure view: a composed stage's prims, their resolved properties, their opinions with the layers and
# arcs that gave them, and the stage's layers.
pr_library(studio_usd_view INCLUDE usd_view/include
    usd_view/src/usd_view.cpp
)
target_link_libraries(pr_studio_usd_view PUBLIC prax::studio_kit prax::usd PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_usd_view
        usd_view/tests/test_usd_view.cpp
        usd_view/tests/test_usd_view_c.c
    )
    target_link_libraries(studio_usd_view_tests PRIVATE kit_fixture prax::usd)
endif()

# Studio's page view: a document of the web, HTML or Markdown, rendered by lib/html as content of a Kit tree.
pr_library(studio_page_view INCLUDE page_view/include
    page_view/src/page_view.cpp
)
target_link_libraries(pr_studio_page_view PUBLIC prax::studio_kit prax::image prax::compress prax::text prax::fetch
    PRIVATE prax::core_cpp prax::html prax::layout prax::ui)

if (PR_DEVELOP)
    pr_module_tests(studio_page_view page_view/tests/test_page_view.cpp)
    target_link_libraries(studio_page_view_tests PRIVATE kit_fixture prax::studio_kit prax::layout prax::net
                                                         prax::fetch)
endif()

# Studio's glTF structure view: an item of a glTF asset, its properties and extensions, and its references.
pr_library(studio_gltf_view INCLUDE gltf_view/include
    gltf_view/src/gltf_view.cpp
)
target_link_libraries(pr_studio_gltf_view PUBLIC prax::gltf prax::json prax::studio_kit
                                          PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_gltf_view gltf_view/tests/test_gltf_view.cpp)
    target_link_libraries(studio_gltf_view_tests PRIVATE kit_fixture prax::layout)
endif()

# Studio's model view: a glTF asset or a USD stage drawn by lib/render through lib/model in a GPU view, with its
# cameras, its animations and the controls of its display. Its shader set has OpenPBR, which MaterialX materials draw
# with.
pr_library(studio_model_view INCLUDE model_view/include
    model_view/src/model_view.cpp
)
_pr_render_library_shaders(pr_studio_model_view StudioModel MATERIAL OpenPbr STRUCT RenderMaterialOpenPbr)
target_link_libraries(pr_studio_model_view PUBLIC prax::gpu prax::studio_app prax::model prax::studio_kit
                                           PRIVATE prax::core_cpp prax::geom prax::layout prax::render)

if (PR_DEVELOP)
    pr_module_tests(studio_model_view model_view/tests/test_model_view.cpp)
    target_link_libraries(studio_model_view_tests PRIVATE prax::compress_select prax::gpu_select kit_fixture prax::font
                                                          prax::gltf prax::gpu prax::studio_app
                                                          prax::usd)
endif()

# Studio's applications and their windows: an application's kits in windows of the platform, rendered with lib/draw on
# one GPU device.
pr_library(studio_app INCLUDE app/include
    app/src/studio_app.cpp
    app/src/studio_app_shared.cpp
    app/src/studio_window.cpp
)
target_link_libraries(pr_studio_app PUBLIC prax::app prax::color prax::draw prax::font prax::gpu
                                           prax::layout prax::studio_kit prax::task prax::text prax::ui
                                    PRIVATE prax::core_cpp prax::app_select prax::compress prax::draw_select prax::gpu_select
                                            prax::image prax::svg prax::texture)

if (PR_DEVELOP)
    pr_module_tests(studio_app
        app/tests/test_studio_app.cpp
        app/tests/test_studio_app_c.c
        app/tests/test_studio_window.cpp
    )
    target_link_libraries(studio_app_tests PRIVATE prax::studio_kit prax::gpu
                                                   prax::font)
endif()
