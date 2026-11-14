set(ruvia_core_sources
    src/asio_ip_address.cpp
    src/blocking_pool.cpp
    src/event_loop.cpp
    src/failure_report.cpp
    src/native_wake_pair.cpp
    src/operation_scope.cpp
    src/pool_lease.cpp
    src/stop_token.cpp
    src/task.cpp
    src/task_scope.cpp
    src/worker_dispatcher.cpp
    src/worker_group.cpp
    src/worker_notification.cpp
    src/worker_runtime.cpp
    src/worker_timer.cpp
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
    # Sanitizers interpose malloc themselves; whole-archive mimalloc would override them, so link it as an ordinary archive.
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
    include/asio_bridge.h
    include/asio_ip_address.h
    include/blocking_pool.h
    include/byte_view.h
    include/channel.h
    include/event_loop.h
    include/failure_report.h
    include/move_only_function.h
    include/one_shot.h
    include/operation_options.h
    include/operation_scope.h
    include/pool_lease.h
    include/stop_token.h
    include/suspend_race.h
    include/task.h
    include/task_scope.h
    include/timeout_duration.h
    include/worker_cancellation.h
    include/worker_notification.h
    include/worker_ref.h
    include/worker_result.h
    include/worker_runtime.h
    include/worker_signal.h
    include/worker_timer.h
    include/worker_wait.h
)
