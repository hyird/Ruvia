ruvia_collect_cpp_sources(ruvia_core_sources src)
add_library(ruvia-core STATIC ${ruvia_core_sources})

ruvia_configure_library(ruvia-core core)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_CORE_DEPENDENCIES}
    PRIVATE
        $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
)
target_compile_definitions(ruvia-core PUBLIC ASIO_STANDALONE)

ruvia_install_target(ruvia-core core)

ruvia_install_public_header_closure(core include/ruvia/core)

if(WIN32)
    target_link_libraries(ruvia-core PUBLIC ws2_32 mswsock PRIVATE Advapi32 Bcrypt)
endif()

ruvia_install_export(core)
