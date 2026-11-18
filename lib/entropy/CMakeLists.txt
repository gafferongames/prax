# Entropy coding: the pieces that the coders of the compression and image format libraries share. Its functions are
# inline, so the libraries that use it link it privately and carry only what they call.
pr_library(entropy INCLUDE include)
target_link_libraries(pr_entropy INTERFACE prax::core)

if (PR_DEVELOP)
    pr_module_tests(entropy
        tests/test_entropy_lz.cpp
        tests/test_entropy_lz_c.c
    )
endif()
