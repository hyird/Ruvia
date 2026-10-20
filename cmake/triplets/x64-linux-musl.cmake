set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../llvm.cmake")
set(VCPKG_ENV_PASSTHROUGH RUVIA_MUSL_ROOT FLEXEDGE_MUSL_ROOT)
# Meson uses shared-linker settings for executable probes; override those probes directly.
set(VCPKG_MESON_CONFIGURE_OPTIONS "-Dc_link_args=-static" "-Dcpp_link_args=-static")
if(PORT STREQUAL "brotli")
    # Encoder allocation failures must return to the request, never exit the process.
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_C_FLAGS=-DBROTLI_ENCODER_CLEANUP_ON_OOM")
endif()
