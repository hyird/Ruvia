ruvia_collect_cpp_sources(ruvia_core_sources src)
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

ruvia_install_public_header_closure(core include/ruvia/core)

