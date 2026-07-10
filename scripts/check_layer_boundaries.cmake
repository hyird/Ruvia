cmake_minimum_required(VERSION 3.25)

get_filename_component(RUVIA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(RULE_EDGE "ruvia-edge|ruvia::edge|RUVIA_BUILD_EDGE")
set(RULE_ASIO "#[ \t]*include[ \t]*[<\"]asio|asio::")
# The pure sans-I/O protocol library must do NO OS file I/O. The ResponseFileBody
# descriptor (path + size) is fine; opening the file (ifstream / ::open / CreateFile)
# is a web-layer runtime driver concern and must not ship in ruvia-http.
set(RULE_FILE_IO
    "#[ \t]*include[ \t]*[<\"](fstream|fcntl\\.h|unistd\\.h|io\\.h)|ifstream|ofstream|::open[ \t]*\\(|::CreateFile")
set(RULE_HTTP_FRAMEWORK_INCLUDE
    "#[ \t]*include[ \t]*\"ruvia/(app/|core/|memory/|detail/|router/|http/Context\\.(h|inl))")
set(RULE_HTTP_CORE_LINK "ruvia::core|ruvia-core")
set(RULE_CORE_PROTOCOL "ruvia/http/|Http[A-Z]|WebSocket|websocket")
set(RULE_PUBLIC_SRC_INCLUDE "BUILD_INTERFACE:[^>\r\n]*[/\\\\]src")
set(RULE_CROSS_TARGET_SRC "ruvia-(core|http|web)[/\\\\]src")
set(RULE_WEB_CODEC
    "#[ \t]*include[ \t]*[<\"](zlib|zstd|brotli)|deflateInit|inflateInit|Brotli[A-Z]|ZSTD_")
set(RULE_SCANNER_SEMANTICS
    "http|websocket|client_header|client_body|send_timeout|keepalive")
set(RULE_HTTP1_CONNECTION "Http1Connection")
set(RULE_DELETED_H2_SESSION "Http2ServerSession")
set(RULE_STALE_DEPENDENCY
    "ruvia-web[ \t]*->[ \t]*ruvia-http[ \t]*->[ \t]*ruvia-core|http[ \t]*->[ \t]*core|http[^\r\n]*(asio/TLS|socket/TLS)[ \t]*runtime driver")

function(expect_match label regex sample)
    string(REGEX MATCH "${regex}" detected "${sample}")
    if(detected STREQUAL "")
        message(FATAL_ERROR
            "boundary self-test FAIL: ${label} was not detected\n"
            "sample: ${sample}\nregex: ${regex}")
    endif()
endfunction()

function(example_has_private_include content output)
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" includes "${content}")
    set(found FALSE)
    foreach(include_line IN LISTS includes)
        if(NOT include_line MATCHES "#[ \t]*include[ \t]*\"ruvia/")
            set(found TRUE)
            break()
        endif()
    endforeach()
    set(${output} ${found} PARENT_SCOPE)
endfunction()

if(RUVIA_BOUNDARY_SELF_TEST)
    expect_match("removed edge target" "${RULE_EDGE}" "add_subdirectory(ruvia-edge)")
    expect_match("asio in HTTP" "${RULE_ASIO}" "#include <asio.hpp>")
    expect_match("file I/O in HTTP" "${RULE_FILE_IO}" "#include <fstream>")
    expect_match("native open in HTTP" "${RULE_FILE_IO}" "::open(path, O_RDONLY)")
    expect_match("framework include in HTTP" "${RULE_HTTP_FRAMEWORK_INCLUDE}"
        "#include \"ruvia/core/detail/AsioAwait.h\"")
    expect_match("core link in HTTP" "${RULE_HTTP_CORE_LINK}"
        "target_link_libraries(ruvia-http PRIVATE ruvia::core)")
    expect_match("protocol semantics in core" "${RULE_CORE_PROTOCOL}"
        "#include \"ruvia/http/HttpParser.h\"")
    expect_match("public src include" "${RULE_PUBLIC_SRC_INCLUDE}"
        "$<BUILD_INTERFACE:C:/repo/ruvia-http/src>")
    expect_match("cross-target private source include" "${RULE_CROSS_TARGET_SRC}"
        "target_include_directories(ruvia-web PRIVATE C:/repo/ruvia-http/src)")
    expect_match("codec in web" "${RULE_WEB_CODEC}" "#include <zlib.h>")
    expect_match("protocol semantics in core scanner" "${RULE_SCANNER_SEMANTICS}"
        "client_header_timeout")
    expect_match("parallel HTTP/1 state machine" "${RULE_HTTP1_CONNECTION}"
        "class Http1Connection {};")
    expect_match("deleted HTTP/2 session in docs" "${RULE_DELETED_H2_SESSION}"
        "Http2ServerSession owns the socket")
    expect_match("stale dependency description" "${RULE_STALE_DEPENDENCY}"
        "ruvia-web -> ruvia-http -> ruvia-core")
    example_has_private_include("#include \"HttpServerInternal.h\"" private_example)
    if(NOT private_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: private example include was not detected")
    endif()
    example_has_private_include("#include \"ruvia/http/Context.h\"" public_example)
    if(public_example)
        message(FATAL_ERROR
            "boundary self-test FAIL: public example include was rejected")
    endif()
    message(STATUS "boundary self-test OK (all negative rules detect planted violations)")
    return()
endif()

set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED FALSE)

function(boundary_error label details)
    message(STATUS "boundary-check FAIL: ${label}\n    ${details}")
    set_property(GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED TRUE)
endfunction()

function(check_files_no_match label regex)
    set(hit_files)
    foreach(path IN LISTS ARGN)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

function(check_files_no_lower_match label regex)
    set(hit_files)
    foreach(path IN LISTS ARGN)
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(READ "${path}" content)
            string(TOLOWER "${content}" content)
            string(REGEX MATCH "${regex}" detected "${content}")
            if(NOT detected STREQUAL "")
                file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
                list(APPEND hit_files "${relative}: ${detected}")
            endif()
        endif()
    endforeach()
    if(hit_files)
        list(JOIN hit_files "\n    " details)
        boundary_error("${label}" "${details}")
    endif()
endfunction()

foreach(required_dir IN ITEMS ruvia-core ruvia-http ruvia-web)
    if(NOT IS_DIRECTORY "${RUVIA_ROOT}/${required_dir}")
        boundary_error("missing target directory"
            "${required_dir}/ is required; otherwise checks would be vacuous")
    endif()
endforeach()
foreach(required_doc IN ITEMS README.md AGENTS.md)
    if(NOT EXISTS "${RUVIA_ROOT}/${required_doc}")
        boundary_error("missing project boundary document"
            "${required_doc} is required; otherwise checks would be vacuous")
    endif()
endforeach()
if(IS_DIRECTORY "${RUVIA_ROOT}/ruvia-edge")
    boundary_error("ruvia-edge must remain fully removed" "ruvia-edge/ exists")
endif()

file(GLOB_RECURSE HTTP_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/*.h"
    "${RUVIA_ROOT}/ruvia-http/*.cpp"
    "${RUVIA_ROOT}/ruvia-http/*.inl")
file(GLOB_RECURSE WEB_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/*.h"
    "${RUVIA_ROOT}/ruvia-web/*.cpp"
    "${RUVIA_ROOT}/ruvia-web/*.inl")
file(GLOB_RECURSE CORE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h"
    "${RUVIA_ROOT}/ruvia-core/*.cpp"
    "${RUVIA_ROOT}/ruvia-core/*.inl")
file(GLOB_RECURSE EDGE_REFERENCE_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/*.h" "${RUVIA_ROOT}/ruvia-core/*.cpp" "${RUVIA_ROOT}/ruvia-core/*.inl"
    "${RUVIA_ROOT}/ruvia-core/*.cmake" "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/*.h" "${RUVIA_ROOT}/ruvia-http/*.cpp" "${RUVIA_ROOT}/ruvia-http/*.inl"
    "${RUVIA_ROOT}/ruvia-http/*.cmake" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/*.h" "${RUVIA_ROOT}/ruvia-web/*.cpp" "${RUVIA_ROOT}/ruvia-web/*.inl"
    "${RUVIA_ROOT}/ruvia-web/*.cmake" "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/*.h" "${RUVIA_ROOT}/examples/*.cpp" "${RUVIA_ROOT}/examples/*.inl"
    "${RUVIA_ROOT}/examples/*.cmake" "${RUVIA_ROOT}/examples/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl"
    "${RUVIA_ROOT}/tests/*.cmake" "${RUVIA_ROOT}/tests/CMakeLists.txt")
list(APPEND EDGE_REFERENCE_SOURCE
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/README.md"
    "${RUVIA_ROOT}/AGENTS.md")

check_files_no_match("removed edge target is still referenced" "${RULE_EDGE}"
    ${EDGE_REFERENCE_SOURCE})
check_files_no_match("ruvia-http must not reference asio" "${RULE_ASIO}"
    ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not perform OS file I/O (sans-I/O protocol lib)"
    "${RULE_FILE_IO}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not include core/web headers"
    "${RULE_HTTP_FRAMEWORK_INCLUDE}" ${HTTP_SOURCE})
check_files_no_match("ruvia-http must not link/name ruvia-core in CMake"
    "${RULE_HTTP_CORE_LINK}" "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt")
check_files_no_match("ruvia-core must not contain HTTP/WebSocket semantics"
    "${RULE_CORE_PROTOCOL}" ${CORE_SOURCE})
check_files_no_match("target src directories must not be PUBLIC/INTERFACE includes"
    "${RULE_PUBLIC_SRC_INCLUDE}"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt")
check_files_no_match("targets must not include another target's private src tree"
    "${RULE_CROSS_TARGET_SRC}"
    "${RUVIA_ROOT}/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-core/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt")
check_files_no_match("ruvia-web must not implement content/transfer coding"
    "${RULE_WEB_CODEC}" ${WEB_SOURCE})
check_files_no_lower_match("core connection scanner contains protocol/product semantics"
    "${RULE_SCANNER_SEMANTICS}"
    "${RUVIA_ROOT}/ruvia-core/include/ruvia/core/detail/ConnectionScanner.h"
    "${RUVIA_ROOT}/ruvia-core/src/ConnectionScanner.cpp")

# Cross-target contracts live in include/.../detail. Source trees contain
# implementations only (plus each target's own PCH), so another target cannot
# silently become a friend by reaching into private implementation headers.
file(GLOB_RECURSE HTTP_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-http/src/*.h"
    "${RUVIA_ROOT}/ruvia-http/src/*.inl")
if(HTTP_PRIVATE_HEADERS)
    list(JOIN HTTP_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-http/src contains private headers" "${details}")
endif()
file(GLOB_RECURSE CORE_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-core/src/*.h"
    "${RUVIA_ROOT}/ruvia-core/src/*.inl")
list(FILTER CORE_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(CORE_PRIVATE_HEADERS)
    list(JOIN CORE_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-core/src contains cross-target contract headers" "${details}")
endif()
file(GLOB_RECURSE WEB_PRIVATE_HEADERS LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/ruvia-web/src/*.h"
    "${RUVIA_ROOT}/ruvia-web/src/*.inl")
list(FILTER WEB_PRIVATE_HEADERS EXCLUDE REGEX "[/\\\\]pch\\.h$")
if(WEB_PRIVATE_HEADERS)
    list(JOIN WEB_PRIVATE_HEADERS "\n    " details)
    boundary_error("ruvia-web/src contains contract headers" "${details}")
endif()
foreach(stale_dir IN ITEMS
    "${RUVIA_ROOT}/ruvia-core/src/memory"
    "${RUVIA_ROOT}/ruvia-core/src/net"
    "${RUVIA_ROOT}/ruvia-core/src/runtime"
    "${RUVIA_ROOT}/ruvia-http/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/net"
    "${RUVIA_ROOT}/ruvia-web/src/db/core"
    "${RUVIA_ROOT}/ruvia-web/src/redis/core"
    "${RUVIA_ROOT}/ruvia-web/src/router/core")
    if(IS_DIRECTORY "${stale_dir}")
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${stale_dir}")
        boundary_error("redundant source directory layer was reintroduced" "${relative}")
    endif()
endforeach()

file(GLOB_RECURSE EXAMPLE_SOURCE LIST_DIRECTORIES FALSE "${RUVIA_ROOT}/examples/*.cpp")
set(private_examples)
foreach(path IN LISTS EXAMPLE_SOURCE)
    file(READ "${path}" content)
    example_has_private_include("${content}" has_private)
    if(has_private)
        file(RELATIVE_PATH relative "${RUVIA_ROOT}" "${path}")
        list(APPEND private_examples "${relative}")
    endif()
endforeach()
if(private_examples)
    list(JOIN private_examples "\n    " details)
    boundary_error("examples include target-private headers" "${details}")
endif()

set(WS_RUNTIME_HEADER "${RUVIA_ROOT}/ruvia-web/src/websocket/HttpWebSocketConnection.h")
if(EXISTS "${WS_RUNTIME_HEADER}")
    file(READ "${WS_RUNTIME_HEADER}" ws_runtime)
    if(NOT ws_runtime MATCHES "WsConnection")
        boundary_error("ruvia-web WebSocket runtime does not drive WsConnection"
            "missing WsConnection in ruvia-web/src/websocket/HttpWebSocketConnection.h")
    endif()
endif()

set(HTTP1_SCAN ${HTTP_SOURCE} ${WEB_SOURCE} ${EXAMPLE_SOURCE})
file(GLOB_RECURSE TEST_SOURCE LIST_DIRECTORIES FALSE
    "${RUVIA_ROOT}/tests/*.h" "${RUVIA_ROOT}/tests/*.cpp" "${RUVIA_ROOT}/tests/*.inl")
list(APPEND HTTP1_SCAN ${TEST_SOURCE}
    "${RUVIA_ROOT}/ruvia-http/CMakeLists.txt"
    "${RUVIA_ROOT}/ruvia-web/CMakeLists.txt"
    "${RUVIA_ROOT}/tests/CMakeLists.txt"
    "${RUVIA_ROOT}/examples/CMakeLists.txt")
check_files_no_match("parallel Http1Connection state machine was reintroduced"
    "${RULE_HTTP1_CONNECTION}" ${HTTP1_SCAN})

set(HTTP_RUNTIME_STATE "${RUVIA_ROOT}/ruvia-web/src/server/HttpConnectionState.h")
set(HTTP_BODY_READER "${RUVIA_ROOT}/ruvia-web/src/body/HttpStreamBodyReader.h")
if(EXISTS "${HTTP_RUNTIME_STATE}" AND EXISTS "${HTTP_BODY_READER}")
    file(READ "${HTTP_RUNTIME_STATE}" runtime_state)
    file(READ "${HTTP_BODY_READER}" body_reader)
    if(NOT runtime_state MATCHES "HttpServerParser" OR
       NOT body_reader MATCHES "HttpChunkDecoder")
        boundary_error("ruvia-web HTTP/1 runtime is not driving http-owned primitives"
            "expected HttpServerParser and HttpChunkDecoder in web runtime state")
    endif()
endif()

set(BOUNDARY_DOCS "${RUVIA_ROOT}/README.md" "${RUVIA_ROOT}/AGENTS.md")
check_files_no_match("docs reference the deleted coroutine h2 server session"
    "${RULE_DELETED_H2_SESSION}" ${BOUNDARY_DOCS})
check_files_no_match("docs contain stale dependency/runtime ownership"
    "${RULE_STALE_DEPENDENCY}" ${BOUNDARY_DOCS})

get_property(boundary_failed GLOBAL PROPERTY RUVIA_BOUNDARY_FAILED)
if(boundary_failed)
    message(FATAL_ERROR "Ruvia layer-boundary checks failed")
endif()
message(STATUS "layer boundaries OK")
