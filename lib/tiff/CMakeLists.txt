# TIFF: the first image of a file, uncompressed, with 8-bit samples stored chunky.
pr_library(tiff INCLUDE include
    src/tiff.cpp
)
target_link_libraries(pr_tiff PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(tiff
        tests/test_tiff.cpp
        tests/test_tiff_c.c
    )
endif()
