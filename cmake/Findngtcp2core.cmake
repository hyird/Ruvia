find_path(ngtcp2core_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2.h
)

# Multi-config toolchains may put a debug prefix first in CMAKE_PREFIX_PATH.
# Prefer the matching installation's explicit library directories on Windows.
if(WIN32 AND ngtcp2core_INCLUDE_DIR)
    find_library(ngtcp2core_LIBRARY_RELEASE
        NAMES ngtcp2
        PATHS "${ngtcp2core_INCLUDE_DIR}/../lib"
        NO_DEFAULT_PATH
    )
    find_library(ngtcp2core_LIBRARY_DEBUG
        NAMES ngtcp2d ngtcp2
        PATHS "${ngtcp2core_INCLUDE_DIR}/../debug/lib"
        NO_DEFAULT_PATH
    )
endif()

find_library(ngtcp2core_LIBRARY_RELEASE
    NAMES ngtcp2
    PATH_SUFFIXES lib
)
find_library(ngtcp2core_LIBRARY_DEBUG
    NAMES ngtcp2d ngtcp2
    PATH_SUFFIXES debug/lib
)

include(SelectLibraryConfigurations)
select_library_configurations(ngtcp2core)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ngtcp2core
    REQUIRED_VARS ngtcp2core_INCLUDE_DIR ngtcp2core_LIBRARY
)

if(ngtcp2core_FOUND AND NOT TARGET ngtcp2::core)
    add_library(ngtcp2::core UNKNOWN IMPORTED)
    set_target_properties(ngtcp2::core PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ngtcp2core_INCLUDE_DIR}"
    )
    if(WIN32)
        # ngtcp2.h includes <ws2tcpip.h>; keep <windows.h> from defining the
        # min/max macros in every translation unit that sees the QUIC facade.
        set_property(TARGET ngtcp2::core APPEND PROPERTY
            INTERFACE_COMPILE_DEFINITIONS NOMINMAX)
    endif()
    if(ngtcp2core_LIBRARY_RELEASE)
        set_property(TARGET ngtcp2::core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
        set_target_properties(ngtcp2::core PROPERTIES
            IMPORTED_LOCATION_RELEASE "${ngtcp2core_LIBRARY_RELEASE}"
        )
    endif()
    if(ngtcp2core_LIBRARY_DEBUG)
        set_property(TARGET ngtcp2::core APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(ngtcp2::core PROPERTIES
            IMPORTED_LOCATION_DEBUG "${ngtcp2core_LIBRARY_DEBUG}"
        )
    endif()
    if(NOT ngtcp2core_LIBRARY_RELEASE AND NOT ngtcp2core_LIBRARY_DEBUG)
        set_target_properties(ngtcp2::core PROPERTIES
            IMPORTED_LOCATION "${ngtcp2core_LIBRARY}"
        )
    endif()
endif()

mark_as_advanced(
    ngtcp2core_INCLUDE_DIR
    ngtcp2core_LIBRARY_RELEASE
    ngtcp2core_LIBRARY_DEBUG
)
