# emjc: the images of Apple Color Emoji on iOS, whose planes decompress as LZFSE through a set of compression codecs.
pr_library(emjc INCLUDE include
    src/emjc.cpp
)
target_link_libraries(pr_emjc PUBLIC prax::core prax::compress PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(emjc
        tests/test_emjc.cpp
        tests/test_emjc_c.c
    )
    target_link_libraries(emjc_tests PRIVATE prax::compress)
endif()
