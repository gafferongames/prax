# OpenUSD layers, read from text, crate and package files into their specs, fields and values.
pr_library(usd INCLUDE include
    src/usd.cpp
    src/usd_crate.cpp
    src/usd_geom.cpp
    src/usd_compose.cpp
    src/usd_layer.cpp
    src/usd_model.cpp
    src/usd_mtlx.cpp
    src/usd_path.cpp
    src/usd_stage.cpp
    src/usd_text.cpp
)
target_link_libraries(pr_usd PUBLIC prax::core prax::compress prax::geom prax::model PRIVATE prax::core_cpp
    prax::materialx prax::mesh prax::zip)

if (PR_DEVELOP)
    pr_module_tests(usd
        tests/test_usd.cpp
        tests/test_usd_c.c
        tests/test_usd_crate.cpp
        tests/test_usd_geom.cpp
        tests/test_usd_package.cpp
        tests/test_usd_path.cpp
        tests/test_usd_stage.cpp
        tests/test_usd_text.cpp
    )
    target_link_libraries(usd_tests PRIVATE prax::compress_select prax::geom)
    target_compile_definitions(usd_tests PRIVATE PR_MATERIALX_DIR="${PR_TEST_MATERIALX_DIR}")
    # The corpus harness, which tools/suite/usd.py drives.
    pr_harness(usd_corpus prax::usd tests/usd_corpus.cpp)
    target_link_libraries(usd_corpus PRIVATE prax::compress_select prax::core_cpp prax::geom)
endif()
