# The shader sets of lib/render (pr/render_shaders.h): the material types and the shading models that a renderer draws
# with, compiled with the passes that call them.
#
#   pr_render_shaders(<target> <Name> [EXCLUDE_FROM_ALL]
#       [SOURCES <file>...] [INCLUDE <directory>...] [DEFINES <macro>...] [PRECISE_MATH]
#       [TABLE <Table> EXTENT <width> <height>]...
#       [MATERIAL <Type> STRUCT <structure>]...)
#
# compiles the library's pass sources with the Slang SOURCES into the shader set <Name>, and adds the static library
# <target>, which is the set. A target that creates a renderer with the set links <target>, and so do the targets that
# name the set's material types, such as a library of an application's scenes and its tests:
#
#   target_link_libraries(my_app PRIVATE my_shaders)
#
# Their sources include "render_shaders/<Name>Shaders.h", which declares the function <name>ShaderSet that returns the
# set, the id k<Name>Material<Type> of each material type, the index k<Name><Type>Texture<Field> of each texture and
# k<Name><Type>Attribute<Field> of each attribute that a type declares, and the id k<Name>Table<Table> of each table.
# <Name>, <Type> and <Table> are identifiers that begin with a capital. The library links prax::render, and carries it
# to the targets that link the set. EXCLUDE_FROM_ALL builds the library only for a target that links it.
#
# MATERIAL adds a material type. STRUCT names the structure of the SOURCES that implements IRenderMaterial
# (pr/render_surface.slang), which declares everything that the type reads: its parameter record, its textures with
# their stand-ins, and the attributes of a mesh's vertices, and names its shading model. Nothing about a type is
# written here but its name. A shading model is not declared here either: it is the structure that a material type
# names as its Surface. The library's OpenPBR type is a type like any other: a set that wants it adds MATERIAL OpenPbr
# STRUCT RenderMaterialOpenPbr, since every set's sources include the library's types.
#
# TABLE adds a lookup table of <width> by <height> texels, which the set's sources sample as kRenderTable<Table>
# (pr/render_table.slang). The set's header names the table's id, k<Name>Table<Table>, by which the application gives
# the function that writes the table's texels when it creates a renderer (PrRenderOptions::tables). A renderer keeps
# every table of its set in one texture, one under another, so the sum of their heights is at most the height of the
# device's largest texture. A table of the library's own names its function here, with FILL.
#
# Every set begins with the library's own table and its four types, Gltf, Unlit, GltfLayered and UsdPreview
# (PR_RENDER_STANDARD_SHADERS), which this function reads as it reads an application's, and which a renderer finds by
# their names. The library's headers define the constant of its table, which is therefore the set's first.
# INCLUDE names directories that the sources include from, beside pr/. DEFINES names macros that the sources compile
# with, each defined to 1. PRECISE_MATH compiles the set's arithmetic as it is written, without the fusing and
# reassociation of fast math, which costs some speed and makes the binding tiers draw the same bits
# (shader_library.cmake).
#
# The set compiles three libraries, each for a device with bindless and for the slots alone. Each has a vertex shader for
# each layout of the types' attributes, which types of equal declarations share, so a pipeline is made for a layout. The
# masked depth and the visibility technique shade any material of a layout in one shader, so their libraries dispatch
# on a material's type. The forward technique draws each material type with a pipeline of its own, so its library has a
# fragment entry point for each type, which holds that type's material and its shading model alone.
#
# The build reads the set before it compiles the passes (render_shaders_check.cmake): it fails with what is wrong, and
# writes the set's data, its header and the passes' sources from what the compiler reflects of each type.
#
# The file serves the source tree and the installed package. Each sets the global properties PR_SHADER_TARGETS and
# PR_SHADER_METAL_SDK, which name the shader libraries' targets (shader_library.cmake), and includes slang.cmake, whose
# pr_slangc gives the compiler.

# Where the scripts, the headers that the shaders include, and the pass sources are. In the installed package the
# scripts are beside this file, and the package's configuration names the other two. In the source tree each is in its
# module. The pass sources include each other as "render/src/<file>".
if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/shader_library.cmake)
    set_property(GLOBAL PROPERTY PR_RENDER_SHADER_SCRIPTS ${CMAKE_CURRENT_LIST_DIR})
else()
    cmake_path(SET _pr_render_root NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../..)
    set_property(GLOBAL PROPERTY PR_RENDER_SHADER_SCRIPTS ${_pr_render_root}/cmake)
    set_property(GLOBAL PROPERTY PR_RENDER_SHADER_INCLUDES ${_pr_render_root}/lib/gpu/include
        ${_pr_render_root}/lib/render/include)
    set_property(GLOBAL PROPERTY PR_RENDER_SHADER_PASSES ${_pr_render_root}/lib)
endif()
set_property(GLOBAL PROPERTY PR_RENDER_SHADER_CHECK ${CMAKE_CURRENT_LIST_DIR}/render_shaders_check.cmake)

# The library's table and its material types, in pr_render_shaders' own arguments.
set_property(GLOBAL PROPERTY PR_RENDER_STANDARD_SHADERS
    TABLE SplitSum EXTENT 32 32 FILL prRenderSplitSumTable
    MATERIAL KhronosPbr STRUCT RenderMaterialKhronosPbr
    MATERIAL Unlit STRUCT RenderMaterialUnlit
    MATERIAL KhronosPbrLayered STRUCT RenderMaterialKhronosPbrLayered
    MATERIAL UsdPreview STRUCT RenderMaterialUsdPreview)

# Generates the files of the shader set `name` from pr_render_shaders' arguments, and the rules that compile its
# shaders. Sets, in the caller's scope, _pr_render_set_sources to the sources of the target that holds the set, and
# _pr_render_set_directory to the directory of the set's files, whose `include` holds the set's header. `target` names
# the caller's target in messages.
function(_pr_render_shader_set target name)
    # The passes that a shader set compiles, in the order of PrRenderShaderPass. They are set here because a project
    # that adds this one calls the function from a directory that does not see this file's variables.
    set(_pr_render_shader_passes DepthMask Forward Visibility)
    set(where "pr_render_shaders(${target} ${name})")
    if (NOT name MATCHES "^[A-Z][A-Za-z0-9]*$")
        message(FATAL_ERROR "${where}: the set's name is an identifier that begins with a capital")
    endif()
    get_property(scripts GLOBAL PROPERTY PR_RENDER_SHADER_SCRIPTS)
    get_property(includes GLOBAL PROPERTY PR_RENDER_SHADER_INCLUDES)
    get_property(passes GLOBAL PROPERTY PR_RENDER_SHADER_PASSES)
    get_property(check GLOBAL PROPERTY PR_RENDER_SHADER_CHECK)
    get_property(standard GLOBAL PROPERTY PR_RENDER_STANDARD_SHADERS)
    get_property(targets GLOBAL PROPERTY PR_SHADER_TARGETS)
    get_property(metal_sdk GLOBAL PROPERTY PR_SHADER_METAL_SDK)
    pr_slangc(slangc)

    # The arguments before the first table or type are the set's own. The rest are groups, one for each table and one
    # for each type, after the library's.
    set(head "")
    set(groups ${standard})
    set(in_head TRUE)
    foreach (argument IN LISTS ARGN)
        if (argument MATCHES "^(MATERIAL|TABLE)$")
            set(in_head FALSE)
        endif()
        if (in_head)
            list(APPEND head "${argument}")
        else()
            list(APPEND groups "${argument}")
        endif()
    endforeach()
    set(group_count 0)
    set(table_count 0)
    foreach (argument IN LISTS groups)
        if (argument STREQUAL "MATERIAL")
            math(EXPR group_count "${group_count} + 1")
            set(group_${group_count} "")
            set(into group_${group_count})
        elseif (argument STREQUAL "TABLE")
            math(EXPR table_count "${table_count} + 1")
            set(table_${table_count} "")
            set(into table_${table_count})
        endif()
        list(APPEND ${into} "${argument}")
    endforeach()
    cmake_parse_arguments(SET "PRECISE_MATH" "" "SOURCES;INCLUDE;DEFINES" ${head})
    if (SET_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${where}: unknown arguments: ${SET_UNPARSED_ARGUMENTS}")
    endif()
    set(sources "")
    foreach (source IN LISTS SET_SOURCES)
        cmake_path(ABSOLUTE_PATH source NORMALIZE)
        if (NOT EXISTS "${source}")
            message(FATAL_ERROR "${where}: no source at ${source}")
        endif()
        list(APPEND sources "${source}")
    endforeach()
    set(user_includes "")
    foreach (include IN LISTS SET_INCLUDE)
        cmake_path(ABSOLUTE_PATH include NORMALIZE)
        list(APPEND user_includes "${include}")
    endforeach()

    # The tables, one under another in the texture that holds them. The library's headers define the constants of the
    # library's tables, which are the first, and the set's sources get a constant for each of the others.
    list(LENGTH standard standard_length)
    set(standard_tables 0)
    foreach (argument IN LISTS standard)
        if (argument STREQUAL "TABLE")
            math(EXPR standard_tables "${standard_tables} + 1")
        endif()
    endforeach()
    set(tables "")
    set(tables_text "")
    set(table_ids "")
    set(tables_slang "")
    set(tables_width 0)
    set(tables_height 0)
    foreach (index RANGE 1 ${table_count})
        cmake_parse_arguments(TABLE "" "TABLE;FILL" "EXTENT" ${table_${index}})
        if (NOT TABLE_FILL)
            set(TABLE_FILL nullptr)
        endif()
        set(table "${TABLE_TABLE}")
        set(table_where "${where}: table ${table}")
        if (NOT table MATCHES "^[A-Z][A-Za-z0-9]*$")
            message(FATAL_ERROR "${table_where}: a table's name is an identifier that begins with a capital")
        endif()
        if (table IN_LIST tables)
            message(FATAL_ERROR "${table_where}: the set already has a table of that name")
        endif()
        if (TABLE_UNPARSED_ARGUMENTS OR NOT TABLE_FILL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR
                NOT "${TABLE_EXTENT}" MATCHES "^[1-9][0-9]*;[1-9][0-9]*$")
            message(FATAL_ERROR "${table_where}: a table is TABLE <Table> EXTENT <width> <height>")
        endif()
        list(GET TABLE_EXTENT 0 width)
        list(GET TABLE_EXTENT 1 height)
        list(APPEND tables ${table})
        string(APPEND tables_text "    {\"${table}\", {0, ${tables_height}}, {${width}, ${height}}, ${TABLE_FILL}},\n")
        math(EXPR table_id "${index} - 1")
        string(APPEND table_ids "    k${name}Table${table} = ${table_id},\n")
        if (index GREATER standard_tables)
            string(APPEND tables_slang "static const RenderTable kRenderTable${table} = ")
            string(APPEND tables_slang "{float2(0, ${tables_height}), float2(${width}, ${height})};\n")
        endif()
        if (width GREATER tables_width)
            set(tables_width ${width})
        endif()
        math(EXPR tables_height "${tables_height} + ${height}")
    endforeach()

    # The types, numbered in the order of their declaration.
    set(types "")
    set(structures "")
    foreach (index RANGE 1 ${group_count})
        cmake_parse_arguments(TYPE "" "MATERIAL;STRUCT" "" ${group_${index}})
        set(type "${TYPE_MATERIAL}")
        set(type_where "${where}: material type ${type}")
        if (NOT type MATCHES "^[A-Z][A-Za-z0-9]*$")
            message(FATAL_ERROR "${type_where}: a type's name is an identifier that begins with a capital")
        endif()
        if (type IN_LIST types)
            message(FATAL_ERROR "${type_where}: the set already has a material type of that name")
        endif()
        if (TYPE_UNPARSED_ARGUMENTS OR NOT TYPE_STRUCT MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            message(FATAL_ERROR "${type_where}: a type is MATERIAL <Type> STRUCT <structure>. STRUCT names the \
structure of the sources that implements IRenderMaterial (pr/render_surface.slang), which declares what the type reads")
        endif()
        list(APPEND types ${type})
        list(APPEND structures ${TYPE_STRUCT})
    endforeach()
    list(LENGTH types type_count)

    set(directory ${CMAKE_CURRENT_BINARY_DIR}/render_shaders/${name})
    string(SUBSTRING ${name} 0 1 first)
    string(SUBSTRING ${name} 1 -1 rest)
    string(TOLOWER ${first} first)
    set(function ${first}${rest}ShaderSet)

    # ------------------------------------------------------------------------------------------------------------------
    # The set's sources, which the check and the passes compile: the library's material types, which every set may
    # list, and the set's own. The check writes the passes' sources from what it reflects of the types.
    set(banner "// Generated by pr_render_shaders (render_shaders.cmake) for the shader set ${name}. Do not edit.\n")
    set(text "${banner}//\n// The sources of the set's material types and shading models.\n\n#pragma once\n\n")
    string(APPEND text "#include \"pr/render_material.slang\"\n#include \"pr/render_shading.slang\"\n")
    string(APPEND text "#include \"pr/render_material_khronos_pbr.slang\"\n#include \"pr/render_material_unlit.slang\"\n")
    string(APPEND text "#include \"pr/render_material_khronos_pbr_layered.slang\"\n")
    string(APPEND text "#include \"pr/render_material_usd_preview.slang\"\n")
    string(APPEND text "#include \"pr/render_material_openpbr.slang\"\n")
    if (tables_slang)
        string(APPEND text "\n// The set's tables: where each lies in the texture of the tables, and its extent.\n")
        string(APPEND text "${tables_slang}\n")
    endif()
    foreach (source IN LISTS sources)
        string(APPEND text "#include \"${source}\"\n")
    endforeach()
    file(CONFIGURE OUTPUT ${directory}/render_shader_sources.slang CONTENT "${text}" @ONLY)

    set(all_includes ${directory} ${includes} ${passes} ${user_includes})
    list(JOIN all_includes "|" joined_includes)
    list(JOIN targets "|" joined_targets)
    set(layout ${directory}/${name}ShadersLayout.h)
    set(set_header ${directory}/include/render_shaders/${name}Shaders.h)
    set(generated ${directory}/render_shader_set.slang ${directory}/render_shader_forward.slang
        ${directory}/render_shader_depth_mask.slang ${directory}/render_shader_visibility.slang)
    # What the check reads, in a file, so that a test of a set's mistakes runs the check as the build does.
    file(CONFIGURE OUTPUT ${directory}/check/arguments.cmake CONTENT
"set(SLANGC \"${slangc}\")
set(INCLUDES \"${all_includes}\")
set(SOURCES \"${sources}\")
set(DEFINES \"${SET_DEFINES}\")
set(NAME ${name})
set(TYPES \"${types}\")
set(STRUCTURES \"${structures}\")
set(TABLE_IDS \"${table_ids}    k${name}TableCount = ${table_count},\n\")
set(FUNCTION ${function})
set(DIRECTORY \"${directory}/check\")
" @ONLY)
    # The check's compiler writes no dependencies, so the check depends on every Slang source of the directories that
    # the set's sources include from, as the material types that it reflects are there.
    set(included_sources "")
    foreach (include IN LISTS includes passes)
        file(GLOB found CONFIGURE_DEPENDS ${include}/pr/*.slang ${include}/*.slang)
        list(APPEND included_sources ${found})
    endforeach()
    add_custom_command(OUTPUT ${layout} ${set_header} ${generated}
        COMMAND ${CMAKE_COMMAND} -DARGUMENTS=${directory}/check/arguments.cmake -DHEADER=${layout}
            -DDEPFILE=${layout}.d -P ${check}
        DEPENDS ${check} ${scripts}/shader_reflect.cmake ${directory}/check/arguments.cmake
            ${directory}/render_shader_sources.slang ${sources} ${included_sources}
        DEPFILE ${layout}.d
        COMMENT "Checking the shader set ${name}"
        VERBATIM)

    # ------------------------------------------------------------------------------------------------------------------
    # The libraries: each pass for a device with bindless, and for the slots alone unless the pass needs bindless.
    set(library_headers "")
    set(handles "")
    set(slots "")
    set(source_DepthMask ${directory}/render_shader_depth_mask.slang)
    set(source_Forward ${directory}/render_shader_forward.slang)
    set(source_Visibility ${directory}/render_shader_visibility.slang)
    foreach (pass IN LISTS _pr_render_shader_passes)
        set(source ${source_${pass}})
        cmake_path(GET source FILENAME source_name)
        foreach (tier Handle Slot)
            if (tier STREQUAL "Slot" AND pass STREQUAL "Visibility")
                string(APPEND slots "        PrByteView{},\n")
                continue()
            endif()
            set(variable k${name}${pass}${tier}Shaders)
            set(header ${directory}/${variable}.h)
            set(defines ${SET_DEFINES})
            if (tier STREQUAL "Slot")
                list(APPEND defines PR_RENDER_SLOTS)
                string(APPEND slots "        PrByteView{${variable}, sizeof(${variable})},\n")
            else()
                string(APPEND handles "        PrByteView{${variable}, sizeof(${variable})},\n")
            endif()
            list(JOIN defines "|" defines)
            add_custom_command(OUTPUT ${header}
                COMMAND ${CMAKE_COMMAND} -DSLANGC=${slangc} -DTARGETS=${joined_targets} -DVARIABLE=${variable}
                    -DMETAL_SDK=${metal_sdk} -DINCLUDES=${joined_includes} -DDEFINES=${defines}
                    -DPRECISE=${SET_PRECISE_MATH}
                    -DSOURCE=${source} -DHEADER=${header} -DDEPFILE=${header}.d
                    -DSCRATCH=${directory}/${variable} -DROOT=${directory} -P ${scripts}/shader_library.cmake
                DEPENDS ${source} ${scripts}/shader_library.cmake ${scripts}/shader_reflect.cmake ${layout}
                DEPFILE ${header}.d
                COMMENT "Compiling ${source_name} for the shader set ${name}"
                VERBATIM)
            list(APPEND library_headers ${header})
        endforeach()
    endforeach()

    # ------------------------------------------------------------------------------------------------------------------
    # The source that defines the set, from the check's data of its types.
    set(includes_text "")
    foreach (header IN LISTS library_headers)
        cmake_path(GET header FILENAME file)
        string(APPEND includes_text "#include \"${file}\"\n")
    endforeach()
    file(CONFIGURE OUTPUT ${directory}/${name}Shaders.cpp CONTENT
"// Generated by pr_render_shaders (render_shaders.cmake) for the shader set ${name}. Do not edit.

#include \"render_shaders/${name}Shaders.h\"

#include \"${name}ShadersLayout.h\"
${includes_text}
namespace {

const PrRenderTable kTables[] = {
${tables_text}};

const PrRenderShaderSet kSet = {
    \"${name}\",
    {
${handles}    },
    {
${slots}    },
    kMaterialTypes,
    ${type_count},
    kLayouts,
    sizeof(kLayouts) / sizeof(kLayouts[0]),
    kVariants,
    sizeof(kVariants) / sizeof(kVariants[0]),
    kTables,
    ${table_count},
    {${tables_width}, ${tables_height}},
};

} // namespace

const PrRenderShaderSet* ${function}(void) {
    return &kSet;
}
" @ONLY)
    set(_pr_render_set_sources ${directory}/${name}Shaders.cpp ${layout} ${set_header} ${library_headers} PARENT_SCOPE)
    set(_pr_render_set_directory ${directory} PARENT_SCOPE)
endfunction()

function(pr_render_shaders target name)
    set(arguments ${ARGN})
    set(exclude "")
    if ("EXCLUDE_FROM_ALL" IN_LIST arguments)
        list(REMOVE_ITEM arguments EXCLUDE_FROM_ALL)
        set(exclude EXCLUDE_FROM_ALL)
    endif()
    _pr_render_shader_set(${target} ${name} ${arguments})
    add_library(${target} STATIC ${exclude} ${_pr_render_set_sources})
    # The targets that link the set see its header alone. The source that defines the set also includes the libraries.
    target_include_directories(${target} PUBLIC ${_pr_render_set_directory}/include PRIVATE ${_pr_render_set_directory})
    target_link_libraries(${target} PUBLIC prax::render)
    # The engine's own shader sets build with its warnings. An application that uses Prax as a dependency has no
    # pr_warnings target.
    if (TARGET pr_warnings)
        target_link_libraries(${target} PRIVATE pr_warnings)
    endif()
endfunction()

# Adds a shader set, of pr_render_shaders' arguments, to a library of the engine's, which holds it and is installed with
# it: lib/render's own set, which a renderer that is created with no set draws with (prRenderShaderSetLibrary), and the
# set of a library that creates renderers of its own. An application's set is a library of its own (pr_render_shaders).
function(_pr_render_library_shaders target name)
    _pr_render_shader_set(${target} ${name} ${ARGN})
    target_sources(${target} PRIVATE ${_pr_render_set_sources})
    target_include_directories(${target} PRIVATE ${_pr_render_set_directory}/include)
    set_source_files_properties(${_pr_render_set_directory}/${name}Shaders.cpp PROPERTIES
        INCLUDE_DIRECTORIES ${_pr_render_set_directory})
endfunction()
