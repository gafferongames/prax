# Scribe: Studio's text editor in a window, whose operations are commands of lib/studio/command.
# Its interface is a library of its own, which the application and the tests of its interface share. Its resource
# addresses, and the handlers of the formats that they step into, are part of it.
add_library(scribe_editor STATIC
    address.cpp
    gltf_address.cpp
    image_address.cpp
    scribe.cpp
    source_link.cpp
    usd_address.cpp
    workspace.cpp
    zip_address.cpp
)
target_include_directories(scribe_editor PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(scribe_editor PUBLIC prax::compress_select prax::image_select prax::zip prax::app prax::process
                                           prax::syntax prax::studio_app
                                           prax::studio_command prax::studio_kit prax::task
                                           prax::studio_terminal prax::studio_text_editor prax::layout prax::ui
                                           prax::core_cpp prax::compress prax::gltf prax::image prax::mime
                                           prax::studio_image_view prax::studio_usd_view
                                           prax::studio_page_view prax::studio_model_view prax::fetch prax::net prax::model_host
                                           prax::json prax::text_edit prax::texture prax::usd prax::studio_gltf_view
                                    PRIVATE prax::text_view prax::url
                                            pr_warnings prax::color)
if (PR_SYNTAX_TREESITTER)
    target_compile_definitions(scribe_editor PUBLIC SCRIBE_HAS_TREESITTER)
endif()

pr_app(scribe main.cpp FONT_BUNDLES NotoMono NerdFontSymbols)
target_link_libraries(scribe PRIVATE prax::compress_select prax::image_select scribe_editor prax::studio_app
                                     prax::font_bundle
                                     prax::text prax::url
                                     prax::image prax::compress prax::studio_command prax::json)

if (PR_DEVELOP)
    pr_module_tests(scribe_editor
        tests/test_scribe.cpp
        tests/test_scribe_address.cpp
        tests/test_scribe_gltf_address.cpp
        tests/test_scribe_usd_address.cpp
        tests/test_scribe_workspace.cpp
        tests/test_scribe_zip_address.cpp
    )
    # The image handler's tests decode with the built-in codec.
    if (PR_IMAGE_BUILTIN)
        target_sources(scribe_editor_tests PRIVATE tests/test_scribe_image_address.cpp)
    endif()
    target_link_libraries(scribe_editor_tests PRIVATE prax::compress_select prax::image_select kit_fixture prax::font
                                                      prax::layout prax::process prax::text
                                                      prax::text_edit prax::text_view prax::ui prax::url)
endif()
