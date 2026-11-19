# The backends of a module, and its selector.
#
# A module declares each of its backends, and each unit of sources that several backends share. A backend builds if its
# option PR_<MODULE>_<BACKEND> is on (pr_backend_option) and every variable in its WHEN is true. It adds its sources
# and headers to the module's target and defines PR_<MODULE>_HAS_<BACKEND> there. It also adds each shared unit that it
# uses, once. The module's selector (pr_select) receives the same definitions, so the selector and the module agree on
# which backends the build includes.
#
#   pr_backend_shared(<module> <unit> [SOURCES <source>...] [INCLUDE <directory>])
#   pr_backend(<module> <backend> [SOURCES <source>...] [INCLUDE <directory>] [SHARED <unit>...] [REQUIRES <backend>...]
#       [WHEN <variable>...])
#   pr_select(<module> [SOURCES <source>...] [INCLUDE <directory>] [LINK <target>...])

# Declares a unit of sources that backends of `module` share. The first backend that uses the unit adds it to the
# module's target.
function(pr_backend_shared module unit)
    cmake_parse_arguments(PARSE_ARGV 2 SHARED "" "INCLUDE" "SOURCES")
    string(TOUPPER "${module}_${unit}" key)
    set(sources)
    foreach(source IN LISTS SHARED_SOURCES)
        cmake_path(ABSOLUTE_PATH source NORMALIZE)
        list(APPEND sources ${source})
    endforeach()
    set(include)
    if (SHARED_INCLUDE)
        set(include ${SHARED_INCLUDE})
        cmake_path(ABSOLUTE_PATH include NORMALIZE)
    endif()
    set_property(GLOBAL PROPERTY PR_SHARED_SOURCES_${key} "${sources}")
    set_property(GLOBAL PROPERTY PR_SHARED_INCLUDE_${key} "${include}")
    set_property(GLOBAL PROPERTY PR_SHARED_ADDED_${key} FALSE)
    set_property(GLOBAL APPEND PROPERTY PR_SHARED_UNITS ${key})
endfunction()

# Declares a backend of `module`. It builds into the module's target if its option is on and every variable in WHEN is
# true.
function(pr_backend module backend)
    cmake_parse_arguments(PARSE_ARGV 2 BACKEND "" "INCLUDE" "SOURCES;SHARED;REQUIRES;WHEN")
    string(TOUPPER "${module}" MODULE)
    string(TOUPPER "${backend}" BACKEND)
    set(option PR_${MODULE}_${BACKEND})
    if (NOT DEFINED ${option})
        message(FATAL_ERROR "pr_backend(${module} ${backend}): ${option} is not declared (pr_backend_option)")
    endif()
    if (NOT ${option})
        return()
    endif()
    foreach(condition IN LISTS BACKEND_WHEN)
        if (NOT ${condition})
            return()
        endif()
    endforeach()
    foreach(required IN LISTS BACKEND_REQUIRES)
        string(TOUPPER "${required}" REQUIRED)
        if (NOT PR_${MODULE}_${REQUIRED})
            message(FATAL_ERROR "${module}'s ${backend} backend needs its ${required} backend: ${option} needs "
                "PR_${MODULE}_${REQUIRED}")
        endif()
    endforeach()
    set(target pr_${module})
    if (BACKEND_SOURCES)
        target_sources(${target} PRIVATE ${BACKEND_SOURCES})
    endif()
    if (BACKEND_INCLUDE)
        pr_headers(${target} PUBLIC ${BACKEND_INCLUDE})
    endif()
    target_compile_definitions(${target} PRIVATE PR_${MODULE}_HAS_${BACKEND})
    set_property(GLOBAL APPEND PROPERTY PR_BACKENDS_BUILT_${MODULE} ${BACKEND})
    foreach(unit IN LISTS BACKEND_SHARED)
        string(TOUPPER "${module}_${unit}" key)
        get_property(units GLOBAL PROPERTY PR_SHARED_UNITS)
        if (NOT key IN_LIST units)
            message(FATAL_ERROR "pr_backend(${module} ${backend}): no shared unit ${unit} (pr_backend_shared)")
        endif()
        get_property(added GLOBAL PROPERTY PR_SHARED_ADDED_${key})
        if (added)
            continue()
        endif()
        get_property(sources GLOBAL PROPERTY PR_SHARED_SOURCES_${key})
        get_property(include GLOBAL PROPERTY PR_SHARED_INCLUDE_${key})
        if (sources)
            target_sources(${target} PRIVATE ${sources})
        endif()
        if (include)
            pr_headers(${target} PUBLIC ${include})
        endif()
        set_property(GLOBAL PROPERTY PR_SHARED_ADDED_${key} TRUE)
    endforeach()
endfunction()

# Sets `out` to TRUE if the build includes `backend` of `module`, and FALSE otherwise. Call it after the module declares
# the backend.
function(pr_backend_built module backend out)
    string(TOUPPER "${module}" MODULE)
    string(TOUPPER "${backend}" BACKEND)
    get_property(built GLOBAL PROPERTY PR_BACKENDS_BUILT_${MODULE})
    if (BACKEND IN_LIST built)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Adds the selector library of `module`, prax::<module>_select. It links the module and lib/backend, and receives the
# module's PR_<MODULE>_HAS_<BACKEND> definitions. Call it after the module declares its backends.
function(pr_select module)
    cmake_parse_arguments(PARSE_ARGV 1 SELECT "" "INCLUDE" "SOURCES;LINK")
    string(TOUPPER "${module}" MODULE)
    pr_library(${module}_select INCLUDE ${SELECT_INCLUDE} ${SELECT_SOURCES})
    target_link_libraries(pr_${module}_select PUBLIC prax::${module} prax::backend ${SELECT_LINK}
        PRIVATE prax::core_cpp)
    get_property(built GLOBAL PROPERTY PR_BACKENDS_BUILT_${MODULE})
    foreach(backend IN LISTS built)
        target_compile_definitions(pr_${module}_select PRIVATE PR_${MODULE}_HAS_${backend})
    endforeach()
endfunction()
