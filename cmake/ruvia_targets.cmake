# Shared build/install helpers used by every Ruvia component.
# The root lists no per-target build logic of its own. Functions capture
# CMAKE_CURRENT_SOURCE_DIR at call time, so a component invoking these resolves
# paths against its own directory.

# Protocol directories and prefixed headers share one feature-ownership rule.
set(_ruvia_disabled_protocol_patterns)
if(NOT RUVIA_ENABLE_HTTP2)
    list(APPEND _ruvia_disabled_protocol_patterns "/(http2|hpack)(/|_|\\.)")
endif()
if(NOT RUVIA_ENABLE_HTTP3)
    list(APPEND _ruvia_disabled_protocol_patterns "/http3(/|_|\\.)")
endif()
list(JOIN _ruvia_disabled_protocol_patterns "|" _ruvia_disabled_protocol_regex)

function(ruvia_collect_cpp_sources out relative_dir exclude_regex)
    file(GLOB_RECURSE _ruvia_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/*.cpp")
    if(exclude_regex)
        list(FILTER _ruvia_sources EXCLUDE REGEX "${exclude_regex}")
    endif()
    list(SORT _ruvia_sources)
    set(${out} ${_ruvia_sources} PARENT_SCOPE)
endfunction()

# Every component compiles its sources against its own src/pch.h.
function(ruvia_configure_library target component)
    add_library(ruvia::${component} ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${component})
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_precompile_headers(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")

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
        # Reject local/parameter shadowing without rejecting member names.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wshadow=local)
            # GCC prints a psABI note when a function returns std::variant of
            # long double. The note is not a defect in that return and cannot
            # be silenced with a diagnostic pragma.
            target_compile_options(${target} PRIVATE -Wno-psabi)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Clang's -Wshadow and -Wshadow-uncaptured-local both diagnose
            # member shadowing in the same group as locals; neither offers
            # GCC's local-only coverage. Musl CI therefore does not enforce it.
        endif()
    endif()
endfunction()

function(ruvia_install_component target component)
    install(
        TARGETS ${target}
        EXPORT ruvia_${component}_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${component} NAMELINK_COMPONENT ${component}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${component}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
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

# Install the public header surface plus exactly the same-component detail
# headers that surface includes transitively. Source-only implementation headers
# stay build-private without requiring a second hand-maintained header list.
function(ruvia_install_public_header_closure component relative_dir exclude_regex)
    file(GLOB_RECURSE _ruvia_closure_candidates CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}/*.inl")

    list(FILTER _ruvia_closure_candidates EXCLUDE REGEX "(^|/)detail(/|$)")
    set(_ruvia_closure_queue ${_ruvia_closure_candidates})

    set(_ruvia_closure_headers)
    while(_ruvia_closure_queue)
        list(POP_FRONT _ruvia_closure_queue _ruvia_header)
        if(exclude_regex AND _ruvia_header MATCHES "${exclude_regex}")
            continue()
        endif()
        if(_ruvia_header IN_LIST _ruvia_closure_headers)
            continue()
        endif()
        list(APPEND _ruvia_closure_headers "${_ruvia_header}")
        # The GLOB only tracks added and removed files; the closure also depends
        # on each member's include lines, so editing one must re-run the scan.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}")

        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}"
            _ruvia_header_text)
        string(REGEX MATCHALL
            "#[ \t]*include[ \t]*[<\"]ruvia/${component}/[^>\"]+[>\"]"
            _ruvia_header_includes "${_ruvia_header_text}")
        foreach(_ruvia_header_include IN LISTS _ruvia_header_includes)
            string(REGEX REPLACE
                ".*[<\"](ruvia/${component}/[^>\"]+)[>\"].*" "\\1"
                _ruvia_included_path "${_ruvia_header_include}")
            set(_ruvia_included_header "include/${_ruvia_included_path}")
            if(NOT EXISTS
               "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_included_header}")
                continue()
            endif()

            list(APPEND _ruvia_closure_queue "${_ruvia_included_header}")
        endforeach()
    endwhile()

    list(SORT _ruvia_closure_headers)
    foreach(_ruvia_header IN LISTS _ruvia_closure_headers)
        ruvia_assert_component_header_path(${component} "${_ruvia_header}")
        cmake_path(GET _ruvia_header PARENT_PATH _ruvia_header_dir)
        file(RELATIVE_PATH _ruvia_header_install_subdir
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header_dir}")
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_ruvia_header_install_subdir}"
            COMPONENT ${component}
        )
    endforeach()
endfunction()
