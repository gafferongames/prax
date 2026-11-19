# The reference encoder and decoder of Draco, which lib/draco's decoder is compared with. They are fetched for the
# development build alone and built from their sources, with the features that the reference's own build turns on.
pr_dependency_available(draco)

file(GLOB_RECURSE draco_reference_sources
    ${draco_SOURCE_DIR}/src/draco/attributes/*.cc
    ${draco_SOURCE_DIR}/src/draco/compression/*.cc
    ${draco_SOURCE_DIR}/src/draco/core/*.cc
    ${draco_SOURCE_DIR}/src/draco/mesh/*.cc
    ${draco_SOURCE_DIR}/src/draco/metadata/*.cc
    ${draco_SOURCE_DIR}/src/draco/point_cloud/*.cc)
list(FILTER draco_reference_sources EXCLUDE REGEX "test")

# What the reference's build writes for its sources to include: the parts of the bitstream that it is built with.
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/draco/draco_features.h CONTENT
"#define DRACO_POINT_CLOUD_COMPRESSION_SUPPORTED
#define DRACO_MESH_COMPRESSION_SUPPORTED
#define DRACO_NORMAL_ENCODING_SUPPORTED
#define DRACO_STANDARD_EDGEBREAKER_SUPPORTED
#define DRACO_PREDICTIVE_EDGEBREAKER_SUPPORTED
#define DRACO_BACKWARDS_COMPATIBILITY_SUPPORTED
#define DRACO_ATTRIBUTE_INDICES_DEDUPLICATION_SUPPORTED
#define DRACO_ATTRIBUTE_VALUES_DEDUPLICATION_SUPPORTED
" @ONLY)

add_library(draco_reference STATIC ${draco_reference_sources})
target_include_directories(draco_reference SYSTEM PUBLIC ${draco_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/include)
# The reference's values are compared to the bit, so its arithmetic is each operation rounded on its own, as it is on a
# platform with no fused multiply and add.
target_compile_options(draco_reference PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>)
pr_third_party(draco_reference)

# The harness of the draco suite (tools/suite/draco.py), over Draco streams in files.
pr_harness(draco_compare prax::draco draco_compare.cpp)
target_link_libraries(draco_compare PRIVATE draco_reference prax::core_cpp)

# lib/draco's decoder beside the reference's, over streams that the reference encoder writes.
add_executable(draco_reference_tests tests/test_draco_reference.cpp)
target_link_libraries(draco_reference_tests PRIVATE draco_reference prax::draco prax::core_cpp test_main pr_warnings)
pr_add_tests(draco_reference_tests)
