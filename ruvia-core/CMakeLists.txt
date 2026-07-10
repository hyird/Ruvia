add_library(ruvia-core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MimallocOverride.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConnectionScanner.cpp
)
add_library(ruvia::core ALIAS ruvia-core)

set_target_properties(ruvia-core PROPERTIES
    EXPORT_NAME core
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-core)
ruvia_configure_runtime_library(ruvia-core)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_MIMALLOC_TARGET}
        asio::asio
        Threads::Threads
)

ruvia_install_target(ruvia-core core)

ruvia_install_public_headers(core
    include/ruvia/core/version.h
    include/ruvia/core/Task.h
    include/ruvia/core/memory/MemoryPool.h
    include/ruvia/core/memory/PmrObject.h
    include/ruvia/core/memory/PmrResource.h
    include/ruvia/core/memory/ProcessResource.h
)

ruvia_install_public_header_tree(core include/ruvia/core/detail)
