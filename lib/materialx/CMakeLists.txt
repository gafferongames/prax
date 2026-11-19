# MaterialX documents, read into a tree of elements, and the semantics of MaterialX's surfaces as OpenPBR's inputs.
pr_library(materialx INCLUDE include
    src/materialx.cpp
    src/materialx_openpbr.cpp
)
target_link_libraries(pr_materialx PUBLIC prax::core PRIVATE prax::core_cpp prax::xml)

if (PR_DEVELOP)
    pr_module_tests(materialx
        tests/test_materialx.cpp
    )
endif()
