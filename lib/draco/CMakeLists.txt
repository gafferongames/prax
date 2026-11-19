# Draco: the compressed geometry of the Draco bitstream, decoded into points, their attributes and faces.
pr_library(draco INCLUDE include
    src/draco.cpp
    src/draco_edgebreaker.cpp
    src/draco_prediction.cpp
    src/draco_stream.cpp
)
target_link_libraries(pr_draco PUBLIC prax::core PRIVATE prax::core_cpp)

# A value is the reference decoder's to the bit on every platform, so no compiler may fuse a multiply and an add into
# one operation, which rounds differently.
target_compile_options(pr_draco PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>)

if (PR_DEVELOP)
    pr_module_tests(draco
        tests/test_draco.cpp
        tests/test_draco_c.c
    )
endif()
