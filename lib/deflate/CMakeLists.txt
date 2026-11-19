# Deflate: its bare format and its zlib and gzip framings, decoded a piece of input at a time and encoded whole.
pr_library(deflate INCLUDE include
    src/deflate.cpp
)
target_link_libraries(pr_deflate PUBLIC prax::core PRIVATE prax::core_cpp prax::entropy)

if (PR_DEVELOP)
    pr_module_tests(deflate
        tests/test_deflate_encoder.cpp
        tests/test_deflate.cpp
        tests/test_deflate_c.c
    )
endif()
