cmake_minimum_required(VERSION 3.24)

set(_ruvia_is_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_ruvia_is_top_level ON)
endif()

if(_ruvia_is_top_level
   AND NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND DEFINED ENV{VCPKG_ROOT}
   AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _ruvia_vcpkg_root)
    set(_ruvia_vcpkg_toolchain
        "${_ruvia_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    if(NOT EXISTS "${_ruvia_vcpkg_toolchain}")
        message(FATAL_ERROR
            "VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: "
            "$ENV{VCPKG_ROOT}")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${_ruvia_vcpkg_toolchain}" CACHE FILEPATH
        "Toolchain file selected from VCPKG_ROOT")
endif()

option(RUVIA_BUILD_CORE "Build ruvia-core" ON)
option(RUVIA_BUILD_HTTP "Build ruvia-http" ON)
option(RUVIA_BUILD_WEB "Build ruvia-web" ON)
option(RUVIA_BUILD_EXAMPLES "Build examples" OFF)
option(RUVIA_BUILD_TESTS "Build tests" OFF)
option(RUVIA_ENABLE_MARIADB "Enable MariaDB" OFF)
option(RUVIA_ENABLE_POSTGRESQL "Enable PostgreSQL" OFF)
option(RUVIA_ENABLE_REDIS "Enable Redis" OFF)
option(RUVIA_ENABLE_JWT "Enable JWT" OFF)

if(_ruvia_is_top_level AND NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
    set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
endif()
if(_ruvia_is_top_level AND NOT "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" STREQUAL "")
    set(VCPKG_HOST_TRIPLET "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" CACHE STRING "")
endif()

if(RUVIA_BUILD_WEB AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_WEB requires core and HTTP")
endif()
if(RUVIA_BUILD_EXAMPLES AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "RUVIA_BUILD_EXAMPLES requires Web")
endif()

set(_ruvia_options RUVIA_BUILD_CORE RUVIA_BUILD_HTTP RUVIA_BUILD_WEB
    RUVIA_ENABLE_MARIADB RUVIA_ENABLE_POSTGRESQL RUVIA_ENABLE_REDIS RUVIA_ENABLE_JWT)
set(_ruvia_features runtime protocol framework mariadb postgresql redis jwt)
set(_ruvia_vcpkg_features)
foreach(_ruvia_option _ruvia_feature IN ZIP_LISTS _ruvia_options _ruvia_features)
    if(${_ruvia_option})
        list(APPEND _ruvia_vcpkg_features ${_ruvia_feature})
    endif()
endforeach()
if(_ruvia_is_top_level)
    set(VCPKG_MANIFEST_FEATURES "${_ruvia_vcpkg_features}" CACHE STRING "" FORCE)
    set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON CACHE BOOL "" FORCE)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _ruvia_manifest)
string(JSON _ruvia_version GET "${_ruvia_manifest}" version-string)

set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ruvia VERSION ${_ruvia_version} LANGUAGES CXX)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Ruvia Windows builds require MSVC")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(cmake/RuviaTargets.cmake)

foreach(_ruvia_component IN ITEMS core http web)
    string(TOUPPER ${_ruvia_component} _ruvia_component_upper)
    if(RUVIA_BUILD_${_ruvia_component_upper})
        add_subdirectory(ruvia-${_ruvia_component})
    endif()
endforeach()

if(RUVIA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(RUVIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
configure_package_config_file(
    cmake/ruviaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)
