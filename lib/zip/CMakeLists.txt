# ZIP archives: the entries of an archive in memory, ZIP64 included, decompressed through a set of compression codecs.
pr_library(zip INCLUDE include
    src/zip.cpp
)
target_link_libraries(pr_zip PUBLIC prax::core prax::compress PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(zip
        tests/test_zip.cpp
        tests/test_zip_c.c
    )
    target_link_libraries(zip_tests PRIVATE prax::compress)
endif()
