# The tangent generators that lib/mesh's tangents are compared with, and what compares them: MikkTSpace's reference
# implementation, which glTF names for an asset without tangents, and meshoptimizer's generator, which derives from it.
# They are fetched for the development build alone and built from their sources.
pr_dependency_available(mikktspace meshoptimizer)

# MikkTSpace, compiled as lib/mesh is, without fused multiply-adds, so that the two round alike.
add_library(mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})
target_compile_options(mikktspace PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>)
pr_third_party(mikktspace)

# meshoptimizer's tangent generator and the allocator that it uses, compiled as its own build compiles them.
add_library(meshopt_tangents STATIC ${meshoptimizer_SOURCE_DIR}/src/tangentspace.cpp
                                    ${meshoptimizer_SOURCE_DIR}/src/allocator.cpp)
target_include_directories(meshopt_tangents SYSTEM PUBLIC ${meshoptimizer_SOURCE_DIR}/src)
pr_third_party(meshopt_tangents)

# The three behind one interface.
add_library(tangent_references STATIC tangent_references.cpp)
target_include_directories(tangent_references PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(tangent_references PUBLIC prax::core PRIVATE mikktspace meshopt_tangents prax::core_cpp
                                         pr_warnings)

# The harness of the tangents suite (tools/suite/tangents.py), over glTF assets.
pr_harness(tangents_compare prax::mesh tangents_compare.cpp)
target_link_libraries(tangents_compare PRIVATE tangent_references prax::gltf prax::core_cpp)

# lib/mesh's tangents beside MikkTSpace's over the shapes of lib/mesh's tangent tests, which the unit tests run.
add_executable(tangents_reference_tests tests/test_tangents_reference.cpp)
target_link_libraries(tangents_reference_tests PRIVATE tangent_references prax::mesh prax::core_cpp test_main
                                               pr_warnings)
pr_add_tests(tangents_reference_tests)
