add_executable(ruvia_unit_tests
    test_main.cpp
    unit_encoding.cpp
    unit_json.cpp
    unit_cookie_signature.cpp
    unit_cookie_validation.cpp
    unit_http_parsing.cpp
    unit_multipart.cpp
    unit_content_negotiation.cpp
    unit_rate_limiter.cpp
    unit_chunk_size.cpp
    unit_conditional_request.cpp
    unit_websocket_deflate.cpp
    unit_request_body_decoding.cpp
    unit_request_target.cpp
    unit_websocket_accept.cpp
    unit_websocket_validation.cpp
    unit_http_client_response.cpp
    unit_http_client_fetch.cpp
    unit_http2_client.cpp
    unit_routing.cpp
    unit_cors.cpp
)

target_link_libraries(ruvia_unit_tests PRIVATE ruvia::ruvia)

# Unit tests reach into internal detail headers under src/ (e.g. core/Base64.h,
# http/CookieSignature.h) in addition to the public include/ surface.
target_include_directories(ruvia_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(ruvia_unit_tests PRIVATE /bigobj /FS)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_unit_tests)
