# KTX 2.0: the container of GPU textures, read as pixels, as the blocks that a GPU samples, and transcoded to them from
# Basis Universal's formats. Supercompressed levels inflate through the compression codec that a texture is given.
pr_library(ktx INCLUDE include
    src/ktx.cpp
    src/ktx_texture_source.cpp
)
target_link_libraries(pr_ktx PUBLIC prax::core prax::basis prax::compress prax::texture PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(ktx
        tests/test_ktx.cpp
        tests/test_ktx_c.c
        tests/test_ktx_texture_source.cpp
    )
    target_link_libraries(ktx_tests PRIVATE prax::compress_select prax::basis prax::compress prax::gpu prax::texture)
endif()
