find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(mimalloc CONFIG REQUIRED)

set(RUVIA_MIMALLOC_TARGET mimalloc)
if(TARGET mimalloc-static)
    get_target_property(_ruvia_mimalloc_static_type mimalloc-static TYPE)
    if(NOT _ruvia_mimalloc_static_type STREQUAL "INTERFACE_LIBRARY")
        set(RUVIA_MIMALLOC_TARGET mimalloc-static)
    endif()
endif()

set(_ruvia_core_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
set(_ruvia_core_version_header
    "${_ruvia_core_generated_include_dir}/ruvia/core/version.h")
configure_file(
    include/ruvia/core/version.h.in
    "${_ruvia_core_version_header}"
    @ONLY)

add_library(ruvia-core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MimallocOverride.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConnectionScanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EventLoopPool.cpp
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

target_include_directories(ruvia-core
    PUBLIC
        $<BUILD_INTERFACE:${_ruvia_core_generated_include_dir}>
)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_MIMALLOC_TARGET}
        asio::asio
        Threads::Threads
)

target_compile_definitions(ruvia-core
    PRIVATE
        RUVIA_ENABLE_MIMALLOC_OVERRIDE=$<BOOL:${RUVIA_ENABLE_MIMALLOC_OVERRIDE}>
)

ruvia_install_target(ruvia-core core)

ruvia_install_generated_public_header(
    core
    "${_ruvia_core_version_header}"
    ruvia/core/version.h)

ruvia_install_public_headers(core
    include/ruvia/core/Task.h
    include/ruvia/core/Channel.h
    include/ruvia/core/OneShot.h
    include/ruvia/core/TaskScope.h
    include/ruvia/core/Timer.h
    include/ruvia/core/EventLoopPool.h
    include/ruvia/core/WorkerHandle.h
    include/ruvia/core/WorkerWaitResult.h
    include/ruvia/core/memory/MemoryPool.h
    include/ruvia/core/memory/PmrObject.h
    include/ruvia/core/memory/PmrResource.h
    include/ruvia/core/memory/ProcessResource.h
)

ruvia_install_public_header_tree(core include/ruvia/core/detail)

if(WIN32)
    target_link_libraries(ruvia-core PUBLIC Mswsock Ws2_32)
    set_target_properties(ruvia-core PROPERTIES
        INTERFACE_LINK_LIBRARIES_DIRECT "$<TARGET_LINKER_FILE:${RUVIA_MIMALLOC_TARGET}>"
    )
    target_link_options(ruvia-core INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/INCLUDE:mi_version>")
endif()

ruvia_install_export(core)
ruvia_assert_target_file_ownership(ruvia-core)
