# LZFSE: decoding Apple's LZ77 with finite state entropy coding, and its LZVN and stored blocks, a piece of input at a
# time.
pr_library(lzfse INCLUDE include
    src/lzfse.cpp
)
target_link_libraries(pr_lzfse PUBLIC prax::core PRIVATE prax::core_cpp prax::entropy)

if (PR_DEVELOP)
    pr_module_tests(lzfse
        tests/test_lzfse.cpp
        tests/test_lzfse_c.c
    )
endif()
