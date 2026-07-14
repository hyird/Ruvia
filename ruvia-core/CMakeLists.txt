add_library(ruvia-core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MimallocOverride.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConnectionScanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskScope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorkerDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorkerHandle.cpp
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
    include/ruvia/core/Channel.h
    include/ruvia/core/OneShot.h
    include/ruvia/core/TaskScope.h
    include/ruvia/core/Timer.h
    include/ruvia/core/Runtime.h
    include/ruvia/core/WorkerHandle.h
    include/ruvia/core/WorkerWaitResult.h
    include/ruvia/core/memory/MemoryPool.h
    include/ruvia/core/memory/PmrObject.h
    include/ruvia/core/memory/PmrResource.h
    include/ruvia/core/memory/ProcessResource.h
)

ruvia_install_public_header_tree(core include/ruvia/core/detail)
