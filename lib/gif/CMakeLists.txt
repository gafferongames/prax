# GIF: an image's logical screen and its frames, composed with their disposal and delays.
pr_library(gif INCLUDE include
    src/gif.cpp
)
target_link_libraries(pr_gif PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(gif
        tests/test_gif.cpp
        tests/test_gif_c.c
    )
endif()
