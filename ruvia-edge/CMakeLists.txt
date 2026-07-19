set(RUVIA_EDGE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EdgeConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EdgeFreshness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EdgeCache.cpp
)

add_library(ruvia-edge ${RUVIA_EDGE_SOURCES})
add_library(ruvia::edge ALIAS ruvia-edge)

set_target_properties(ruvia-edge PROPERTIES
    EXPORT_NAME edge
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-edge)
ruvia_configure_runtime_library(ruvia-edge)

target_compile_definitions(ruvia-edge PUBLIC RUVIA_ENABLE_EDGE)

# The edge node is a caching reverse proxy: it reuses the web server (accept
# loop, sessions, TLS) to face clients, the sans-I/O HTTP/1 client in ruvia-http
# to fetch from origins, and the core async/socket primitives to drive that I/O.
target_link_libraries(ruvia-edge
    PUBLIC
        ruvia::core
        ruvia::http
        ruvia::web
)

ruvia_install_target(ruvia-edge edge)
ruvia_install_public_header_tree(edge include/ruvia/edge)
ruvia_install_export(edge)
ruvia_assert_target_file_ownership(ruvia-edge)
