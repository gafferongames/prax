# Rendering in three dimensions: meshes, textures, materials and environments on a GPU device, scenes of instances and
# lights, and the techniques that add a view's passes to a render graph.
pr_library(render INCLUDE include
    src/render.cpp
    src/render_deform.cpp
    src/render_display.cpp
    src/render_environment.cpp
    src/render_exposure.cpp
    src/render_meter.cpp
    src/render_pipeline.cpp
    src/render_resource.cpp
    src/render_scene.cpp
    src/render_shaders.cpp
    src/render_tables.cpp
    src/render_view.cpp
)
target_link_libraries(pr_render PUBLIC prax::core prax::color prax::geom prax::gpu prax::mesh PRIVATE prax::core_cpp)

# Each pass's shaders are a library of their own, which compiles once for devices with bindless, and once for the slots
# alone. These passes call no material and no shading model, so they are the same in every renderer.
set(render_shader_includes ${CMAKE_CURRENT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/lib)
foreach (pass Cull Deform Depth Background Display EnvironmentFilter Meter)
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" source ${pass})
    string(TOLOWER ${source} source)
    pr_slang(pr_render src/render_${source}.slang VARIABLE kRender${pass}Shaders INCLUDE ${render_shader_includes})
    pr_slang(pr_render src/render_${source}.slang VARIABLE kRender${pass}SlotShaders
        INCLUDE ${render_shader_includes} DEFINES PR_RENDER_SLOTS)
endforeach()

# The passes that call the materials and the shading models are a shader set. The library's own set, with its types and
# its model alone, is generated as an application's is, and is part of the library (render_shaders.cmake).
_pr_render_library_shaders(pr_render RenderLibrary)

if (PR_DEVELOP)
    pr_module_tests(render
        tests/test_render.cpp
        tests/test_render_arrival.cpp
        tests/test_render_custom.cpp
        tests/test_render_deform.cpp
        tests/test_render_environment.cpp
        tests/test_render_image.cpp
        tests/test_render_layers.cpp
        tests/test_render_openpbr.cpp
        tests/test_render_c.c
    )
    target_link_libraries(render_tests PRIVATE prax::gpu_select prax::geom prax::gpu)
    # A shader of the tests' own, which includes the library's shading functions as an application's does.
    pr_slang(render_tests tests/test_render.slang VARIABLE kTestRenderShaders INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # A second, which sums the lobes of the model's layers.
    pr_slang(render_tests tests/test_render_layers.slang VARIABLE kTestRenderLayerShaders
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # A shader set of the tests' own, as an application builds one: a material type with a pattern, which the library's
    # model lights, a material type of a toon model and of a wrap model, which the sources define, a material type
    # with more textures than a draw binds on a device without bindless, and a larger parameter record than the
    # library's types have, a material type of a model that shows what the library gives a shading model, and a type
    # that reads custom attributes of a mesh's vertices. The set has two tables of its own, which that model samples.
    pr_render_shaders(render_test_shaders TestRender
        SOURCES tests/test_render_materials.slang
        TABLE Ramp EXTENT 4 2
        TABLE Flat EXTENT 2 2
        MATERIAL Rings STRUCT TestRings
        MATERIAL ToonPaint STRUCT TestToonPaint
        MATERIAL Felt STRUCT TestFelt
        MATERIAL Quilt STRUCT TestQuilt
        MATERIAL Probe STRUCT TestProbe
        MATERIAL Heat STRUCT TestHeat)
    target_link_libraries(render_tests PRIVATE render_test_shaders)
    # A set that lists the library's OpenPBR type, which the model's tests draw with.
    pr_render_shaders(render_openpbr_test_shaders TestOpenPbr MATERIAL OpenPbr STRUCT RenderMaterialOpenPbr)
    target_link_libraries(render_tests PRIVATE render_openpbr_test_shaders)

    # Shader sets that each break one rule. Nothing builds them: a test runs the check of each, which must fail and say
    # what the set breaks and in which source. The texts are separated by `|`.
    function(render_broken_shaders name expect)
        pr_render_shaders(render_broken_shaders_${name} Broken${name} EXCLUDE_FROM_ALL ${ARGN})
        pr_add_test(render_shaders_${name} COMMAND ${CMAKE_COMMAND}
            -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/render_shaders_check.cmake
            -DARGUMENTS=${CMAKE_CURRENT_BINARY_DIR}/render_shaders/Broken${name}/check/arguments.cmake
            "-DEXPECT=${expect}" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_render_shaders.cmake)
    endfunction()
    set(bad SOURCES tests/test_render_bad.slang)
    # A structure that does not implement its interface, which the compiler reports where the source declares it.
    set(uncompiled "test_render_bad.slang: shader set|the sources do not compile|test_render_bad.slang:")
    render_broken_shaders(MissingSurface
        "${uncompiled}|type 'LostMaterial' does not provide required interface member 'surface'"
        ${bad} DEFINES TEST_BAD_MISSING_SURFACE)
    render_broken_shaders(ArgumentOrder "${uncompiled}|member 'surface' does not match interface requirement"
        ${bad} DEFINES TEST_BAD_ARGUMENT_ORDER)
    render_broken_shaders(Return "${uncompiled}|member 'surface' return type|does not match interface requirement"
        ${bad} DEFINES TEST_BAD_RETURN)
    render_broken_shaders(WrongSurface
        "${uncompiled}|return type 'RenderPbrSurface' does not match interface requirement return type 'GoodSurface'"
        ${bad} DEFINES TEST_BAD_WRONG_SURFACE)
    render_broken_shaders(MissingParameters
        "${uncompiled}|type 'BareMaterial' does not provide required interface member 'Parameters'"
        ${bad} DEFINES TEST_BAD_MISSING_PARAMETERS)
    render_broken_shaders(LostSurface "${uncompiled}|undefined identifier 'LostSurface'"
        ${bad} DEFINES TEST_BAD_LOST_SURFACE)
    render_broken_shaders(PlainSurface
        "${uncompiled}|'PlainMaterial' does not conform to the required interface 'IRenderMaterial'"
        ${bad} DEFINES TEST_BAD_PLAIN_SURFACE)
    render_broken_shaders(Indirect "${uncompiled}|type 'DimSurface' does not provide required interface member 'indirectRadiance'"
        ${bad} DEFINES TEST_BAD_INDIRECT)
    render_broken_shaders(Emission "${uncompiled}|'emittedRadiance' marked as 'override' is not overriding"
        ${bad} DEFINES TEST_BAD_EMISSION)
    # A member that is named as a method with a default and does not replace it, which the compiler accepts and the
    # build's check reports, with the place of the member in the source.
    render_broken_shaders(SilentEmission
        "test_render_bad.slang:|: shader set BrokenSilentEmission: a structure declares `emittedRadiance` without `override`"
        ${bad} DEFINES TEST_BAD_SILENT_EMISSION)
    render_broken_shaders(EmissionField
        "test_render_bad.slang:|: shader set BrokenEmissionField: a structure declares `emittedRadiance` without `override`"
        ${bad} DEFINES TEST_BAD_EMISSION_FIELD)
    render_broken_shaders(Alpha
        "${uncompiled}|type 'SolidSurface' does not provide required interface member 'alpha'"
        ${bad} DEFINES TEST_BAD_ALPHA)
    # A type that names no material type of the sources.
    render_broken_shaders(MissingStruct
        "test_render_bad.slang: shader set|material type Lost: STRUCT names `GhostMaterial`|undefined identifier"
        ${bad} MATERIAL Lost STRUCT GhostMaterial)
    render_broken_shaders(PlainStruct
        "material type Plain: STRUCT names `GoodParameters`|does not conform to the required interface"
        ${bad} MATERIAL Plain STRUCT GoodParameters)
    # A parameter record that C and the shaders would not read alike, which the reflection of the set shows.
    render_broken_shaders(Vector "material type Vector|BadVectorParameters.color is a 3-vector" ${bad}
        MATERIAL Vector STRUCT VectorMaterial)
    render_broken_shaders(Mark "material type Mark|its parameter `color` is a scalar" ${bad}
        MATERIAL Mark STRUCT MarkMaterial)
    render_broken_shaders(Order "material type Order|BadOrderParameters.color follows 4 bytes" ${bad}
        MATERIAL Order STRUCT OrderMaterial)
    render_broken_shaders(Size "material type Size|BadSizeParameters is 12 bytes" ${bad}
        MATERIAL Size STRUCT SizeMaterial)
    render_broken_shaders(Slot "test_render_bad_slot.slang|the sources declare `badNoise`, which takes a slot"
        SOURCES tests/test_render_bad_slot.slang MATERIAL Noisy STRUCT SlotMaterial)
    render_broken_shaders(Syntax "test_render_bad_syntax.slang|the sources do not compile"
        SOURCES tests/test_render_bad_syntax.slang)
    # Textures and attributes whose declarations do not give what a shader set reads of them.
    render_broken_shaders(UnmarkedTexture "material type Unmarked|its texture `color` is a RenderTexture marked"
        ${bad} DEFINES TEST_BAD_UNMARKED_TEXTURE MATERIAL Unmarked STRUCT UnmarkedMaterial)
    render_broken_shaders(UnmarkedAttribute
        "material type Attributed|its attribute `heat` is marked|RenderVertex(semantic, format"
        ${bad} DEFINES TEST_BAD_UNMARKED_ATTRIBUTE MATERIAL Attributed STRUCT AttributedMaterial)
    render_broken_shaders(Format "its attribute `heat` has the format `float5`, which is none of lib/gpu's"
        ${bad} DEFINES TEST_BAD_FORMAT MATERIAL Attributed STRUCT AttributedMaterial)
    render_broken_shaders(Semantic "its attribute `heat` has the semantic `warmth`, which is none of uv, color and"
        ${bad} DEFINES TEST_BAD_SEMANTIC MATERIAL Attributed STRUCT AttributedMaterial)
    render_broken_shaders(AttributeType "its attribute `heat` is a float, and its format float2 is read as a float2"
        ${bad} DEFINES TEST_BAD_ATTRIBUTE_TYPE MATERIAL Attributed STRUCT AttributedMaterial)

    # The Vulkan backend counts its validation's errors, which the tests check after each case.
    if (PR_GPU_VULKAN)
        target_compile_definitions(render_tests PRIVATE PR_RENDER_TEST_VULKAN)
    endif()

    # On the web, the same tests run again in a page, on the page's WebGPU, which counts its validation's errors too.
    if (PR_GPU_WEBGPU)
        get_target_property(sources render_tests SOURCES)
        get_target_property(includes render_tests INCLUDE_DIRECTORIES)
        pr_page_tests(render ${sources})
        target_include_directories(render_page_tests PRIVATE ${includes})
        target_link_libraries(render_page_tests PRIVATE render_test_shaders render_openpbr_test_shaders prax::geom
            prax::gpu)
        target_compile_definitions(render_page_tests PRIVATE PR_RENDER_TEST_WEBGPU)
    endif()
endif()
