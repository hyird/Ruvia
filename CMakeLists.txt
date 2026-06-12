cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Default vcpkg triplet")
endif()

project(ruvia VERSION 0.0.4 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(mimalloc CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

option(RUVIA_ENABLE_MARIADB "Build MariaDB-compatible database support" OFF)
option(RUVIA_ENABLE_REDIS "Build Redis support" OFF)
option(RUVIA_ENABLE_JWT "Build JWT helper support" OFF)

if(RUVIA_ENABLE_MARIADB)
    find_package(unofficial-libmariadb CONFIG REQUIRED)
endif()

if(RUVIA_ENABLE_REDIS)
    find_package(hiredis CONFIG REQUIRED)
endif()

function(ruvia_deduplicate_link_interface target)
    if(NOT TARGET ${target})
        return()
    endif()

    get_target_property(_ruvia_link_interface ${target} INTERFACE_LINK_LIBRARIES)
    if(NOT _ruvia_link_interface)
        return()
    endif()

    list(REMOVE_DUPLICATES _ruvia_link_interface)
    set_target_properties(${target} PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_ruvia_link_interface}")
endfunction()

if(RUVIA_ENABLE_MARIADB)
    ruvia_deduplicate_link_interface(unofficial::libmariadb)
endif()

set(RUVIA_SOURCES
    src/app/Dotenv.cpp
    src/app/App.cpp
    src/ruvia.cpp
    src/http/Controller.cpp
    src/http/Error.cpp
    src/http/HttpParser.cpp
    src/http/HttpTypes.cpp
    src/memory/MemoryPool.cpp
    src/memory/MimallocOverride.cpp
    src/net/ConnectionScanner.cpp
    src/net/HttpDateCache.cpp
    src/net/HttpResponseWriter.cpp
    src/net/HttpWebSocketUtils.cpp
    src/net/HttpServer.cpp
    src/router/Router.cpp
)

if(RUVIA_ENABLE_MARIADB)
    list(APPEND RUVIA_SOURCES src/db/Db.cpp)
endif()

if(RUVIA_ENABLE_REDIS)
    list(APPEND RUVIA_SOURCES src/redis/Redis.cpp)
endif()

if(RUVIA_ENABLE_JWT)
    list(APPEND RUVIA_SOURCES src/auth/Jwt.cpp)
endif()

add_library(ruvia ${RUVIA_SOURCES})

add_library(ruvia::ruvia ALIAS ruvia)

set_target_properties(ruvia PROPERTIES
    EXPORT_NAME ruvia
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

function(ruvia_configure_library target)
    target_compile_features(${target} PUBLIC cxx_std_23)

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_definitions(${target}
        PUBLIC
            ASIO_STANDALONE
    )
    target_precompile_headers(${target} PRIVATE src/pch.h)

    if(WIN32)
        target_compile_definitions(${target} PUBLIC _WIN32_WINNT=0x0A00)
    endif()

    if(MSVC)
        target_compile_options(${target} PUBLIC /utf-8 /Zc:preprocessor)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

ruvia_configure_library(ruvia)

set(RUVIA_MIMALLOC_TARGET mimalloc)
if(TARGET mimalloc-static)
    get_target_property(_ruvia_mimalloc_static_type mimalloc-static TYPE)
    if(NOT _ruvia_mimalloc_static_type STREQUAL "INTERFACE_LIBRARY")
        set(RUVIA_MIMALLOC_TARGET mimalloc-static)
    endif()
endif()

target_link_libraries(ruvia
    PUBLIC
        asio::asio
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        ${RUVIA_MIMALLOC_TARGET}
)

if(RUVIA_ENABLE_MARIADB)
    target_compile_definitions(ruvia PUBLIC RUVIA_ENABLE_MARIADB)
    target_link_libraries(ruvia PUBLIC unofficial::libmariadb)
endif()

if(RUVIA_ENABLE_REDIS)
    target_compile_definitions(ruvia PUBLIC RUVIA_ENABLE_REDIS)
    target_link_libraries(ruvia PUBLIC hiredis::hiredis)
endif()

if(RUVIA_ENABLE_JWT)
    target_compile_definitions(ruvia PUBLIC RUVIA_ENABLE_JWT)
endif()
target_link_options(ruvia
    INTERFACE
        $<$<PLATFORM_ID:Darwin>:LINKER:-no_warn_duplicate_libraries>
)

if(WIN32)
    target_link_libraries(ruvia PUBLIC Mswsock)
    set_target_properties(ruvia PROPERTIES
        INTERFACE_LINK_LIBRARIES_DIRECT "$<TARGET_LINKER_FILE:${RUVIA_MIMALLOC_TARGET}>"
    )
    target_link_options(ruvia INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/INCLUDE:mi_version>")
endif()

option(RUVIA_BUILD_EXAMPLES "Build Ruvia examples" OFF)
option(RUVIA_BUILD_TECHEMPOWER "Build Ruvia TechEmpower benchmark app" OFF)

if(RUVIA_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RUVIA_BUILD_TECHEMPOWER)
    add_subdirectory(benchmarks/techempower/ruvia)
endif()

install(
    TARGETS
        ruvia
    EXPORT ruviaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    FILES include/ruvia/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
)

install(
    DIRECTORY
        include/ruvia/app
        include/ruvia/http
        include/ruvia/memory
        include/ruvia/router
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
)

if(RUVIA_ENABLE_MARIADB)
    install(
        DIRECTORY include/ruvia/db
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
    )
endif()

if(RUVIA_ENABLE_REDIS)
    install(
        DIRECTORY include/ruvia/redis
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
    )
endif()

if(RUVIA_ENABLE_JWT)
    install(
        DIRECTORY include/ruvia/auth
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
    )
endif()

set(_ruvia_config [=[
include(CMakeFindDependencyMacro)

find_dependency(asio CONFIG REQUIRED)
find_dependency(Threads REQUIRED)
find_dependency(mimalloc CONFIG REQUIRED)
find_dependency(OpenSSL REQUIRED)
find_dependency(ZLIB REQUIRED)

set(RUVIA_ENABLE_MARIADB @RUVIA_ENABLE_MARIADB@)
set(RUVIA_ENABLE_REDIS @RUVIA_ENABLE_REDIS@)
set(RUVIA_ENABLE_JWT @RUVIA_ENABLE_JWT@)

if(RUVIA_ENABLE_MARIADB)
    find_dependency(unofficial-libmariadb CONFIG REQUIRED)
endif()

if(RUVIA_ENABLE_REDIS)
    find_dependency(hiredis CONFIG REQUIRED)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/ruviaTargets.cmake")

set(_ruvia_requested_components ${ruvia_FIND_COMPONENTS})
if(NOT _ruvia_requested_components)
    set(_ruvia_requested_components ruvia)
endif()

foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(_ruvia_component STREQUAL "ruvia")
        set(ruvia_${_ruvia_component}_FOUND TRUE)
    elseif(TARGET ruvia::${_ruvia_component})
        set(ruvia_${_ruvia_component}_FOUND TRUE)
    else()
        set(ruvia_${_ruvia_component}_FOUND FALSE)
    endif()
endforeach()

foreach(_ruvia_component IN LISTS _ruvia_requested_components)
    if(NOT ruvia_${_ruvia_component}_FOUND)
        set(ruvia_FOUND FALSE)
    endif()
endforeach()
]=])

string(CONFIGURE "${_ruvia_config}" _ruvia_config @ONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake "${_ruvia_config}")

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ruviaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
)

install(
    EXPORT ruviaTargets
    FILE ruviaTargets.cmake
    NAMESPACE ruvia::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ruvia
)
