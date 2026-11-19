# glTF 2.0 assets, read into a document of their scenes, meshes, materials, animations and binary data.
pr_library(gltf INCLUDE include
    src/gltf.cpp
    src/gltf_accessor.cpp
    src/gltf_animation.cpp
    src/gltf_draco.cpp
    src/gltf_meshopt.cpp
    src/gltf_document.cpp
    src/gltf_model.cpp
    src/gltf_parse.cpp
)
target_link_libraries(pr_gltf PUBLIC prax::core prax::geom prax::json prax::model
    PRIVATE prax::core_cpp prax::draco)

if (PR_DEVELOP)
    pr_module_tests(gltf
        tests/test_gltf.cpp
        tests/test_gltf_animation.cpp
        tests/test_gltf_c.c
        tests/test_gltf_draco.cpp
        tests/test_gltf_scene.cpp
    )
    # The corpus harness, which tools/suite/gltf.py drives.
    pr_harness(gltf_corpus prax::gltf tests/gltf_corpus.cpp)
endif()
