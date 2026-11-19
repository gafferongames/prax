# PNG: an image's header, its ancillary chunks and its pixels, the frames of an animated PNG, the encoding of 8-bit
# RGBA, and the file's texts. Its zlib streams go through a set of compression codecs.
pr_library(png INCLUDE include
    src/png.cpp
    src/png_text.cpp
)
target_link_libraries(pr_png PUBLIC prax::core prax::compress PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(png
        tests/test_png.cpp
        tests/test_png_text.cpp
        tests/test_png_c.c
    )
    target_link_libraries(png_tests PRIVATE prax::compress)
endif()
