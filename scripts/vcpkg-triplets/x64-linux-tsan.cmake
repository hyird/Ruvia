# Triplet for the TSan load test (scripts/sanitizer_load_test.sh tsan): the
# standard static linux triplet, except mimalloc is built by clang with
# MI_DEBUG_TSAN so its internal synchronization (thread-heap init, meta
# alloc/free, block reuse) is visible to TSan instead of surfacing as false
# races in every consumer frame. mimalloc's MI_DEBUG_TSAN is clang-only, which
# is why the load-test script drives the whole build with clang.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT STREQUAL "mimalloc")
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/mimalloc-clang-tsan-toolchain.cmake")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DMI_DEBUG_TSAN=ON)
endif()
