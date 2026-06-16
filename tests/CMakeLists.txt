find_package(doctest CONFIG REQUIRED)

set(RUVIA_TEST_SOURCES
    test_main.cpp
    test_http_parser.cpp
    test_header_utils.cpp
    test_url_encoding.cpp
    test_file_response.cpp
    test_http_types.cpp
    test_error.cpp
    test_integration.cpp
)

add_executable(ruvia_tests ${RUVIA_TEST_SOURCES})

target_link_libraries(ruvia_tests
    PRIVATE
        ruvia::ruvia
        doctest::doctest
)

# Access internal headers for integration test (HttpServer, RouterInternal)
target_include_directories(ruvia_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(ruvia_tests PRIVATE /W4)
else()
    target_compile_options(ruvia_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# JWT tests need the JWT feature enabled
if(RUVIA_ENABLE_JWT)
    target_sources(ruvia_tests PRIVATE test_jwt.cpp)
endif()

include(${doctest_DIR}/../../../scripts/cmake/doctest.cmake OPTIONAL RESULT_VARIABLE _doctest_cmake)
if(NOT _doctest_cmake)
    include(${doctest_DIR}/../../scripts/cmake/doctest.cmake OPTIONAL)
endif()

add_test(NAME ruvia_unit_tests COMMAND ruvia_tests)
