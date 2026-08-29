# Test code gets a warning set chosen for signal, not the library's full one.
# Measured against this suite, -Wextra and -Wpedantic are ~80% noise here: a
# designated initializer that names only the fields under test reads as a
# missing initializer, and the macro-based test registration leaves a stray
# semicolon at every use. Those would have to be silenced case by case, and a
# warning that is routinely silenced stops being read at all.
#
# What is left still catches the failures that matter in test code: an unused
# helper is a check that was written and never run, and a shadowed local is how
# an assertion quietly starts reading the wrong variable.
#
# -Wdangling-reference is excluded deliberately. GCC 13 flags any const-ref
# binding to a function that took a temporary, with no aliasing analysis, so
# `const auto& m = context.req().valid<T>()` trips it even though the reference
# points into the Context and outlives the temporary facade.
function(ruvia_apply_test_warnings name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /bigobj /FS)
        # std::getenv and friends are standard C++; MSVC's C4996 "unsafe" notice
        # on them is the same curated-out noise as -Wextra, not a real finding.
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${name} PRIVATE -Wall)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${name} PRIVATE -Wshadow=local -Wno-dangling-reference)
        endif()
    endif()
endfunction()

function(ruvia_apply_test_support_includes name lib)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if("${lib}" STREQUAL "ruvia::http")
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/http/support)
    elseif("${lib}" STREQUAL "ruvia::web")
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/http/support
            ${CMAKE_CURRENT_SOURCE_DIR}/web/unit/support)
    elseif(NOT "${lib}" STREQUAL "ruvia::core")
        message(FATAL_ERROR "unknown Ruvia test library for ${name}: ${lib}")
    endif()
endfunction()

function(ruvia_expected_test_source_prefix out lib)
    if("${lib}" STREQUAL "ruvia::core")
        set(_prefix "core/")
    elseif("${lib}" STREQUAL "ruvia::http")
        set(_prefix "http/")
    elseif("${lib}" STREQUAL "ruvia::web")
        set(_prefix "web/")
    else()
        message(FATAL_ERROR "unknown Ruvia test library: ${lib}")
    endif()
    set(${out} "${_prefix}" PARENT_SCOPE)
endfunction()

function(ruvia_validate_test_sources name lib)
    ruvia_expected_test_source_prefix(_expected_prefix ${lib})
    foreach(_source IN LISTS ARGN)
        if(IS_ABSOLUTE "${_source}")
            set(_absolute_source "${_source}")
        else()
            get_filename_component(_absolute_source "${_source}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        file(RELATIVE_PATH _relative_source
            "${CMAKE_CURRENT_SOURCE_DIR}" "${_absolute_source}")
        if("${_relative_source}" STREQUAL "support/test_main.cpp")
            continue()
        endif()
        if(NOT _relative_source MATCHES "^${_expected_prefix}")
            message(FATAL_ERROR
                "test target ${name} links ${lib} but uses source "
                "${_relative_source}; expected tests/${_expected_prefix}**")
        endif()
    endforeach()
endfunction()

# One registration helper for every standalone test executable. It requires an
# explicit target library, exposes only the shared support and target-allowed
# support include directories, applies the MSVC large-object flags uniformly,
# registers the ctest, and sets an optional timeout.
function(ruvia_add_test name source)
    cmake_parse_arguments(T "" "LIB;TIMEOUT" "SOURCES" ${ARGN})
    if(NOT DEFINED T_LIB)
        message(FATAL_ERROR "test executable ${name} requires explicit LIB")
    endif()
    set(_sources ${source})
    if(T_SOURCES)
        set(_sources ${T_SOURCES})
    endif()
    ruvia_validate_test_sources(${name} ${T_LIB} ${_sources})
    add_executable(${name} ${_sources})
    target_link_libraries(${name} PRIVATE ${T_LIB})
    ruvia_apply_test_support_includes(${name} ${T_LIB})
    ruvia_apply_test_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set(_props "")
    if(DEFINED T_TIMEOUT)
        list(APPEND _props TIMEOUT ${T_TIMEOUT})
    endif()
    if(_props)
        set_tests_properties(${name} PROPERTIES ${_props})
    endif()
endfunction()

function(ruvia_add_unit_suite name lib)
    ruvia_add_test(${name} "" LIB ${lib} TIMEOUT 180 SOURCES ${ARGN})
endfunction()

function(ruvia_add_unit_sources name lib)
    cmake_parse_arguments(S "" "" "PRIVATE" ${ARGN})
    if(S_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "ruvia_add_unit_sources(${name}) only supports PRIVATE sources")
    endif()
    ruvia_validate_test_sources(${name} ${lib} ${S_PRIVATE})
    target_sources(${name} PRIVATE ${S_PRIVATE})
endfunction()

if(RUVIA_BUILD_CORE)
ruvia_add_unit_suite(ruvia_core_unit_tests ruvia::core
    # core
    core/base64.cpp
    core/pool_waiter_queue.cpp
    core/scoped_operation.cpp
    core/stop_token.cpp

    # support
    support/test_main.cpp
)
endif()

if(RUVIA_BUILD_HTTP)
ruvia_add_unit_suite(ruvia_http_unit_tests ruvia::http
    # http/unit/client
    http/unit/client/origin.cpp
    http/unit/client/redirect.cpp
    http/unit/client/request.cpp
    http/unit/client/response_coding.cpp
    http/unit/client/response_exchange.cpp
    http/unit/client/response_framing.cpp
    http/unit/client/response_parser.cpp
    http/unit/client/set_cookie.cpp

    # http/unit/coding
    http/unit/coding/content_codec.cpp
    http/unit/coding/content_negotiation.cpp
    http/unit/coding/response_content_semantics.cpp

    # http/unit/cookie
    http/unit/cookie/validation.cpp

    # http/unit/field
    http/unit/field/accept_media_type.cpp
    http/unit/field/byte_range.cpp
    http/unit/field/cache.cpp
    http/unit/field/conditional_request.cpp
    http/unit/field/connection_fields.cpp
    http/unit/field/date_cache.cpp
    http/unit/field/date_parsing.cpp
    http/unit/field/expectations.cpp
    http/unit/field/header_tokens.cpp
    http/unit/field/method.cpp
    http/unit/field/quoted_field_values.cpp
    http/unit/field/semicolon_parameters.cpp
    http/unit/field/syntax.cpp
    http/unit/field/value_primitives.cpp

    # http/http1
    http/http1/chunk_decoder.cpp
    http/http1/chunk_framing.cpp
    http/http1/chunk_size.cpp
    http/http1/chunk_trailers.cpp
    http/http1/chunked_framing.cpp
    http/http1/interim_response.cpp
    http/http1/multipart.cpp
    http/http1/multipart_quoting.cpp
    http/http1/parser.cpp
    http/http1/request_body_decoding.cpp
    http/http1/request_target.cpp
    http/http1/transfer_coding_decoding.cpp

    # http/http2/connection
    http/http2/connection/data.cpp
    http/http2/connection/flow.cpp
    http/http2/connection/headers.cpp
    http/http2/connection/public.cpp
    http/http2/connection/session.cpp
    http/http2/connection/streams.cpp
    http/http2/connection/submit.cpp

    # http/http2/flow
    http/http2/flow/control.cpp
    http/http2/flow/ready_queue.cpp
    http/http2/flow/stream_flow.cpp

    # http/http2/frame
    http/http2/frame/codec.cpp
    http/http2/frame/offset_vector.cpp
    http/http2/frame/output_buffer.cpp
    http/http2/frame/payload.cpp
    http/http2/frame/payload_slice.cpp

    # http/http2/hpack
    http/http2/hpack/header_block.cpp
    http/http2/hpack/header_list.cpp
    http/http2/hpack/hpack.cpp
    http/http2/hpack/public.cpp
    http/http2/hpack/static_table.cpp

    # http/http2/message
    http/http2/message/connect.cpp
    http/http2/message/header_rules.cpp
    http/http2/message/local_content_state.cpp
    http/http2/message/remote_content_state.cpp
    http/http2/message/request_builder.cpp
    http/http2/message/request_headers.cpp
    http/http2/message/response_headers.cpp
    http/http2/message/websocket_handshake.cpp

    # http/http2/settings
    http/http2/settings/local_settings.cpp
    http/http2/settings/peer_settings.cpp
    http/http2/settings/settings.cpp

    # http/http2/stream
    http/http2/stream/classify.cpp
    http/http2/stream/closed_streams.cpp
    http/http2/stream/lifecycle.cpp
    http/http2/stream/request_data.cpp
    http/http2/stream/table.cpp

    # http/unit/parser
    http/unit/parser/char_syntax.cpp
    http/unit/parser/error.cpp
    http/unit/parser/header_block_parser.cpp
    http/unit/parser/header_classification.cpp

    # http/unit/request
    http/unit/request/access.cpp

    # http/unit/response
    http/unit/response/body.cpp
    http/unit/response/computed_response_headers.cpp
    http/unit/response/final_response_control.cpp
    http/unit/response/head_buffer.cpp
    http/unit/response/head_emit.cpp
    http/unit/response/head_policy.cpp
    http/unit/response/header_access.cpp
    http/unit/response/header_classification.cpp
    http/unit/response/header_index_cache.cpp
    http/unit/response/response.cpp
    http/unit/response/static_response_headers.cpp
    http/unit/response/trailers.cpp
    http/unit/response/vary_headers.cpp

    # http/unit/util
    http/unit/util/base64.cpp
    http/unit/util/number_format.cpp
    http/unit/util/pmr_string.cpp
    http/unit/util/url_encoding.cpp
    http/unit/util/url_encoding_detect.cpp

    # http/websocket
    http/websocket/accept.cpp
    http/websocket/assembler.cpp
    http/websocket/close.cpp
    http/websocket/deflate.cpp
    http/websocket/frame.cpp
    http/websocket/utils.cpp
    http/websocket/validation.cpp
    http/websocket/ws_connection.cpp

    # support
    support/test_main.cpp
)
endif()

if(RUVIA_BUILD_WEB)
ruvia_add_unit_suite(ruvia_web_unit_tests ruvia::web
    # support
    support/test_main.cpp

    # web/unit/app
    web/unit/app/config_validation.cpp
    web/unit/app/dotenv_parser.cpp
    web/unit/app/named_capability.cpp
    web/unit/app/route_middleware_config.cpp
    web/unit/app/server_options_validation.cpp
    web/unit/app/testing_facade.cpp
    web/unit/app/worker_capabilities.cpp
    web/unit/app/worker_state.cpp

    # web/unit/auth
    web/unit/auth/cookie_signature.cpp

    # web/unit/http
    web/unit/http/conn_info.cpp
    web/unit/http/trusted_proxy.cpp
    web/unit/http/request_deadline.cpp
    web/unit/http/context_capabilities.cpp
    web/unit/http/context_cookie.cpp
    web/unit/http/context_parse_body.cpp
    web/unit/http/context_redirect.cpp
    web/unit/http/context_request_lookups.cpp
    web/unit/http/cors.cpp
    web/unit/http/csrf.cpp
    web/unit/http/error.cpp
    web/unit/http/error_response.cpp
    web/unit/http/health.cpp
    web/unit/http/multipart_reader.cpp
    web/unit/http/request_body_limits.cpp
    web/unit/http/request_body_loader.cpp
    web/unit/http/response_compression.cpp
    web/unit/http/response_stream.cpp
    web/unit/http/scoped_operation_lifetime.cpp
    web/unit/http/security_headers.cpp
    web/unit/http/session.cpp
    web/unit/http/sse_writer.cpp
    web/unit/http/static_file_metadata.cpp
    web/unit/http/static_file_serving.cpp
    web/unit/http/static_files.cpp
    web/unit/http/static_path_normalization.cpp
    web/unit/http/stream_body_reader.cpp

    # web/unit/http2
    web/unit/http2/buffered_response_write.cpp
    web/unit/http2/cleartext_upgrade.cpp
    web/unit/http2/sansio_driver_body.cpp
    web/unit/http2/sansio_driver_session.cpp
    web/unit/http2/sansio_driver_websocket.cpp
    web/unit/http2/sansio_stream_runtime.cpp
    web/unit/http2/sansio_tls.cpp
    web/unit/http2/server_flow_control.cpp

    # web/unit/json
    web/unit/json/escape.cpp
    web/unit/json/json.cpp
    web/unit/json/lex.cpp
    web/unit/json/object_fields.cpp
    web/unit/json/size_hint.cpp

    # web/unit/model
    web/unit/model/content_type_match.cpp
    web/unit/model/email_like.cpp
    web/unit/model/form_parser.cpp
    web/unit/model/json_parser.cpp
    web/unit/model/plain_scalar_fields.cpp
    web/unit/model/json_writer.cpp
    web/unit/model/last_match.cpp
    web/unit/model/boxed_array.cpp
    web/unit/model/pattern_compiler.cpp
    web/unit/model/pattern_matcher.cpp
    web/unit/model/pattern_types.cpp
    web/unit/model/regex_matcher.cpp
    web/unit/model/string.cpp
    web/unit/model/validator.cpp

    # web/unit/ratelimit
    web/unit/ratelimit/rate_limit_decision.cpp
    web/unit/ratelimit/rate_limiter.cpp

    # web/unit/router
    web/unit/router/response_stream_dispatch.cpp
    web/unit/router/route_resolution.cpp
    web/unit/router/routing_dispatch.cpp
    web/unit/router/routing_matching.cpp
    web/unit/router/routing_middleware.cpp

    # web/unit/websocket
    web/unit/websocket/connection.cpp
    web/unit/websocket/handshake.cpp
    web/unit/websocket/heartbeat.cpp
)

# The JWT suite links jwtSign/jwtVerify, which only exist when the optional JWT
# module is compiled in, so gate it on the same flag.
if(RUVIA_ENABLE_JWT)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/auth/jwt.cpp)
endif()

# The Redis RESP-protocol suite links appendRespCommand, gated the same way.
if(RUVIA_ENABLE_REDIS)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/redis/api_surface.cpp
        web/unit/redis/protocol.cpp
    )
endif()

# The backend-neutral API suite is present for either database driver.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/api_surface.cpp
        web/unit/db/migration.cpp)
endif()

# Driver-specific parameter encoding is tested against the same library that
# performs it; neither suite pulls the other backend into a single-driver build.
if(RUVIA_ENABLE_MARIADB)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/sql.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_unit_sources(ruvia_web_unit_tests ruvia::web PRIVATE
        web/unit/db/postgresql.cpp)
endif()

endif()

if(RUVIA_BUILD_CORE)
    ruvia_add_test(ruvia_asio_task core/asio_task.cpp LIB ruvia::core)

    ruvia_validate_test_sources(ruvia_smoke_core_target ruvia::core
        core/smoke_core_target.cpp)
    add_executable(ruvia_smoke_core_target core/smoke_core_target.cpp)
    target_link_libraries(ruvia_smoke_core_target PRIVATE ruvia::core)
    target_compile_definitions(ruvia_smoke_core_target PRIVATE
        RUVIA_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        RUVIA_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        RUVIA_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        RUVIA_EXPECTED_VERSION_STRING="${PROJECT_VERSION}")
    add_test(NAME ruvia_smoke_core_target COMMAND ruvia_smoke_core_target)

    ruvia_add_test(ruvia_connection_scanner_lifetime core/connection_scanner_lifetime.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_event_loop_pool core/runtime_worker.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_operation_deadline core/operation_deadline.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_pool_lease_scheduler core/pool_lease_scheduler.cpp
        LIB ruvia::core TIMEOUT 10)
    ruvia_add_test(ruvia_task_scope core/task_scope.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_task_completion core/task_completion.cpp LIB ruvia::core)

    ruvia_add_test(ruvia_blocking_pool core/blocking_pool.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_channel core/channel.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_one_shot core/one_shot.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_timer core/worker_timer.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_dispatch_failure core/worker_dispatch_failure.cpp LIB ruvia::core)
    ruvia_add_test(ruvia_worker_async_await core/asio_await.cpp LIB ruvia::core)

endif()

if(RUVIA_BUILD_HTTP)
    ruvia_add_test(ruvia_smoke_http_target http/unit/smoke.cpp LIB ruvia::http)
endif()
