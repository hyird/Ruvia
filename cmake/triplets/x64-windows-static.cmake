set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)
if(PORT STREQUAL "brotli")
    # Encoder allocation failures must return to the request, never exit the process.
    string(APPEND VCPKG_C_FLAGS " /DBROTLI_ENCODER_CLEANUP_ON_OOM")
    string(APPEND VCPKG_CXX_FLAGS " /DBROTLI_ENCODER_CLEANUP_ON_OOM")
endif()
