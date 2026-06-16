add_executable(ruvia_bench main.cpp)

ruvia_configure_library(ruvia_bench)

target_link_libraries(ruvia_bench PRIVATE ruvia::ruvia)

if(MSVC)
    target_compile_options(ruvia_bench PRIVATE /W4 /O2)
else()
    target_compile_options(ruvia_bench PRIVATE -Wall -O3)
endif()
