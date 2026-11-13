# Keep useful diagnostics without the noise from partially initialized fixtures.
function(ruvia_apply_test_warnings name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /bigobj /FS)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -Wall)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${name} PRIVATE -Wshadow=local -Wno-psabi)
        endif()
    endif()
endfunction()

function(ruvia_apply_test_support_includes name lib)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE
        RUVIA_TEST_TEMP_ROOT="${PROJECT_SOURCE_DIR}/build/test-tmp")
    if(NOT "${lib}" STREQUAL "ruvia::core" AND NOT "${lib}" STREQUAL "ruvia::http" AND NOT "${lib}" STREQUAL "ruvia::web")
        message(FATAL_ERROR "unknown Ruvia test library for ${name}: ${lib}")
    endif()
endfunction()

function(ruvia_add_test name lib)
    cmake_parse_arguments(test "" "TIMEOUT" "SOURCES" ${ARGN})
    if(NOT test_SOURCES)
        message(FATAL_ERROR "test executable ${name} requires SOURCES")
    endif()
    add_executable(${name} ${test_SOURCES} test_main.cpp)
    target_link_libraries(${name} PRIVATE ${lib} GTest::gtest)
    ruvia_apply_test_support_includes(${name} ${lib})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    if(DEFINED test_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${test_TIMEOUT})
    endif()
endfunction()

function(ruvia_collect_test_sources out)
    set(_patterns "")
    foreach(_pattern IN LISTS ARGN)
        list(APPEND _patterns "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
    endforeach()
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" ${_patterns})
    list(SORT _sources)
    set(${out} ${_sources} PARENT_SCOPE)
endfunction()

# Web functional fixtures exercise native TLS/coding/backend behavior. Declare
# those dependencies here instead of exposing implementation headers and compile
# options through the installed Web target.
function(ruvia_add_web_test name timeout)
    ruvia_add_test(${name} ruvia::web TIMEOUT ${timeout} SOURCES ${ARGN})
    target_link_libraries(${name} PRIVATE ${RUVIA_WEB_DEPENDENCIES} ${RUVIA_HTTP_DEPENDENCIES})
    target_compile_definitions(${name} PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX)
    endif()
endfunction()

# Web suites that need their own process-wide application or peer build into
# separate executables; their sources are excluded from ruvia_web_unit_tests.
function(ruvia_add_web_standalone_test name timeout)
    ruvia_add_web_test(${name} ${timeout} ${ARGN})
    set(_ruvia_web_standalone_sources ${_ruvia_web_standalone_sources} ${ARGN}
        PARENT_SCOPE)
endfunction()

if(RUVIA_BUILD_CORE)
    ruvia_collect_test_sources(_ruvia_core_unit_sources core/*.cpp)
    ruvia_add_test(ruvia_core_unit_tests ruvia::core TIMEOUT 180 SOURCES
        ${_ruvia_core_unit_sources})
    # 作用域任务的所有权转移必须在关闭可选拷贝消除时同样有效。
    set_property(SOURCE core/worker_wait.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-elide-constructors>")
endif()

if(RUVIA_BUILD_HTTP)
    ruvia_collect_test_sources(_ruvia_http_unit_sources http/*.cpp)
    if(NOT RUVIA_ENABLE_HTTP2)
        list(FILTER _ruvia_http_unit_sources EXCLUDE REGEX "^http/http2_")
    endif()
    if(NOT RUVIA_ENABLE_HTTP3)
        list(FILTER _ruvia_http_unit_sources EXCLUDE REGEX "^http/http3_")
    endif()
    list(SORT _ruvia_http_unit_sources)

    ruvia_add_test(ruvia_http_unit_tests ruvia::http TIMEOUT 180 SOURCES
        ${_ruvia_http_unit_sources})
    target_link_libraries(ruvia_http_unit_tests PRIVATE ${RUVIA_HTTP_DEPENDENCIES})
endif()

if(RUVIA_BUILD_WEB)
    # Process-singleton application fixtures run in separate test executables.
    ruvia_add_web_standalone_test(ruvia_connection_lifecycle 240 web/connection_lifecycle.cpp)
    ruvia_add_web_standalone_test(ruvia_connection_close_tests 240 web/http1_connection_close.cpp)
    if(RUVIA_ENABLE_HTTP3)
        ruvia_add_web_standalone_test(ruvia_http3_quic_tests 120 web/http3_quic.cpp)
    endif()

    # Feature directories join the unit suite only when their feature is built.
    ruvia_collect_test_sources(_ruvia_web_unit_sources web/*.cpp)
    list(FILTER _ruvia_web_unit_sources EXCLUDE REGEX "^web/(db_pool|jwt|redis)\\.cpp$")
    if(NOT RUVIA_ENABLE_HTTP2)
        list(FILTER _ruvia_web_unit_sources EXCLUDE REGEX "^web/http2_")
    endif()
    if(NOT RUVIA_ENABLE_HTTP3)
        list(FILTER _ruvia_web_unit_sources EXCLUDE REGEX "^web/http3_")
    endif()
    list(REMOVE_ITEM _ruvia_web_unit_sources ${_ruvia_web_standalone_sources})
    if(RUVIA_ENABLE_JWT)
        list(APPEND _ruvia_web_unit_sources web/jwt.cpp)
    endif()
    if(RUVIA_ENABLE_REDIS)
        list(APPEND _ruvia_web_unit_sources web/redis.cpp)
    endif()
    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        list(APPEND _ruvia_web_unit_sources web/db_pool.cpp)
    endif()
    list(SORT _ruvia_web_unit_sources)

    ruvia_add_web_test(ruvia_web_unit_tests 360 ${_ruvia_web_unit_sources})
endif()
