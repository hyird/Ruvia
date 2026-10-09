cmake_minimum_required(VERSION 3.28)

option(RUVIA_BUILD_CORE "Build ruvia-core" ON)
option(RUVIA_BUILD_HTTP "Build ruvia-http" ON)
option(RUVIA_BUILD_WEB "Build ruvia-web" ON)
option(RUVIA_BUILD_EXAMPLES "Build examples" OFF)
option(RUVIA_BUILD_TESTS "Build tests" OFF)
option(RUVIA_ENABLE_MARIADB "Enable MariaDB" OFF)
option(RUVIA_ENABLE_POSTGRESQL "Enable PostgreSQL" OFF)
option(RUVIA_ENABLE_REDIS "Enable Redis" OFF)
option(RUVIA_ENABLE_JWT "Enable JWT" OFF)

if(RUVIA_BUILD_WEB AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_WEB requires core and HTTP")
endif()
if(RUVIA_BUILD_EXAMPLES AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "RUVIA_BUILD_EXAMPLES requires Web")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ruvia VERSION 0.2.0 LANGUAGES C CXX)

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Ruvia Windows builds require MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-static)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(cmake/ruvia_targets.cmake)
include(cmake/ruvia_dependencies.cmake)

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
ruvia_define_dependencies_target()
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
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ruvia_dependencies.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ruvia_dependency_configure.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ruvia_dependency_build.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)
