# Meshes: geometry processing for triangle meshes held in the caller's arrays.
pr_library(mesh INCLUDE include
    src/mesh.cpp
    src/mesh_access.cpp
    src/mesh_bvh.cpp
    src/mesh_cache.cpp
    src/mesh_normals.cpp
    src/mesh_polygons.cpp
    src/mesh_shapes.cpp
    src/mesh_tangents.cpp
)
target_link_libraries(pr_mesh PUBLIC prax::core prax::geom PRIVATE prax::core_cpp)

# The same input gives the same output on every platform, so no compiler may fuse a multiply and an add into one
# operation, which rounds differently.
target_compile_options(pr_mesh PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>)

if (PR_DEVELOP)
    pr_module_tests(mesh
        tests/test_mesh.cpp
        tests/test_mesh_access.cpp
        tests/test_mesh_bvh.cpp
        tests/test_mesh_c.c
        tests/test_mesh_cache.cpp
        tests/test_mesh_normals.cpp
        tests/test_mesh_polygons.cpp
        tests/test_mesh_shapes.cpp
        tests/test_mesh_tangents.cpp
    )
endif()
