cmake_minimum_required(VERSION 3.25)
project(ruvia_package_consumer LANGUAGES CXX)

set(RUVIA_PACKAGE_COMPONENTS "" CACHE STRING "Comma-separated Ruvia components to consume")
set(RUVIA_PACKAGE_OPTIONAL_COMPONENTS "" CACHE STRING
    "Comma-separated unavailable optional Ruvia components to probe")
set(RUVIA_EXPECT_VISIBLE_COMPONENTS "" CACHE STRING
    "Comma-separated Ruvia targets expected after find_package")
set(RUVIA_EXPECT_AVAILABLE_COMPONENTS "" CACHE STRING
    "Comma-separated components expected in ruvia_AVAILABLE_COMPONENTS")
set(RUVIA_EXPECT_PACKAGE_VERSION "" CACHE STRING
    "Exact installed Ruvia package version expected by this consumer")
if(RUVIA_PACKAGE_COMPONENTS STREQUAL "")
    message(FATAL_ERROR "RUVIA_PACKAGE_COMPONENTS must name at least one component")
endif()
string(REPLACE "," ";" _ruvia_components "${RUVIA_PACKAGE_COMPONENTS}")
string(REPLACE "," ";" _ruvia_optional_components "${RUVIA_PACKAGE_OPTIONAL_COMPONENTS}")

if(_ruvia_optional_components)
    if(RUVIA_EXPECT_PACKAGE_VERSION STREQUAL "")
        find_package(ruvia CONFIG REQUIRED
            COMPONENTS ${_ruvia_components}
            OPTIONAL_COMPONENTS ${_ruvia_optional_components})
    else()
        find_package(ruvia ${RUVIA_EXPECT_PACKAGE_VERSION} EXACT CONFIG REQUIRED
            COMPONENTS ${_ruvia_components}
            OPTIONAL_COMPONENTS ${_ruvia_optional_components})
    endif()
else()
    if(RUVIA_EXPECT_PACKAGE_VERSION STREQUAL "")
        find_package(ruvia CONFIG REQUIRED COMPONENTS ${_ruvia_components})
    else()
        find_package(ruvia ${RUVIA_EXPECT_PACKAGE_VERSION} EXACT CONFIG REQUIRED
            COMPONENTS ${_ruvia_components})
    endif()
endif()

if(NOT RUVIA_EXPECT_PACKAGE_VERSION STREQUAL "" AND
   NOT ruvia_VERSION STREQUAL RUVIA_EXPECT_PACKAGE_VERSION)
    message(FATAL_ERROR
        "package version mismatch: expected '${RUVIA_EXPECT_PACKAGE_VERSION}', got '${ruvia_VERSION}'")
endif()

foreach(_ruvia_component IN LISTS _ruvia_components)
    if(NOT ruvia_${_ruvia_component}_FOUND)
        message(FATAL_ERROR "required component was not reported found: ${_ruvia_component}")
    endif()
endforeach()
foreach(_ruvia_component IN LISTS _ruvia_optional_components)
    if(ruvia_${_ruvia_component}_FOUND)
        message(FATAL_ERROR "unavailable optional component was reported found: ${_ruvia_component}")
    endif()
endforeach()

if(RUVIA_EXPECT_VISIBLE_COMPONENTS STREQUAL "")
    set(_ruvia_visible_components ${_ruvia_components})
    if("web" IN_LIST _ruvia_visible_components)
        list(APPEND _ruvia_visible_components core http)
    endif()
else()
    string(REPLACE "," ";" _ruvia_visible_components
        "${RUVIA_EXPECT_VISIBLE_COMPONENTS}")
endif()
list(REMOVE_DUPLICATES _ruvia_visible_components)

foreach(_ruvia_component IN ITEMS core http web)
    if(_ruvia_component IN_LIST _ruvia_visible_components)
        if(NOT TARGET ruvia::${_ruvia_component})
            message(FATAL_ERROR "expected imported target is missing: ruvia::${_ruvia_component}")
        endif()
    elseif(TARGET ruvia::${_ruvia_component})
        message(FATAL_ERROR "unrequested imported target leaked into the consumer: ruvia::${_ruvia_component}")
    endif()
endforeach()

if(NOT RUVIA_EXPECT_AVAILABLE_COMPONENTS STREQUAL "")
    string(REPLACE "," ";" _ruvia_expected_available
        "${RUVIA_EXPECT_AVAILABLE_COMPONENTS}")
    set(_ruvia_actual_available ${ruvia_AVAILABLE_COMPONENTS})
    list(SORT _ruvia_expected_available)
    list(SORT _ruvia_actual_available)
    if(NOT _ruvia_actual_available STREQUAL _ruvia_expected_available)
        message(FATAL_ERROR
            "available component mismatch: expected '${_ruvia_expected_available}', got '${_ruvia_actual_available}'")
    endif()
endif()

foreach(_ruvia_component IN LISTS _ruvia_components)
    set(_ruvia_consumer_source ${_ruvia_component}.cpp)
    if(_ruvia_component STREQUAL "http")
        # The exhaustive public-header contract is a regular test target and is
        # compiled once per build. Package scenarios only need to prove that each
        # isolated imported target can compile and link.
        set(_ruvia_consumer_source http_smoke.cpp)
    endif()
    add_executable(ruvia_consumer_${_ruvia_component} ${_ruvia_consumer_source})
    target_link_libraries(ruvia_consumer_${_ruvia_component} PRIVATE ruvia::${_ruvia_component})
    if(_ruvia_component STREQUAL "core" AND
       NOT RUVIA_EXPECT_PACKAGE_VERSION STREQUAL "")
        string(REPLACE "." ";" _ruvia_expected_version_parts
            "${RUVIA_EXPECT_PACKAGE_VERSION}")
        list(GET _ruvia_expected_version_parts 0 _ruvia_expected_version_major)
        list(GET _ruvia_expected_version_parts 1 _ruvia_expected_version_minor)
        list(GET _ruvia_expected_version_parts 2 _ruvia_expected_version_patch)
        target_compile_definitions(ruvia_consumer_core PRIVATE
            RUVIA_EXPECTED_VERSION_MAJOR=${_ruvia_expected_version_major}
            RUVIA_EXPECTED_VERSION_MINOR=${_ruvia_expected_version_minor}
            RUVIA_EXPECTED_VERSION_PATCH=${_ruvia_expected_version_patch}
            RUVIA_EXPECTED_VERSION_STRING="${RUVIA_EXPECT_PACKAGE_VERSION}")
    endif()
endforeach()
