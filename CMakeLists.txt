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
option(RUVIA_BUILD_EDGE "Build the ruvia-edge CDN edge node library" OFF)
option(RUVIA_BUILD_EXAMPLES "Build Ruvia examples" OFF)
option(RUVIA_BUILD_TESTS "Build Ruvia unit tests" OFF)
option(RUVIA_BUILD_BENCHMARKS "Build Ruvia protocol hot-path benchmarks" OFF)
option(RUVIA_BUILD_FUZZERS "Build Clang libFuzzer protocol targets" OFF)
option(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS "Run RFC 9113 wire conformance tests against a Ruvia h2c server" OFF)
option(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS "Run tests against a live PostgreSQL server" OFF)

set(_ruvia_sanitizer_flags OFF)
if(CMAKE_CXX_FLAGS MATCHES "(^|[ ;])[-/]fsanitize=" OR
   CMAKE_EXE_LINKER_FLAGS MATCHES "(^|[ ;])[-/]fsanitize=")
    set(_ruvia_sanitizer_flags ON)
endif()
set(_ruvia_mimalloc_override_default ON)
if(_ruvia_sanitizer_flags)
    set(_ruvia_mimalloc_override_default OFF)
endif()
option(
    RUVIA_ENABLE_MIMALLOC_OVERRIDE
    "Override the process allocator with mimalloc (incompatible with sanitizer interceptors)"
    ${_ruvia_mimalloc_override_default})
unset(_ruvia_mimalloc_override_default)
if(_ruvia_sanitizer_flags AND RUVIA_ENABLE_MIMALLOC_OVERRIDE)
    message(FATAL_ERROR
        "RUVIA_ENABLE_MIMALLOC_OVERRIDE=ON is incompatible with sanitizer interceptors")
endif()
unset(_ruvia_sanitizer_flags)

# Emitted for clangd and clang-tidy (.clang-tidy at the repo root); harmless
# for generators that do not support it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RUVIA_ENABLE_MARIADB "Build MariaDB-compatible database support" OFF)
option(RUVIA_ENABLE_POSTGRESQL "Build PostgreSQL database support" OFF)
option(RUVIA_ENABLE_REDIS "Build Redis support" OFF)
option(RUVIA_ENABLE_JWT "Build JWT helper support" OFF)

if(NOT RUVIA_BUILD_CORE AND NOT RUVIA_BUILD_HTTP AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "At least one Ruvia component must be enabled")
endif()
if(RUVIA_BUILD_WEB AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_WEB=ON requires RUVIA_BUILD_CORE=ON and RUVIA_BUILD_HTTP=ON")
endif()
if(RUVIA_BUILD_EDGE AND (NOT RUVIA_BUILD_CORE OR NOT RUVIA_BUILD_HTTP))
    message(FATAL_ERROR "RUVIA_BUILD_EDGE=ON requires RUVIA_BUILD_CORE=ON and RUVIA_BUILD_HTTP=ON")
endif()
if(NOT RUVIA_BUILD_WEB AND
   (RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL OR RUVIA_ENABLE_REDIS OR RUVIA_ENABLE_JWT))
    message(FATAL_ERROR
        "RUVIA_ENABLE_MARIADB/POSTGRESQL/REDIS/JWT require RUVIA_BUILD_WEB=ON")
endif()
if(RUVIA_BUILD_EXAMPLES AND NOT RUVIA_BUILD_WEB)
    message(FATAL_ERROR "RUVIA_BUILD_EXAMPLES=ON requires RUVIA_BUILD_WEB=ON")
endif()
if(RUVIA_BUILD_BENCHMARKS AND NOT RUVIA_BUILD_HTTP)
    message(FATAL_ERROR "RUVIA_BUILD_BENCHMARKS=ON requires RUVIA_BUILD_HTTP=ON")
endif()
if(RUVIA_BUILD_FUZZERS AND NOT RUVIA_BUILD_HTTP)
    message(FATAL_ERROR "RUVIA_BUILD_FUZZERS=ON requires RUVIA_BUILD_HTTP=ON")
endif()
if(RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS AND (NOT RUVIA_BUILD_TESTS OR NOT RUVIA_BUILD_WEB))
    message(FATAL_ERROR
        "RUVIA_ENABLE_HTTP2_CONFORMANCE_TESTS=ON requires RUVIA_BUILD_TESTS=ON and RUVIA_BUILD_WEB=ON")
endif()
if(RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS AND
   (NOT RUVIA_BUILD_TESTS OR NOT RUVIA_ENABLE_POSTGRESQL))
    message(FATAL_ERROR
        "RUVIA_ENABLE_POSTGRESQL_INTEGRATION_TESTS=ON requires RUVIA_BUILD_TESTS=ON and RUVIA_ENABLE_POSTGRESQL=ON")
endif()

# Keep manifest-mode dependency installation aligned with the selected targets.
# These values must be finalized before project(), when the vcpkg toolchain runs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_ruvia_vcpkg_features ${VCPKG_MANIFEST_FEATURES})
    list(REMOVE_ITEM _ruvia_vcpkg_features
        core http web edge runtime allocator-override protocol framework mariadb postgresql redis jwt http-client)
    if(RUVIA_BUILD_CORE)
        list(APPEND _ruvia_vcpkg_features runtime)
        if(RUVIA_ENABLE_MIMALLOC_OVERRIDE)
            list(APPEND _ruvia_vcpkg_features allocator-override)
        endif()
    endif()
    if(RUVIA_BUILD_HTTP)
        list(APPEND _ruvia_vcpkg_features protocol)
    endif()
    if(RUVIA_BUILD_WEB)
        list(APPEND _ruvia_vcpkg_features framework)
    endif()
    if(RUVIA_BUILD_EDGE)
        list(APPEND _ruvia_vcpkg_features edge)
    endif()
    if(RUVIA_ENABLE_MARIADB)
        list(APPEND _ruvia_vcpkg_features mariadb)
    endif()
    if(RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_vcpkg_features postgresql)
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

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _ruvia_manifest_json)
string(JSON _ruvia_manifest_version GET "${_ruvia_manifest_json}" version-string)
if(NOT _ruvia_manifest_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "vcpkg.json version-string must be a three-component numeric version, got: ${_ruvia_manifest_version}")
endif()
project(ruvia VERSION "${_ruvia_manifest_version}" LANGUAGES CXX)
unset(_ruvia_manifest_json)
unset(_ruvia_manifest_version)

if(RUVIA_BUILD_FUZZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "RUVIA_BUILD_FUZZERS=ON requires Clang with libFuzzer")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
# Shared build/install/export helpers. Each component's CMakeLists finds its own
# dependencies and installs and exports itself using these; the root adds no
# per-target build logic beyond wiring the components together below.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RuviaHelpers.cmake)

# Each component runs find_package() in its own subdirectory now, so make the
# resulting imported targets global: a sibling component (e.g. web resolving
# core's transitive asio link) must see targets another component discovered.
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
if(RUVIA_BUILD_EDGE)
    add_subdirectory(ruvia-edge)
    list(APPEND RUVIA_BUILT_COMPONENTS edge)
endif()
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

if(RUVIA_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# Umbrella package config: the shared find_package(ruvia) entry point that loads
# whichever component export sets are present in an install prefix. The component
# targets files themselves are installed by each component's own CMakeLists.
set(_ruvia_config [=[
include(CMakeFindDependencyMacro)

set(ruvia_VERSION "@PROJECT_VERSION@")
set(RUVIA_ENABLE_MARIADB @RUVIA_ENABLE_MARIADB@)
set(RUVIA_ENABLE_POSTGRESQL @RUVIA_ENABLE_POSTGRESQL@)
set(RUVIA_ENABLE_REDIS @RUVIA_ENABLE_REDIS@)
set(RUVIA_ENABLE_JWT @RUVIA_ENABLE_JWT@)

# Discover what is actually present in this install prefix. A build may contain
# every target while a component-scoped install deliberately copies only one
# export set, so build-time options are not an honest package inventory.
set(_ruvia_package_components "@RUVIA_BUILT_COMPONENTS@")
set(ruvia_AVAILABLE_COMPONENTS)
foreach(_ruvia_component IN LISTS _ruvia_package_components)
    set(ruvia_${_ruvia_component}_FOUND FALSE)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ruvia-${_ruvia_component}-targets.cmake")
        set(_ruvia_${_ruvia_component}_installed TRUE)
    else()
        set(_ruvia_${_ruvia_component}_installed FALSE)
    endif()
endforeach()

if(_ruvia_core_installed)
    list(APPEND ruvia_AVAILABLE_COMPONENTS core)
endif()
if(_ruvia_http_installed)
    list(APPEND ruvia_AVAILABLE_COMPONENTS http)
endif()
# web's exported link interface names both lower targets. Do not advertise an
# unusable partial web install as an available component.
if(_ruvia_web_installed AND _ruvia_core_installed AND _ruvia_http_installed)
    list(APPEND ruvia_AVAILABLE_COMPONENTS web)
endif()
# edge's exported link interface names both lower targets as well.
if(_ruvia_edge_installed AND _ruvia_core_installed AND _ruvia_http_installed)
    list(APPEND ruvia_AVAILABLE_COMPONENTS edge)
endif()

set(_ruvia_requested_components ${ruvia_FIND_COMPONENTS})
if(NOT _ruvia_requested_components)
    set(_ruvia_requested_components ${ruvia_AVAILABLE_COMPONENTS})
endif()

set(ruvia_FOUND TRUE)
set(_ruvia_components_to_load)
foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(_ruvia_component IN_LIST ruvia_AVAILABLE_COMPONENTS)
        list(APPEND _ruvia_components_to_load ${_ruvia_component})
    endif()
endforeach()

set(_ruvia_needs_core FALSE)
set(_ruvia_needs_http FALSE)
set(_ruvia_needs_web FALSE)
set(_ruvia_needs_edge FALSE)

foreach(_ruvia_component IN LISTS _ruvia_components_to_load)
    if(_ruvia_component STREQUAL "core")
        set(_ruvia_needs_core TRUE)
    elseif(_ruvia_component STREQUAL "http")
        set(_ruvia_needs_http TRUE)
    elseif(_ruvia_component STREQUAL "web")
        set(_ruvia_needs_core TRUE)
        set(_ruvia_needs_http TRUE)
        set(_ruvia_needs_web TRUE)
    elseif(_ruvia_component STREQUAL "edge")
        set(_ruvia_needs_core TRUE)
        set(_ruvia_needs_http TRUE)
        set(_ruvia_needs_edge TRUE)
    endif()
endforeach()

if(_ruvia_needs_core)
    find_dependency(mimalloc CONFIG)
    find_dependency(asio CONFIG)
    find_dependency(Threads)
endif()

if(_ruvia_needs_http)
    find_dependency(ZLIB)
    find_dependency(unofficial-brotli CONFIG)
    find_dependency(zstd CONFIG)
endif()

if(_ruvia_needs_web OR _ruvia_needs_edge)
    find_dependency(OpenSSL)
endif()

if(_ruvia_needs_web AND RUVIA_ENABLE_MARIADB)
    find_dependency(unofficial-libmariadb CONFIG)
endif()

if(_ruvia_needs_web AND RUVIA_ENABLE_POSTGRESQL)
    find_dependency(PostgreSQL)
endif()

if(_ruvia_needs_web AND RUVIA_ENABLE_REDIS)
    find_dependency(hiredis CONFIG)
endif()

if(_ruvia_needs_core)
    include("${CMAKE_CURRENT_LIST_DIR}/ruvia-core-targets.cmake")
endif()
if(_ruvia_needs_http)
    include("${CMAKE_CURRENT_LIST_DIR}/ruvia-http-targets.cmake")
endif()
if(_ruvia_needs_web)
    include("${CMAKE_CURRENT_LIST_DIR}/ruvia-web-targets.cmake")
endif()
if(_ruvia_needs_edge)
    include("${CMAKE_CURRENT_LIST_DIR}/ruvia-edge-targets.cmake")
endif()

foreach(_ruvia_component IN ITEMS core http web edge)
    if(TARGET ruvia::${_ruvia_component})
        set(ruvia_${_ruvia_component}_FOUND TRUE)
    endif()
endforeach()

set(_ruvia_missing_required_components)
foreach(_ruvia_component IN LISTS ruvia_FIND_COMPONENTS)
    if(ruvia_FIND_REQUIRED_${_ruvia_component} AND
       NOT ruvia_${_ruvia_component}_FOUND)
        list(APPEND _ruvia_missing_required_components ${_ruvia_component})
    endif()
endforeach()
if(_ruvia_missing_required_components)
    set(ruvia_FOUND FALSE)
    string(JOIN ", " _ruvia_missing_required_text ${_ruvia_missing_required_components})
    string(JOIN ", " _ruvia_available_text ${ruvia_AVAILABLE_COMPONENTS})
    if(_ruvia_available_text STREQUAL "")
        set(_ruvia_available_text "none")
    endif()
    set(ruvia_NOT_FOUND_MESSAGE
        "Required component(s) not installed: ${_ruvia_missing_required_text}; available components: ${_ruvia_available_text}")
elseif(NOT ruvia_FIND_COMPONENTS AND NOT ruvia_AVAILABLE_COMPONENTS)
    set(ruvia_FOUND FALSE)
    set(ruvia_NOT_FOUND_MESSAGE "No usable Ruvia component is installed in this prefix")
endif()
]=])

string(CONFIGURE "${_ruvia_config}" _ruvia_config @ONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake "${_ruvia_config}")

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# The package config/version files are shared find_package() glue. Component-scoped
# installs pull Development deliberately; each target export stays with the target's
# own component so a partial prefix never imports libraries that were not installed.
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
    COMPONENT Development
)
