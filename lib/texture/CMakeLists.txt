# Textures: sources of a texture's bits, the sources that convert pixels and make mip levels of another, the openers
# that make sources of encoded bytes by their formats, the upload of a source to a GPU device, and the cache of the
# textures that sources realize on one device. It depends on no format.
pr_library(texture INCLUDE include
    src/texture.cpp
    src/texture_cache.cpp
)
target_link_libraries(pr_texture PUBLIC prax::core prax::color prax::gpu prax::task PRIVATE prax::core_cpp prax::mime)

if (PR_DEVELOP)
    pr_module_tests(texture
        tests/test_texture.cpp
        tests/test_texture_cache.cpp
        tests/test_texture_c.c
    )
    target_link_libraries(texture_tests PRIVATE prax::color prax::gpu prax::task)
endif()
