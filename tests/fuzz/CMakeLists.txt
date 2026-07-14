function(ruvia_add_protocol_fuzzer name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ruvia::http)
    target_compile_options(${name} PRIVATE
        -fsanitize=fuzzer,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    target_link_options(${name} PRIVATE
        -fsanitize=fuzzer,undefined
        -fno-sanitize-recover=all
    )
endfunction()

ruvia_add_protocol_fuzzer(ruvia_fuzz_http1_request http1_request_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http2_connection http2_connection_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_hpack hpack_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_http_chunk http_chunk_fuzz.cpp)
ruvia_add_protocol_fuzzer(ruvia_fuzz_multipart multipart_fuzz.cpp)
