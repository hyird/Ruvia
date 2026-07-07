add_library(ruvia-edge
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Edge.cpp
)
add_library(ruvia::edge ALIAS ruvia-edge)

set_target_properties(ruvia-edge PROPERTIES
    EXPORT_NAME edge
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-edge)

target_link_libraries(ruvia-edge
    PUBLIC
        ruvia::http
)

ruvia_install_target(ruvia-edge edge)
