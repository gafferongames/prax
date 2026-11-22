# Pandite is an image viewer that stays responsive, with a gallery of the images in the folder of the open image. It
# runs in a Studio window, and its own view themes the elements of lib/ui. Each picture is a texture source that a
# catalog names by the path of its file. The window's texture cache realizes each picture at the size at which it is
# shown. The model is a separate library, which the application and the tests link. The model holds the folder, the
# picture loader and the view state of an image.
add_library(pandite_model STATIC
    edit.cpp
    files.cpp
    folder.cpp
    info.cpp
    loader.cpp
    view_state.cpp
)
target_include_directories(pandite_model PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(pandite_model PUBLIC prax::image prax::texture prax::geom prax::core_cpp
                                    PRIVATE prax::color prax::mime pr_warnings)

# The application. It adds pandite's view to the model library, and main() composes them.
pr_app(pandite
    main.cpp
    commands.cpp
    compare.cpp
    editor.cpp
    icons.cpp
    peek.cpp
    view.cpp
)
target_link_libraries(pandite PRIVATE prax::compress_select prax::image_select pandite_model prax::studio_app
                                      prax::studio_kit prax::app prax::ui_cpp prax::studio_command prax::color
                                      prax::font prax::image prax::task prax::texture prax::texture_select
                                      prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(pandite_model
        tests/test_edit.cpp
        tests/test_files.cpp
        tests/test_folder.cpp
        tests/test_info.cpp
        tests/test_loader.cpp
        tests/test_view_state.cpp
    )
    target_link_libraries(pandite_model_tests PRIVATE prax::compress_select prax::image_select prax::gpu_select
                                                      prax::image)
    target_compile_definitions(pandite_model_tests PRIVATE
        PANDITE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_files")
endif()
