include_guard(GLOBAL)
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/RuviaNativeDependencies.cmake")

# Release archives and hashes are updated together. Dependency options stay in
# function scope so embedding Ruvia never changes the parent project's options.
FetchContent_Declare(ruvia_asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz
    URL_HASH SHA256=9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db
    SOURCE_SUBDIR ruvia-no-cmake)
FetchContent_Declare(ruvia_zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    EXCLUDE_FROM_ALL SYSTEM)
FetchContent_Declare(ruvia_brotli
    URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
    URL_HASH SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
    EXCLUDE_FROM_ALL SYSTEM)
FetchContent_Declare(ruvia_zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake EXCLUDE_FROM_ALL SYSTEM)
FetchContent_Declare(ruvia_ngtcp2
    URL https://github.com/ngtcp2/ngtcp2/releases/download/v1.25.0/ngtcp2-1.25.0.tar.gz
    URL_HASH SHA256=1c0843076528a87b65e9a9d455100941f4cb65d44f96c5da6ae56df146043955
    EXCLUDE_FROM_ALL SYSTEM)
FetchContent_Declare(ruvia_openssl
    URL https://github.com/openssl/openssl/releases/download/openssl-4.0.3/openssl-4.0.3.tar.gz
    URL_HASH SHA256=325b5c806167c13b40b1ffeadfe0248197c00eccc4cf123ec1e28d2d2fd216d9
    SOURCE_SUBDIR ruvia-no-cmake)
FetchContent_Declare(ruvia_mariadb
    URL https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v3.4.11.tar.gz
    URL_HASH SHA256=7cdb35c571dd0c187f806f569285829d31b4aba4feb93957a034b0b1e887e276
    SOURCE_SUBDIR ruvia-no-cmake)
FetchContent_Declare(ruvia_postgresql
    URL https://ftp.postgresql.org/pub/source/v18.6/postgresql-18.6.tar.gz
    URL_HASH SHA256=983ee554ec53dbeb9b70797bef9fcf4e67e117e7e48ca1463cc80b3ff8e8ff3f
    SOURCE_SUBDIR ruvia-no-cmake)
FetchContent_Declare(ruvia_hiredis
    URL https://github.com/redis/hiredis/archive/refs/tags/v1.4.1.tar.gz
    URL_HASH SHA256=ca3180359a8b1275838a45415851f8cd5c411e27bdbf18f4823012e45507d2e4
    EXCLUDE_FROM_ALL SYSTEM)

function(ruvia_fetch_core_dependencies)
    if(TARGET ruvia_dependencies_core)
        return()
    endif()
    FetchContent_MakeAvailable(ruvia_asio)
    find_package(Threads REQUIRED)
    add_library(ruvia_dependencies_core INTERFACE IMPORTED GLOBAL)
    set_target_properties(ruvia_dependencies_core PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ruvia_asio_SOURCE_DIR}/include"
        INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE
        INTERFACE_LINK_LIBRARIES Threads::Threads)
    if(WIN32)
        target_link_libraries(ruvia_dependencies_core INTERFACE ws2_32 mswsock)
    endif()
endfunction()

function(ruvia_fetch_http_dependencies)
    if(TARGET ruvia_dependencies_http)
        return()
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_STATIC ON)
    set(ZLIB_INSTALL OFF)
    set(BROTLI_DISABLE_TESTS ON)
    set(BROTLI_BUNDLED_MODE ON)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_STATIC ON)
    set(ENABLE_LIB_ONLY ON)
    set(ENABLE_STATIC_LIB ON)
    set(ENABLE_SHARED_LIB OFF)
    set(ENABLE_OPENSSL OFF)
    set(ENABLE_GNUTLS OFF)
    set(ENABLE_BORINGSSL OFF)
    set(ENABLE_PICOTLS OFF)
    set(ENABLE_WOLFSSL OFF)
    FetchContent_MakeAvailable(ruvia_zlib ruvia_brotli ruvia_zstd ruvia_ngtcp2)
    add_library(ruvia_dependencies_http INTERFACE IMPORTED GLOBAL)
    target_link_libraries(ruvia_dependencies_http INTERFACE
        zlibstatic brotlienc brotlidec brotlicommon libzstd_static ngtcp2_static)
endfunction()

function(ruvia_fetch_web_dependencies mariadb postgresql redis)
    if(TARGET ruvia_dependencies_web)
        return()
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    ruvia_fetch_openssl()
    add_library(ruvia_dependencies_web INTERFACE IMPORTED GLOBAL)
    target_compile_definitions(ruvia_dependencies_web INTERFACE
        OPENSSL_API_COMPAT=40000 OPENSSL_NO_DEPRECATED)
    target_link_libraries(ruvia_dependencies_web INTERFACE ruvia_openssl_ssl)
    if(mariadb)
        ruvia_fetch_mariadb()
        target_link_libraries(ruvia_dependencies_web INTERFACE ruvia_mariadb_client)
    endif()
    if(postgresql)
        ruvia_fetch_postgresql()
        target_link_libraries(ruvia_dependencies_web INTERFACE ruvia_postgresql_backend)
    endif()
    if(redis)
        set(DISABLE_TESTS ON)
        set(ENABLE_EXAMPLES OFF)
        set(ENABLE_SSL OFF)
        FetchContent_MakeAvailable(ruvia_hiredis)
        if(MSVC)
            target_compile_options(hiredis PRIVATE /utf-8)
        endif()
        target_link_libraries(ruvia_dependencies_web INTERFACE hiredis)
    endif()
endfunction()
