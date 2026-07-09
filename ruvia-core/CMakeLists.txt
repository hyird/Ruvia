add_library(ruvia-core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory/MemoryPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memory/MimallocOverride.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/ConnectionScanner.cpp
)
add_library(ruvia::core ALIAS ruvia-core)

set_target_properties(ruvia-core PROPERTIES
    EXPORT_NAME core
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-core)
ruvia_configure_runtime_library(ruvia-core)

target_include_directories(ruvia-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_MIMALLOC_TARGET}
        asio::asio
        Threads::Threads
)

ruvia_install_target(ruvia-core core)

ruvia_install_public_headers(core
    include/ruvia/version.h
    include/ruvia/app/Task.h
    include/ruvia/app/detail/TaskAwaiter.h
    include/ruvia/app/detail/TaskPromise.h
    include/ruvia/detail/AsciiCase.h
    include/ruvia/detail/Base64Url.h
    include/ruvia/detail/ConstantTime.h
    include/ruvia/detail/NativePath.h
    include/ruvia/detail/NumberFormat.h
    include/ruvia/memory/MemoryPool.h
    include/ruvia/memory/PmrObject.h
    include/ruvia/memory/PmrResource.h
    include/ruvia/memory/ProcessResource.h
)
