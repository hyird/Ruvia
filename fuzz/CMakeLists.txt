# Reproducible smoke-fuzzers for the untrusted-input parsers. Each is a plain
# executable that drives a parser with a deterministic stream of adversarial byte
# strings, so it runs in CI without libFuzzer. Configure the build with a sanitizer
# (e.g. -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all") to turn
# the runs into UB/OOB detection. Iteration count is argv[1] for deeper local runs.
#
# The fuzzers reach into internal detail headers under src/ (parser, http2, ws) in
# addition to the public include/ surface, mirroring the unit tests.
set(_ruvia_fuzzers
    fuzz_parsers
    fuzz_request
    fuzz_hpack_ws
)

foreach(_fuzzer IN LISTS _ruvia_fuzzers)
    add_executable(${_fuzzer} ${_fuzzer}.cpp)
    target_link_libraries(${_fuzzer} PRIVATE ruvia::ruvia)
    target_include_directories(${_fuzzer} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    # A short smoke run wired into ctest; pass a larger argv[1] for deep fuzzing.
    add_test(NAME ${_fuzzer} COMMAND ${_fuzzer} 50000)
endforeach()
