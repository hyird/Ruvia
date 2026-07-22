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

if(NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
    set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
endif()
if(NOT "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" STREQUAL "")
    set(VCPKG_HOST_TRIPLET "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" CACHE STRING "")
endif()

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

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Ruvia Windows builds require MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
# Shared build/install helpers used by every Ruvia component (core/http/web).
# The root lists no per-target build logic of its own; it only includes this
# module before add_subdirectory() so each component's CMakeLists can configure,
# install, and export itself. Functions capture CMAKE_CURRENT_SOURCE_DIR at call
# time, so a component invoking these resolves paths against its own directory.

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

function(ruvia_configure_library target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_definitions(${target} PUBLIC _WIN32_WINNT=0x0A00)
        target_compile_options(${target}
            PUBLIC /utf-8 /Zc:preprocessor
            PRIVATE /W4 /permissive- /bigobj /FS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_options(${target}
        INTERFACE
            $<$<PLATFORM_ID:Darwin>:LINKER:-no_warn_duplicate_libraries>
    )
endfunction()

function(ruvia_configure_runtime_library target)
    target_compile_definitions(${target}
        PUBLIC
            ASIO_STANDALONE
    )
    target_precompile_headers(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")
endfunction()

function(ruvia_install_target target component)
    install(
        TARGETS ${target}
        EXPORT ruvia_${component}_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component} NAMELINK_COMPONENT ${component}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${component}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endfunction()

# Install the export set for a component. Each component calls this itself so its
# targets file lives with the component that produced it; a partial prefix never
# imports libraries that were not installed.
function(ruvia_install_export component)
    install(
        EXPORT ruvia_${component}_targets
        FILE ruvia-${component}-targets.cmake
        NAMESPACE ruvia::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
        COMPONENT ${component}
    )
endfunction()

function(ruvia_assert_component_header_path component relative_path)
    set(_ruvia_expected_header_root "include/ruvia/${component}")
    if(NOT relative_path MATCHES "^${_ruvia_expected_header_root}(/|$)")
        message(FATAL_ERROR
            "${component} cannot install a header outside ${_ruvia_expected_header_root}: ${relative_path}")
    endif()

    set(_ruvia_header_root "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_expected_header_root}")
    set(_ruvia_header_path "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
    cmake_path(NORMAL_PATH _ruvia_header_root OUTPUT_VARIABLE _ruvia_header_root_normalized)
    cmake_path(IS_PREFIX _ruvia_header_root_normalized
        "${_ruvia_header_path}" NORMALIZE _ruvia_header_is_owned)
    if(NOT _ruvia_header_is_owned)
        message(FATAL_ERROR
            "${component} header path escapes its target directory: ${relative_path}")
    endif()
endfunction()

# Every installed header is recorded as "<path under include/>|<file it comes
# from>". ruvia_guard_install_closure reads the record to prove that an
# installed header never includes one that stayed behind.
function(ruvia_record_installed_header install_relative source_file)
    set_property(GLOBAL APPEND PROPERTY
        RUVIA_INSTALLED_HEADERS "${install_relative}|${source_file}")
endfunction()

function(ruvia_install_public_headers component)
    foreach(_ruvia_header ${ARGN})
        ruvia_assert_component_header_path(${component} "${_ruvia_header}")
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
        get_filename_component(_ruvia_header_name "${_ruvia_header}" NAME)
        ruvia_record_installed_header(
            "${_ruvia_header_install_subdir}/${_ruvia_header_name}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}")
    endforeach()
endfunction()

function(ruvia_install_public_header_tree component relative_dir)
    ruvia_assert_component_header_path(${component} "${relative_dir}")
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
            ${ARGN}
    )

    # Mirror the same selection into the record. ARGN carries "PATTERN <p>
    # EXCLUDE" triples, and install(DIRECTORY) matches a PATTERN against one
    # path component, so a match anywhere along the path drops the file.
    set(_ruvia_tree_excludes "")
    set(_ruvia_expect_pattern FALSE)
    foreach(_ruvia_tree_argument IN LISTS ARGN)
        if(_ruvia_expect_pattern)
            list(APPEND _ruvia_tree_excludes "${_ruvia_tree_argument}")
            set(_ruvia_expect_pattern FALSE)
        elseif(_ruvia_tree_argument STREQUAL "PATTERN")
            set(_ruvia_expect_pattern TRUE)
        endif()
    endforeach()

    file(GLOB_RECURSE _ruvia_tree_headers
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/*.inl")
    foreach(_ruvia_tree_header IN LISTS _ruvia_tree_headers)
        set(_ruvia_tree_header_excluded FALSE)
        string(REPLACE "/" ";" _ruvia_tree_components "${_ruvia_tree_header}")
        foreach(_ruvia_tree_exclude IN LISTS _ruvia_tree_excludes)
            foreach(_ruvia_tree_component IN LISTS _ruvia_tree_components)
                if(_ruvia_tree_component STREQUAL _ruvia_tree_exclude)
                    set(_ruvia_tree_header_excluded TRUE)
                endif()
            endforeach()
        endforeach()
        if(NOT _ruvia_tree_header_excluded)
            ruvia_record_installed_header(
                "${_ruvia_header_tree_install_dir}/${_ruvia_tree_header}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/${_ruvia_tree_header}")
        endif()
    endforeach()
endfunction()

function(ruvia_install_generated_public_header component generated_header relative_path)
    set(_ruvia_expected_header_root "ruvia/${component}")
    if(NOT relative_path MATCHES "^${_ruvia_expected_header_root}(/|$)")
        message(FATAL_ERROR
            "${component} cannot install a generated header outside ${_ruvia_expected_header_root}: ${relative_path}")
    endif()

    set(_ruvia_target_binary_dir "${CMAKE_CURRENT_BINARY_DIR}")
    cmake_path(NORMAL_PATH _ruvia_target_binary_dir
        OUTPUT_VARIABLE _ruvia_target_binary_root)
    cmake_path(IS_PREFIX _ruvia_target_binary_root
        "${generated_header}" NORMALIZE _ruvia_generated_header_is_owned)
    if(NOT _ruvia_generated_header_is_owned)
        message(FATAL_ERROR
            "${component} generated header is outside its target build directory: ${generated_header}")
    endif()

    get_filename_component(_ruvia_header_install_subdir "${relative_path}" DIRECTORY)
    install(
        FILES "${generated_header}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_ruvia_header_install_subdir}"
        COMPONENT ${component}
    )
    ruvia_record_installed_header("${relative_path}" "${generated_header}")
endfunction()

function(ruvia_assert_target_file_ownership target)
    get_target_property(_ruvia_target_source_dir ${target} SOURCE_DIR)
    cmake_path(NORMAL_PATH _ruvia_target_source_dir OUTPUT_VARIABLE _ruvia_target_root)
    get_target_property(_ruvia_target_binary_dir ${target} BINARY_DIR)
    cmake_path(NORMAL_PATH _ruvia_target_binary_dir
        OUTPUT_VARIABLE _ruvia_target_binary_root)

    get_target_property(_ruvia_target_sources ${target} SOURCES)
    foreach(_ruvia_source IN LISTS _ruvia_target_sources)
        if(_ruvia_source MATCHES "^\\$<")
            continue()
        endif()
        if(IS_ABSOLUTE "${_ruvia_source}")
            set(_ruvia_source_path "${_ruvia_source}")
        else()
            set(_ruvia_source_path "${_ruvia_target_source_dir}/${_ruvia_source}")
        endif()
        cmake_path(IS_PREFIX _ruvia_target_root
            "${_ruvia_source_path}" NORMALIZE _ruvia_source_is_owned)
        if(NOT _ruvia_source_is_owned)
            message(FATAL_ERROR
                "${target} cannot compile another target's source: ${_ruvia_source}")
        endif()
    endforeach()

    get_target_property(_ruvia_target_include_dirs ${target} INCLUDE_DIRECTORIES)
    foreach(_ruvia_include_dir IN LISTS _ruvia_target_include_dirs)
        if(_ruvia_include_dir MATCHES "^\\$<INSTALL_INTERFACE:")
            continue()
        elseif(_ruvia_include_dir MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
            set(_ruvia_include_path "${CMAKE_MATCH_1}")
        elseif(_ruvia_include_dir MATCHES "^\\$<")
            continue()
        else()
            set(_ruvia_include_path "${_ruvia_include_dir}")
        endif()
        cmake_path(IS_PREFIX _ruvia_target_root
            "${_ruvia_include_path}" NORMALIZE _ruvia_include_is_owned)
        cmake_path(IS_PREFIX _ruvia_target_binary_root
            "${_ruvia_include_path}" NORMALIZE _ruvia_generated_include_is_owned)
        if(NOT _ruvia_include_is_owned AND NOT _ruvia_generated_include_is_owned)
            message(FATAL_ERROR
                "${target} cannot add another target's private include directory: ${_ruvia_include_dir}")
        endif()
    endforeach()
endfunction()


foreach(_ruvia_component IN ITEMS core http web edge)
    string(TOUPPER ${_ruvia_component} _ruvia_component_upper)
    if(RUVIA_BUILD_${_ruvia_component_upper})
        add_subdirectory(ruvia-${_ruvia_component})
    endif()
endforeach()

if(RUVIA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
# Written after every target has declared its installs, and read by
# ruvia_guard_install_closure at test time.
get_property(_ruvia_installed_headers GLOBAL PROPERTY RUVIA_INSTALLED_HEADERS)
list(SORT _ruvia_installed_headers)
list(JOIN _ruvia_installed_headers "\n" _ruvia_installed_headers_text)
file(WRITE "${CMAKE_BINARY_DIR}/ruvia_installed_headers.txt"
    "${_ruvia_installed_headers_text}\n")

if(RUVIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(RUVIA_BUILD_BENCHMARKS)
    add_subdirectory(tests/http/benchmarks)
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
