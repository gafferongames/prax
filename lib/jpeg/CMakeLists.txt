# JPEG: Huffman-coded, 8-bit, sequential and progressive images, with their JFIF and Exif metadata.
pr_library(jpeg INCLUDE include
    src/jpeg.cpp
)
target_link_libraries(pr_jpeg PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(jpeg
        tests/test_jpeg.cpp
        tests/test_jpeg_c.c
    )
endif()
