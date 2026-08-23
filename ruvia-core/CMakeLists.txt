find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(_ruvia_core_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
set(_ruvia_core_version_header
    "${_ruvia_core_generated_include_dir}/ruvia/core/version.h")
configure_file(
    include/ruvia/core/version.h.in
    "${_ruvia_core_version_header}"
    @ONLY)

add_library(ruvia-core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BlockingPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConnectionScanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EventLoopPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FailureReport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScopedOperation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TaskScope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorkerDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorkerTimers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WorkerHandle.cpp
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
        $<BUILD_INTERFACE:${_ruvia_core_generated_include_dir}>
)

target_link_libraries(ruvia-core
    PUBLIC
        asio::asio
        Threads::Threads
)

ruvia_install_target(ruvia-core core)

ruvia_install_generated_public_header(
    core
    "${_ruvia_core_version_header}"
    ruvia/core/version.h)

ruvia_install_public_header_closure(core include/ruvia/core)

# Compile-time contracts consumed by ruvia-web's installed template surface.
ruvia_install_public_headers(core
    include/ruvia/core/detail/number/DecimalNumber.h)

if(WIN32)
    target_link_libraries(ruvia-core PUBLIC Mswsock Ws2_32)
endif()

ruvia_install_export(core)
ruvia_assert_target_file_ownership(ruvia-core)
