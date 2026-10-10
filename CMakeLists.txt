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

# A top-level build installs exactly the manifest features of the enabled
# components. Embedding projects own their toolchain and dependency manifest.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _ruvia_vcpkg_root)
        set(CMAKE_TOOLCHAIN_FILE "${_ruvia_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "Toolchain file selected from VCPKG_ROOT")
        if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
            message(FATAL_ERROR "VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: $ENV{VCPKG_ROOT}")
        endif()
    endif()
    # MSVC builds use the static runtime, so dependencies use the matching triplet.
    if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
        set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "vcpkg target triplet")
    endif()
    set(_ruvia_options RUVIA_BUILD_CORE RUVIA_BUILD_HTTP RUVIA_BUILD_WEB
        RUVIA_ENABLE_MARIADB RUVIA_ENABLE_POSTGRESQL RUVIA_ENABLE_REDIS)
    set(_ruvia_features runtime protocol framework mariadb postgresql redis)
    set(_ruvia_vcpkg_features)
    foreach(_ruvia_option _ruvia_feature IN ZIP_LISTS _ruvia_options _ruvia_features)
        if(${_ruvia_option})
            list(APPEND _ruvia_vcpkg_features ${_ruvia_feature})
        endif()
    endforeach()
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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-static)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(cmake/ruvia_targets.cmake)

# Third-party packages. Subdirectories link these lists; ruviaConfig.cmake
# repeats the matching find_dependency calls for installed consumers.
if(RUVIA_BUILD_CORE)
    find_package(asio CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    set(RUVIA_CORE_DEPENDENCIES asio::asio Threads::Threads)
endif()
if(RUVIA_BUILD_HTTP)
    find_package(ZLIB REQUIRED)
    find_package(unofficial-brotli CONFIG REQUIRED)
    find_package(zstd CONFIG REQUIRED)
    find_package(ngtcp2 1.25.0 CONFIG REQUIRED)
    set(RUVIA_HTTP_DEPENDENCIES ZLIB::ZLIB unofficial::brotli::brotlienc
        unofficial::brotli::brotlidec zstd::libzstd ngtcp2::ngtcp2_static)
endif()
if(RUVIA_BUILD_WEB)
    # OpenSSL 3.5 introduced the external QUIC TLS callbacks used by HTTP/3.
    find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
    set(RUVIA_WEB_DEPENDENCIES OpenSSL::SSL OpenSSL::Crypto)
    set(RUVIA_OPENSSL_DEFINITIONS OPENSSL_API_COMPAT=30000 OPENSSL_NO_DEPRECATED)
    if(RUVIA_ENABLE_MARIADB)
        find_package(unofficial-libmariadb CONFIG REQUIRED)
        list(APPEND RUVIA_WEB_DEPENDENCIES unofficial::libmariadb)
    endif()
    if(RUVIA_ENABLE_POSTGRESQL)
        find_package(PostgreSQL REQUIRED)
        list(APPEND RUVIA_WEB_DEPENDENCIES PostgreSQL::PostgreSQL)
        # vcpkg's static libpq splits frontend symbols between the cyclic
        # pgcommon/pgport pair, which GNU linkers must rescan.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND RUVIA_WEB_DEPENDENCIES
                "$<LINK_GROUP:RESCAN,$<$<CONFIG:Debug>:${PostgreSQL_pgcommon_LIBRARY_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgcommon_LIBRARY_RELEASE}>,$<$<CONFIG:Debug>:${PostgreSQL_pgport_LIBRARY_DEBUG}>$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgport_LIBRARY_RELEASE}>>")
        endif()
    endif()
    if(RUVIA_ENABLE_REDIS)
        find_package(hiredis CONFIG REQUIRED)
        list(APPEND RUVIA_WEB_DEPENDENCIES hiredis::hiredis)
    endif()
endif()

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
