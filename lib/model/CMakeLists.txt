# Models: an asset of a scene format, through a source that describes it in no format's terms, made ready to draw with
# lib/render, with its hierarchy, cameras, lights and animations. It links no format: the formats' adapters implement
# its source in their own libraries.
pr_library(model INCLUDE include
    src/model.cpp
    src/model_bind.cpp
    src/model_mesh.cpp
    src/model_source.cpp
    src/model_texture.cpp
)
target_link_libraries(pr_model PUBLIC prax::core prax::color prax::geom prax::gpu prax::mesh prax::render prax::task
                               prax::texture PRIVATE prax::core_cpp prax::mime)

# The model host: the default formats and services that a program hands lib/model, and the wiring that opens a model
# of a file in one call.
pr_library(model_host INCLUDE host/include
    host/src/model_host.cpp
)
target_link_libraries(pr_model_host PUBLIC prax::core prax::compress prax::image prax::model prax::task prax::texture
                                    PRIVATE prax::core_cpp prax::gltf prax::ktx prax::usd)

if (PR_DEVELOP)
    pr_module_tests(model
        tests/test_model.cpp
        tests/test_model_c.c
        tests/test_model_mesh.cpp
        tests/test_model_render.cpp
        tests/test_model_texture.cpp
        tests/test_model_usd.cpp
    )
    target_link_libraries(model_tests PRIVATE prax::compress_select prax::image_select prax::gpu_select prax::geom
                                              prax::gltf prax::gpu
                                              prax::image prax::ktx
                                              prax::mesh prax::model_host prax::usd prax::render prax::task prax::texture
                                              render_openpbr_test_shaders)
    target_compile_definitions(model_tests PRIVATE PR_GLTF_DIR="${PR_TEST_GLTF_DIR}"
        PR_MATERIALX_DIR="${PR_TEST_MATERIALX_DIR}")
    # The Vulkan backend counts its validation's errors, which the tests check after each case.
    if (PR_GPU_VULKAN)
        target_compile_definitions(model_tests PRIVATE PR_RENDER_TEST_VULKAN)
    endif()
    # On the web, the tests that render run again in a page, on the page's WebGPU.
    if (PR_GPU_WEBGPU)
        pr_page_tests(model tests/test_model_render.cpp)
        target_link_libraries(model_page_tests PRIVATE prax::compress prax::geom prax::gltf prax::gpu prax::image
                                                       prax::ktx prax::mesh prax::render prax::task prax::texture
                                                       prax::model_host prax::usd)
        target_compile_definitions(model_page_tests PRIVATE PR_RENDER_TEST_WEBGPU)
    endif()
    # The corpus harness, which tools/suite/model.py drives.
    pr_harness(model_corpus prax::model tests/model_corpus.cpp)
    target_link_libraries(model_corpus PRIVATE prax::compress_select prax::image_select prax::gpu_select prax::gltf
                                               prax::gpu prax::image
                                               prax::ktx prax::model_host
                                               prax::render harness_png)
    if (PR_GPU_VULKAN)
        target_compile_definitions(model_corpus PRIVATE PR_RENDER_TEST_VULKAN)
    endif()
endif()
