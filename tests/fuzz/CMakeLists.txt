# AddressSanitizer is part of the fuzzing flags: fuzzing without it is blind to
# the heap-buffer-overflow / use-after-free class it is best at surfacing. For the
# library under test to be instrumented too (not just the harness), configure the
# build with -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined so ruvia::http carries
# the same instrumentation -- see the ubuntu_protocol_sanitizers CI job.
function(ruvia_add_protocol_fuzzer name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ruvia::http)
    target_compile_options(${name} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    target_link_options(${name} PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all
    )
endfunction()

ruvia_add_protocol_fuzzer(ruvia_fuzz_http1_request http1_request_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http1_request_roundtrip http1_request_roundtrip_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http1_client_response http1_client_response_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http2_connection http2_connection_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_hpack hpack_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_hpack_roundtrip hpack_roundtrip_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http_chunk http_chunk_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_multipart multipart_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_websocket_frame websocket_frame_fuzz.cpp)

# The JSON machinery lives in header-only ruvia-web detail headers, present in
# the source tree regardless of RUVIA_BUILD_WEB. It needs only include paths --
# no compiled ruvia-web/ruvia-core objects -- so reuse the protocol fuzzer setup
# (which links ruvia::http) and add the ruvia-web and ruvia-core header roots.
ruvia_add_protocol_fuzzer(ruvia_fuzz_json json_fuzz.cpp)
target_include_directories(ruvia_fuzz_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ruvia-web/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ruvia-core/include)
