find_path(nghttp2_INCLUDE_DIR
    NAMES nghttp2/nghttp2.h
)

find_library(nghttp2_LIBRARY_RELEASE
    NAMES nghttp2
    PATH_SUFFIXES lib
)
find_library(nghttp2_LIBRARY_DEBUG
    NAMES nghttp2d nghttp2
    PATH_SUFFIXES debug/lib
)

include(SelectLibraryConfigurations)
select_library_configurations(nghttp2)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(nghttp2
    REQUIRED_VARS nghttp2_INCLUDE_DIR nghttp2_LIBRARY
)

if(nghttp2_FOUND AND NOT TARGET nghttp2::nghttp2)
    add_library(nghttp2::nghttp2 UNKNOWN IMPORTED)
    set_target_properties(nghttp2::nghttp2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${nghttp2_INCLUDE_DIR}"
    )
    if(WIN32)
        set_property(TARGET nghttp2::nghttp2 APPEND PROPERTY
            INTERFACE_COMPILE_DEFINITIONS NGHTTP2_NO_SSIZE_T)
    endif()
    if(nghttp2_LIBRARY_RELEASE)
        set_property(TARGET nghttp2::nghttp2 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
        set_target_properties(nghttp2::nghttp2 PROPERTIES
            IMPORTED_LOCATION_RELEASE "${nghttp2_LIBRARY_RELEASE}"
        )
    endif()
    if(nghttp2_LIBRARY_DEBUG)
        set_property(TARGET nghttp2::nghttp2 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(nghttp2::nghttp2 PROPERTIES
            IMPORTED_LOCATION_DEBUG "${nghttp2_LIBRARY_DEBUG}"
        )
    endif()
    if(NOT nghttp2_LIBRARY_RELEASE AND NOT nghttp2_LIBRARY_DEBUG)
        set_target_properties(nghttp2::nghttp2 PROPERTIES
            IMPORTED_LOCATION "${nghttp2_LIBRARY}"
        )
    endif()
endif()

mark_as_advanced(
    nghttp2_INCLUDE_DIR
    nghttp2_LIBRARY_RELEASE
    nghttp2_LIBRARY_DEBUG
)
