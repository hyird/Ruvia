set(ruvia_core_sources
    src/blocking_pool.cpp
    src/task_scope.cpp
    src/worker_runtime.cpp
)
add_library(ruvia-core STATIC ${ruvia_core_sources})

ruvia_configure_library(ruvia-core core)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_CORE_DEPENDENCIES}
)
if(WIN32)
    target_link_libraries(ruvia-core PRIVATE $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>)
    target_link_libraries(ruvia-core PUBLIC ws2_32 mswsock PRIVATE Advapi32 Bcrypt)
else()
    if(NOT TARGET mimalloc-static)
        message(FATAL_ERROR "Ruvia non-Windows core requires a static mimalloc build with override enabled")
    endif()
    get_target_property(_ruvia_mimalloc_type mimalloc-static TYPE)
    if(NOT _ruvia_mimalloc_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Ruvia non-Windows core requires a real mimalloc-static STATIC_LIBRARY")
    endif()
    if("${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}" MATCHES "-fsanitize=([^ ]*address|[^ ]*thread|[^ ]*memory|[^ ]*hwaddress)")
        target_link_libraries(ruvia-core PRIVATE mimalloc-static)
    else()
        target_link_libraries(ruvia-core PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>")
    endif()
endif()
# Allocation reuse belongs to mimalloc alone: Asio's per-thread handler and
# awaitable-frame caches would be a second cache layer over it. PUBLIC because
# every translation unit that includes Asio must agree.
target_compile_definitions(ruvia-core PUBLIC
    ASIO_STANDALONE
    ASIO_NO_DEPRECATED
    ASIO_DISABLE_SMALL_BLOCK_RECYCLING
    ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING)

ruvia_install_component(ruvia-core core)

ruvia_install_public_headers(core
    include/ruvia/core/blocking_pool.h
    include/ruvia/core/event_loop.h
    include/ruvia/core/stop_token.h
    include/ruvia/core/task.h
    include/ruvia/core/task_scope.h
    include/ruvia/core/worker_ref.h
    include/ruvia/core/worker_wait.h
)

