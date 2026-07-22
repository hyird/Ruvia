add_executable(ruvia_benchmark_http_hot_paths
    hot_paths.cpp
)
target_link_libraries(ruvia_benchmark_http_hot_paths PRIVATE ruvia::http)
target_compile_definitions(ruvia_benchmark_http_hot_paths PRIVATE
    RUVIA_BENCHMARK_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    RUVIA_BENCHMARK_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    RUVIA_BENCHMARK_CONFIGURATION="$<CONFIG>"
    RUVIA_BENCHMARK_PROCESSOR="${CMAKE_SYSTEM_PROCESSOR}"
)

if(MSVC)
    target_compile_options(ruvia_benchmark_http_hot_paths PRIVATE
        /W4 /permissive- /bigobj /FS)
else()
    target_compile_options(ruvia_benchmark_http_hot_paths PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

# Benchmarks are deliberately not CTest tests: correctness gates must be stable
# across hosts, while timing data is recorded and compared per runner/toolchain.
add_custom_target(ruvia_benchmarks
    DEPENDS ruvia_benchmark_http_hot_paths
)
