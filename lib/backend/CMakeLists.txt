# Backend choices: the descriptor of an interface whose backend a program chooses, and the choices that the environment
# makes. Each module's selector links it.
pr_library(backend INCLUDE include
    src/backend.cpp
)
target_link_libraries(pr_backend PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(backend
        tests/test_backend.cpp
        tests/test_backend_c.c
    )
endif()
