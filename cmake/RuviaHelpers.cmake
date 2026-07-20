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
