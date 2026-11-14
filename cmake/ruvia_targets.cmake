# Shared build/install helpers used by every Ruvia component.
# The root lists no per-target build logic of its own. Functions capture
# CMAKE_CURRENT_SOURCE_DIR at call time, so a component invoking these resolves
# paths against its own directory.

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
    set(_ruvia_expected_header_root "include")
    if(NOT relative_path MATCHES "^${_ruvia_expected_header_root}/[^/]+\\.h$")
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

# Install the explicit public header list for a component. Source-only
# implementation headers stay build-private.
function(ruvia_install_public_headers component)
    foreach(_ruvia_header IN LISTS ARGN)
        ruvia_assert_component_header_path(${component} "${_ruvia_header}")
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_ruvia_header}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            COMPONENT ${component}
        )
    endforeach()
endfunction()
