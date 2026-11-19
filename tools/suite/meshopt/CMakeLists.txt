# meshoptimizer's codecs and filters, which lib/gltf's decoders of meshopt compression are compared with. They are fetched
# for the development build alone and built from their sources, as meshoptimizer's own build compiles them.
pr_dependency_available(meshoptimizer)

add_library(meshopt_codecs STATIC ${meshoptimizer_SOURCE_DIR}/src/vertexcodec.cpp
                                  ${meshoptimizer_SOURCE_DIR}/src/indexcodec.cpp
                                  ${meshoptimizer_SOURCE_DIR}/src/vertexfilter.cpp
                                  ${meshoptimizer_SOURCE_DIR}/src/allocator.cpp)
target_include_directories(meshopt_codecs SYSTEM PUBLIC ${meshoptimizer_SOURCE_DIR}/src)
pr_third_party(meshopt_codecs)

# lib/gltf's decoders beside meshoptimizer's, over streams that meshoptimizer's encoders write.
add_executable(meshopt_reference_tests tests/test_meshopt_reference.cpp)
target_link_libraries(meshopt_reference_tests PRIVATE meshopt_codecs prax::gltf prax::core_cpp test_main pr_warnings)
pr_add_tests(meshopt_reference_tests)
