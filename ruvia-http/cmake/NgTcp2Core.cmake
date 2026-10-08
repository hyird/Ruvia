# Find only ngtcp2's protocol core. Its package config can pull in an optional
# OpenSSL crypto target, which must not become a dependency of HTTP-only builds.
function(ruvia_find_ngtcp2_core)
    if(TARGET ruvia_http_ngtcp2_core)
        return()
    endif()

    set(_ruvia_ngtcp2_prefix_hints)
    foreach(_ruvia_root_var IN ITEMS ngtcp2_ROOT NGTCP2_ROOT)
        if(DEFINED ${_ruvia_root_var} AND NOT "${${_ruvia_root_var}}" STREQUAL "")
            list(APPEND _ruvia_ngtcp2_prefix_hints "${${_ruvia_root_var}}")
        endif()
    endforeach()
    if(DEFINED ngtcp2_DIR AND IS_DIRECTORY "${ngtcp2_DIR}")
        if(ngtcp2_DIR MATCHES "/share/ngtcp2/?$")
            get_filename_component(_ruvia_ngtcp2_config_prefix
                "${ngtcp2_DIR}/../.." ABSOLUTE)
        else()
            get_filename_component(_ruvia_ngtcp2_config_prefix
                "${ngtcp2_DIR}/../../.." ABSOLUTE)
        endif()
        list(APPEND _ruvia_ngtcp2_prefix_hints "${_ruvia_ngtcp2_config_prefix}")
    endif()

    find_path(_ruvia_ngtcp2_include_dir NAMES ngtcp2/version.h
        HINTS ${_ruvia_ngtcp2_prefix_hints}
        PATH_SUFFIXES include NO_DEFAULT_PATH NO_CACHE)
    if(NOT _ruvia_ngtcp2_include_dir)
        find_path(_ruvia_ngtcp2_include_dir NAMES ngtcp2/version.h
            PATH_SUFFIXES include NO_CACHE)
    endif()
    if(_ruvia_ngtcp2_include_dir)
        get_filename_component(_ruvia_ngtcp2_include_parent
            "${_ruvia_ngtcp2_include_dir}" DIRECTORY)
    endif()

    find_library(_ruvia_ngtcp2_core_library NAMES ngtcp2 ngtcp2_static
        PATHS "${_ruvia_ngtcp2_include_parent}"
        PATH_SUFFIXES lib lib64 "lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        NO_DEFAULT_PATH NO_CACHE)
    find_library(_ruvia_ngtcp2_core_library_debug NAMES ngtcp2 ngtcp2_static
        PATHS "${_ruvia_ngtcp2_include_parent}/debug"
        PATH_SUFFIXES lib lib64 "lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        NO_DEFAULT_PATH NO_CACHE)
    if(NOT _ruvia_ngtcp2_include_dir OR NOT _ruvia_ngtcp2_core_library)
        message(FATAL_ERROR "ruvia-http requires the ngtcp2 QUIC core library and headers")
    endif()
    if(WIN32 AND MSVC AND NOT _ruvia_ngtcp2_core_library_debug)
        message(FATAL_ERROR
            "ruvia-http requires an ngtcp2 Debug library for MSVC /MTd builds")
    endif()

    file(STRINGS "${_ruvia_ngtcp2_include_dir}/ngtcp2/version.h"
        _ruvia_ngtcp2_version_line REGEX "^#define NGTCP2_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"")
    if(NOT _ruvia_ngtcp2_version_line MATCHES "NGTCP2_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
        message(FATAL_ERROR "Could not determine the ngtcp2 core version from version.h")
    endif()
    set(_ruvia_ngtcp2_version "${CMAKE_MATCH_1}")
    if(_ruvia_ngtcp2_version VERSION_LESS 1.25.0)
        message(FATAL_ERROR "ruvia-http requires ngtcp2 1.25.0 or newer; found ${_ruvia_ngtcp2_version}")
    endif()

    add_library(ruvia_http_ngtcp2_core UNKNOWN IMPORTED GLOBAL)
    set_target_properties(ruvia_http_ngtcp2_core PROPERTIES
        IMPORTED_LOCATION "${_ruvia_ngtcp2_core_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ruvia_ngtcp2_include_dir}"
    )
    # Without a distinct debug archive, use the generic release location for
    # non-MSVC configurations. MSVC requires a real Debug archive above.
    if(_ruvia_ngtcp2_core_library_debug)
        set_target_properties(ruvia_http_ngtcp2_core PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
            IMPORTED_LOCATION_DEBUG "${_ruvia_ngtcp2_core_library_debug}"
            IMPORTED_LOCATION_RELEASE "${_ruvia_ngtcp2_core_library}"
            IMPORTED_LOCATION_RELWITHDEBINFO "${_ruvia_ngtcp2_core_library}"
            IMPORTED_LOCATION_MINSIZEREL "${_ruvia_ngtcp2_core_library}"
        )
    endif()
    if(WIN32 OR _ruvia_ngtcp2_core_library MATCHES "\\.a$")
        set_property(TARGET ruvia_http_ngtcp2_core PROPERTY
            INTERFACE_COMPILE_DEFINITIONS NGTCP2_STATICLIB)
    endif()
endfunction()
