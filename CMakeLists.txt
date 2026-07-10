cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Default vcpkg triplet")
endif()

option(RUVIA_BUILD_CORE "Build the ruvia-core runtime library" ON)
option(RUVIA_BUILD_HTTP "Build the standalone ruvia-http protocol library" ON)
option(RUVIA_BUILD_WEB "Build the complete ruvia-web framework" ON)
option(RUVIA_BUILD_EXAMPLES "Build Ruvia examples" OFF)
option(RUVIA_BUILD_TESTS "Build Ruvia unit tests" OFF)

option(RUVIA_ENABLE_MARIADB "Build MariaDB-compatible database support" OFF)
option(RUVIA_ENABLE_REDIS "Build Redis support" OFF)
option(RUVIA_ENABLE_JWT "Build JWT helper support" OFF)

if(NOT RUVIA_BUILD_CORE AND NOT RUVIA_BUILD_HTTP AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "At least one Ruvia component must be enabled")
endif()
if(RUVIA_BUILD_WEB AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_WEB=ON requires RUVIA_BUILD_CORE=ON and RUVIA_BUILD_HTTP=ON")
endif()
if(NOT RUVIA_BUILD_WEB AND
   (RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_REDIS OR RUVIA_ENABLE_JWT))
    message(FATAL_ERROR
        "RUVIA_ENABLE_MARIADB/REDIS/JWT require RUVIA_BUILD_WEB=ON")
endif()
if(RUVIA_BUILD_EXAMPLES AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "RUVIA_BUILD_EXAMPLES=ON requires RUVIA_BUILD_WEB=ON")
endif()

# Keep manifest-mode dependency installation aligned with the selected targets.
# These values must be finalized before project(), when the vcpkg toolchain runs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_ruvia_vcpkg_features ${VCPKG_MANIFEST_FEATURES})
    list(REMOVE_ITEM _ruvia_vcpkg_features
        core http web runtime protocol framework mariadb redis jwt http-client)
    if(RUVIA_BUILD_CORE)
        list(APPEND _ruvia_vcpkg_features runtime)
    endif()
    if(RUVIA_BUILD_HTTP)
        list(APPEND _ruvia_vcpkg_features protocol)
    endif()
    if(RUVIA_BUILD_WEB)
        list(APPEND _ruvia_vcpkg_features framework)
    endif()
    if(RUVIA_ENABLE_MARIADB)
        list(APPEND _ruvia_vcpkg_features mariadb)
    endif()
    if(RUVIA_ENABLE_REDIS)
        list(APPEND _ruvia_vcpkg_features redis)
    endif()
    if(RUVIA_ENABLE_JWT)
        list(APPEND _ruvia_vcpkg_features jwt)
    endif()
    list(REMOVE_DUPLICATES _ruvia_vcpkg_features)
    set(
        VCPKG_MANIFEST_FEATURES
        "${_ruvia_vcpkg_features}"
        CACHE STRING "vcpkg features derived from the selected Ruvia components" FORCE)
    set(
        VCPKG_MANIFEST_NO_DEFAULT_FEATURES
        ON
        CACHE BOOL "Install only dependencies for selected Ruvia components" FORCE)
    unset(_ruvia_vcpkg_features)
endif()

project(ruvia VERSION 0.0.6 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(RUVIA_BUILD_CORE)
    find_package(asio CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    find_package(mimalloc CONFIG REQUIRED)
endif()

if(RUVIA_BUILD_HTTP)
    find_package(ZLIB REQUIRED)
    find_package(unofficial-brotli CONFIG REQUIRED)
    find_package(zstd CONFIG REQUIRED)

    if(TARGET zstd::libzstd)
        set(RUVIA_ZSTD_TARGET zstd::libzstd)
    elseif(TARGET zstd::libzstd_static)
        set(RUVIA_ZSTD_TARGET zstd::libzstd_static)
    else()
        set(RUVIA_ZSTD_TARGET zstd::libzstd_shared)
    endif()
endif()

if(RUVIA_BUILD_WEB)
    find_package(OpenSSL REQUIRED)
endif()

if(RUVIA_ENABLE_MARIADB)
    find_package(unofficial-libmariadb CONFIG REQUIRED)
endif()

if(RUVIA_ENABLE_REDIS)
    find_package(hiredis CONFIG REQUIRED)
endif()

function(ruvia_deduplicate_link_interface target)
    if(NOT TARGET ${target})
        return()
    endif()

    get_target_property(_ruvia_link_interface ${target} INTERFACE_LINK_LIBRARIES)
    if(NOT _ruvia_link_interface)
        return()
    endif()

    list(REMOVE_DUPLICATES _ruvia_link_interface)
    set_target_properties(${target} PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_ruvia_link_interface}")
endfunction()

if(RUVIA_ENABLE_MARIADB)
    ruvia_deduplicate_link_interface(unofficial::libmariadb)
endif()

function(ruvia_configure_library target)
    target_compile_features(${target} PUBLIC cxx_std_23)

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(WIN32)
        target_compile_definitions(${target} PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    if(MSVC)
        target_compile_options(${target} PUBLIC /utf-8 /Zc:preprocessor)
        target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /FS)
        if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded")
            target_compile_options(${target}
                INTERFACE
                    "$<$<CXX_COMPILER_ID:MSVC>:/MT$<$<CONFIG:Debug>:d>>"
            )
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(ruvia_configure_runtime_library target)
    target_compile_definitions(${target}
        PUBLIC
            ASIO_STANDALONE
    )
    target_precompile_headers(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")
endfunction()

if(RUVIA_BUILD_CORE)
    set(RUVIA_MIMALLOC_TARGET mimalloc)
    if(TARGET mimalloc-static)
        get_target_property(_ruvia_mimalloc_static_type mimalloc-static TYPE)
        if(NOT _ruvia_mimalloc_static_type STREQUAL "INTERFACE_LIBRARY")
            set(RUVIA_MIMALLOC_TARGET mimalloc-static)
        endif()
    endif()
endif()

function(ruvia_install_target target component)
    install(
        TARGETS ${target}
        EXPORT ruviaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component} NAMELINK_COMPONENT ${component}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${component}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endfunction()

function(ruvia_install_public_headers component)
    foreach(_ruvia_header ${ARGN})
        get_filename_component(_ruvia_header_dir "${_ruvia_header}" DIRECTORY)
        file(
            RELATIVE_PATH
            _ruvia_header_install_subdir
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header_dir}"
        )
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_ruvia_header_install_subdir}"
            COMPONENT ${component}
        )
    endforeach()
endfunction()

function(ruvia_install_public_header_tree component relative_dir)
    file(
        RELATIVE_PATH
        _ruvia_header_tree_install_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}"
    )
    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_ruvia_header_tree_install_dir}"
        COMPONENT ${component}
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.inl"
    )
endfunction()

set(RUVIA_BUILT_COMPONENTS)
if(RUVIA_BUILD_CORE)
    add_subdirectory(ruvia-core)
    list(APPEND RUVIA_BUILT_COMPONENTS core)
endif()
if(RUVIA_BUILD_HTTP)
    add_subdirectory(ruvia-http)
    list(APPEND RUVIA_BUILT_COMPONENTS http)
endif()
if(RUVIA_BUILD_WEB)
    add_subdirectory(ruvia-web)
    list(APPEND RUVIA_BUILT_COMPONENTS web)
endif()
string(JOIN "," RUVIA_BUILT_COMPONENTS_CSV ${RUVIA_BUILT_COMPONENTS})

foreach(_ruvia_target IN ITEMS ruvia-core ruvia-http ruvia-web)
    if(TARGET ${_ruvia_target})
        target_link_options(${_ruvia_target}
            INTERFACE
                $<$<PLATFORM_ID:Darwin>:LINKER:-no_warn_duplicate_libraries>
        )
    endif()
endforeach()

if(WIN32 AND RUVIA_BUILD_CORE)
    target_link_libraries(ruvia-core PUBLIC Mswsock Ws2_32)
    set_target_properties(ruvia-core PROPERTIES
        INTERFACE_LINK_LIBRARIES_DIRECT "$<TARGET_LINKER_FILE:${RUVIA_MIMALLOC_TARGET}>"
    )
    target_link_options(ruvia-core INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/INCLUDE:mi_version>")
endif()

if(RUVIA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RUVIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set(_ruvia_config [=[
include(CMakeFindDependencyMacro)

set(RUVIA_ENABLE_MARIADB @RUVIA_ENABLE_MARIADB@)
set(RUVIA_ENABLE_REDIS @RUVIA_ENABLE_REDIS@)
set(RUVIA_ENABLE_JWT @RUVIA_ENABLE_JWT@)
set(ruvia_AVAILABLE_COMPONENTS "@RUVIA_BUILT_COMPONENTS@")

set(_ruvia_requested_components ${ruvia_FIND_COMPONENTS})
if(NOT _ruvia_requested_components)
    set(_ruvia_requested_components ${ruvia_AVAILABLE_COMPONENTS})
endif()

set(ruvia_FOUND TRUE)
foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(NOT _ruvia_component IN_LIST ruvia_AVAILABLE_COMPONENTS)
        set(ruvia_${_ruvia_component}_FOUND FALSE)
        set(ruvia_FOUND FALSE)
        set(ruvia_NOT_FOUND_MESSAGE
            "Requested component '${_ruvia_component}' is not installed; available components: ${ruvia_AVAILABLE_COMPONENTS}")
    endif()
endforeach()
if(NOT ruvia_FOUND)
    return()
endif()

set(_ruvia_needs_core FALSE)
set(_ruvia_needs_http FALSE)
set(_ruvia_needs_web FALSE)

foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(_ruvia_component STREQUAL "core")
        set(_ruvia_needs_core TRUE)
    elseif(_ruvia_component STREQUAL "http")
        set(_ruvia_needs_http TRUE)
    elseif(_ruvia_component STREQUAL "web")
        set(_ruvia_needs_core TRUE)
        set(_ruvia_needs_http TRUE)
        set(_ruvia_needs_web TRUE)
    endif()
endforeach()

if(_ruvia_needs_core)
    find_dependency(mimalloc CONFIG REQUIRED)
    find_dependency(asio CONFIG REQUIRED)
    find_dependency(Threads REQUIRED)
endif()

if(_ruvia_needs_http)
    find_dependency(ZLIB REQUIRED)
    find_dependency(unofficial-brotli CONFIG REQUIRED)
    find_dependency(zstd CONFIG REQUIRED)
endif()

if(_ruvia_needs_web)
    find_dependency(OpenSSL REQUIRED)
endif()

if(_ruvia_needs_web AND RUVIA_ENABLE_MARIADB)
    find_dependency(unofficial-libmariadb CONFIG REQUIRED)
endif()

if(_ruvia_needs_web AND RUVIA_ENABLE_REDIS)
    find_dependency(hiredis CONFIG REQUIRED)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/ruviaTargets.cmake")

foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(TARGET ruvia::${_ruvia_component})
        set(ruvia_${_ruvia_component}_FOUND TRUE)
    else()
        set(ruvia_${_ruvia_component}_FOUND FALSE)
    endif()
endforeach()
]=])

string(CONFIGURE "${_ruvia_config}" _ruvia_config @ONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake "${_ruvia_config}")

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# The package config + generated targets export are the find_package() glue shared by
# every component. Tag them with a dedicated Development component so a component-scoped
# install (cmake --install --component ...) can pull them deliberately instead of
# silently omitting them (a full install still ships them, as it installs all components).
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)

install(
    EXPORT ruviaTargets
    FILE ruviaTargets.cmake
    NAMESPACE ruvia::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)
