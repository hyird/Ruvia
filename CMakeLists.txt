cmake_minimum_required(VERSION 3.24)

option(RUVIA_BUILD_CORE "Build ruvia-core" ON)
option(RUVIA_BUILD_HTTP "Build ruvia-http" ON)
option(RUVIA_BUILD_WEB "Build ruvia-web" ON)
option(RUVIA_BUILD_EDGE "Build ruvia-edge" OFF)
option(RUVIA_BUILD_EXAMPLES "Build examples" OFF)
option(RUVIA_BUILD_TESTS "Build tests" OFF)
option(RUVIA_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS "Run HTTP/2 conformance tests" OFF)
option(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS "Run PostgreSQL integration tests" OFF)
option(RUVIA_ENABLE_MARIADB "Enable MariaDB" OFF)
option(RUVIA_ENABLE_POSTGRESQL "Enable PostgreSQL" OFF)
option(RUVIA_ENABLE_REDIS "Enable Redis" OFF)
option(RUVIA_ENABLE_JWT "Enable JWT" OFF)

if(RUVIA_BUILD_WEB AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_WEB requires core and HTTP")
endif()
if(RUVIA_BUILD_EDGE AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_EDGE requires core and HTTP")
endif()

set(_ruvia_options RUVIA_BUILD_CORE RUVIA_BUILD_HTTP RUVIA_BUILD_WEB RUVIA_BUILD_EDGE
    RUVIA_ENABLE_MARIADB RUVIA_ENABLE_POSTGRESQL RUVIA_ENABLE_REDIS RUVIA_ENABLE_JWT)
set(_ruvia_features runtime protocol framework edge mariadb postgresql redis jwt)
set(_ruvia_vcpkg_features)
foreach(_ruvia_option _ruvia_feature IN ZIP_LISTS _ruvia_options _ruvia_features)
    if(${_ruvia_option})
        list(APPEND _ruvia_vcpkg_features ${_ruvia_feature})
    endif()
endforeach()
set(VCPKG_MANIFEST_FEATURES "${_ruvia_vcpkg_features}" CACHE STRING "" FORCE)
set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON CACHE BOOL "" FORCE)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _ruvia_manifest)
string(JSON _ruvia_version GET "${_ruvia_manifest}" version-string)
project(ruvia VERSION ${_ruvia_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/RuviaHelpers.cmake)

set(RUVIA_BUILT_COMPONENTS)
foreach(_ruvia_component IN ITEMS core http web edge)
    string(TOUPPER ${_ruvia_component} _ruvia_component_upper)
    if(RUVIA_BUILD_${_ruvia_component_upper})
        add_subdirectory(ruvia-${_ruvia_component})
        list(APPEND RUVIA_BUILT_COMPONENTS ${_ruvia_component})
    endif()
endforeach()
string(JOIN "," RUVIA_BUILT_COMPONENTS_CSV ${RUVIA_BUILT_COMPONENTS})

if(RUVIA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(RUVIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(RUVIA_BUILD_BENCHMARKS)
    add_subdirectory(tests/benchmarks)
endif()

configure_package_config_file(
    cmake/ruviaConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)
