# Basis Universal's reference transcoder and encoder, which lib/basis's decoders of ETC1S and UASTC are compared with.
# They are fetched for the development build alone and built from their sources, with the copy of Zstandard that they
# bring, and without the kernels and the OpenCL that their own build may add.
pr_dependency_available(basis_universal)

set(basis ${basis_universal_SOURCE_DIR})
file(GLOB basis_encoder ${basis}/encoder/*.cpp ${basis}/encoder/3rdparty/*.cpp)
add_library(basis_reference STATIC ${basis}/transcoder/basisu_transcoder.cpp ${basis_encoder} ${basis}/zstd/zstd.c)
target_include_directories(basis_reference SYSTEM PUBLIC ${basis}/transcoder ${basis}/encoder)
target_compile_definitions(basis_reference PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1 BASISU_SUPPORT_SSE=0
                                                  BASISU_SUPPORT_OPENCL=0)
pr_third_party(basis_reference)
# The reference uses exceptions, run-time types and the standard library, which the engine's own targets build without.
target_compile_options(basis_reference PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fexceptions -frtti>)

# lib/basis's decoders, read through lib/ktx, beside the reference transcoder, over the textures of Khronos's sample
# assets and textures that the reference encoder writes.
add_executable(basis_reference_tests tests/test_basis_reference.cpp)
target_compile_definitions(basis_reference_tests PRIVATE PR_ROOT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(basis_reference_tests PRIVATE prax::compress_select basis_reference prax::basis prax::ktx
                                                    prax::core_cpp harness
                                                    test_main pr_warnings)
pr_add_tests(basis_reference_tests)
