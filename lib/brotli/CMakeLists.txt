# Brotli: decoding its streams a piece of input at a time, with the static dictionary and transforms that the format
# defines (tools/gen/gen_brotli.py generates their tables).
pr_library(brotli INCLUDE include
    src/brotli.cpp
    src/brotli_tables.cpp
)
target_link_libraries(pr_brotli PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(brotli
        tests/test_brotli.cpp
        tests/test_brotli_c.c
    )
endif()
