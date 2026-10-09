include_guard(GLOBAL)
include(ProcessorCount)

# OpenSSL and PostgreSQL do not ship CMake projects. FetchContent still owns
# their source archives; build rules invoke the supported upstream toolchains.
function(ruvia_native_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 native "" "" "LIBRARIES;DEPENDS;ARGUMENTS")
    FetchContent_MakeAvailable(ruvia_${name})
    FetchContent_GetProperties(ruvia_${name} SOURCE_DIR source)
    if(name STREQUAL "openssl")
        file(SHA256 "${source}/VERSION.dat" source_version)
    elseif(name STREQUAL "postgresql")
        file(SHA256 "${source}/configure.ac" source_version)
    else()
        file(SHA256 "${source}/CMakeLists.txt" source_version)
    endif()
    string(SHA256 build_id "${source_version};${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_VERSION};${native_ARGUMENTS}")
    string(SUBSTRING "${build_id}" 0 12 build_id)
    set(root "${FETCHCONTENT_BASE_DIR}/ruvia-${name}-${build_id}")
    foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY "${root}/${config}/include" "${root}/${config}/lib")
    endforeach()
    ProcessorCount(jobs)
    if(NOT jobs)
        set(jobs 1)
    endif()
    set(configuration "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>")
    set(outputs)
    foreach(library IN LISTS native_LIBRARIES)
        list(APPEND outputs "${root}/${configuration}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${library}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endforeach()
    add_custom_command(
        OUTPUT "${root}/${configuration}/complete.stamp"
        BYPRODUCTS ${outputs}
        COMMAND "${CMAKE_COMMAND}"
            "-Dkind=${name}" "-Dsource=${source}" "-Droot=${root}"
            "-Dconfig=${configuration}" "-Djobs=${jobs}"
            "-Dcompiler=${CMAKE_C_COMPILER}" "-Dgenerator=${CMAKE_GENERATOR}"
            "-Dplatform=${CMAKE_GENERATOR_PLATFORM}"
            "-Dlibrary_prefix=${CMAKE_STATIC_LIBRARY_PREFIX}"
            "-Dlibrary_suffix=${CMAKE_STATIC_LIBRARY_SUFFIX}"
            ${native_ARGUMENTS}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RuviaNativeBuild.cmake"
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RuviaNativeBuild.cmake" ${native_DEPENDS}
        VERBATIM USES_TERMINAL)
    add_custom_target(ruvia_build_${name} DEPENDS "${root}/${configuration}/complete.stamp")
    foreach(library IN LISTS native_LIBRARIES)
        add_library(ruvia_${library} STATIC IMPORTED GLOBAL)
        set_target_properties(ruvia_${library} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
            IMPORTED_LOCATION "${root}/Release/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${library}${CMAKE_STATIC_LIBRARY_SUFFIX}"
            INTERFACE_INCLUDE_DIRECTORIES "${root}/${configuration}/include")
        foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${config}" upper)
            set_property(TARGET ruvia_${library} PROPERTY IMPORTED_LOCATION_${upper}
                "${root}/${config}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${library}${CMAKE_STATIC_LIBRARY_SUFFIX}")
        endforeach()
        add_dependencies(ruvia_${library} ruvia_build_${name})
    endforeach()
    set(ruvia_${name}_ROOT "${root}" PARENT_SCOPE)
endfunction()

function(ruvia_fetch_openssl)
    if(TARGET ruvia_openssl_ssl)
        return()
    endif()
    find_package(Perl REQUIRED)
    if(WIN32)
        find_program(ruvia_jom NAMES jom REQUIRED)
        set(make "${ruvia_jom}")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
            set(target VC-WIN64-ARM)
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(target VC-WIN64A)
        else()
            set(target VC-WIN32)
        endif()
    else()
        find_program(ruvia_make NAMES gmake make REQUIRED)
        set(make "${ruvia_make}")
        set(target)
    endif()
    ruvia_native_dependency(openssl
        LIBRARIES openssl_ssl openssl_crypto
        ARGUMENTS "-Dperl=${PERL_EXECUTABLE}" "-Dmake=${make}" "-Dopenssl_target=${target}")
    target_link_libraries(ruvia_openssl_ssl INTERFACE ruvia_openssl_crypto)
    if(WIN32)
        target_link_libraries(ruvia_openssl_crypto INTERFACE ws2_32 crypt32 advapi32 user32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(ruvia_openssl_crypto INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    endif()
    set_property(GLOBAL PROPERTY RUVIA_OPENSSL_ROOT "${ruvia_openssl_ROOT}")
endfunction()

function(ruvia_fetch_mariadb)
    get_property(openssl_root GLOBAL PROPERTY RUVIA_OPENSSL_ROOT)
    FetchContent_GetProperties(ruvia_zlib SOURCE_DIR zlib_source)
    ruvia_native_dependency(mariadb
        LIBRARIES mariadb_client
        DEPENDS ruvia_build_openssl zlibstatic
        ARGUMENTS "-Dopenssl_root=${openssl_root}" "-Dzlib_include=${zlib_source}"
            "-Dzlib_library=$<TARGET_FILE:zlibstatic>")
    target_link_libraries(ruvia_mariadb_client INTERFACE ruvia_openssl_ssl zlibstatic)
    if(WIN32)
        target_link_libraries(ruvia_mariadb_client INTERFACE ws2_32 shlwapi secur32 bcrypt)
    else()
        target_link_libraries(ruvia_mariadb_client INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
endfunction()

function(ruvia_fetch_postgresql)
    find_program(ruvia_meson NAMES meson REQUIRED)
    find_package(Perl REQUIRED)
    get_property(openssl_root GLOBAL PROPERTY RUVIA_OPENSSL_ROOT)
    ruvia_native_dependency(postgresql
        LIBRARIES postgresql_client postgresql_common postgresql_port postgresql_frontend
        DEPENDS ruvia_build_openssl
        ARGUMENTS "-Dopenssl_root=${openssl_root}" "-Dmeson=${ruvia_meson}" "-Dperl=${PERL_EXECUTABLE}")
    add_library(ruvia_postgresql_backend INTERFACE IMPORTED GLOBAL)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(ruvia_postgresql_backend INTERFACE
            "$<LINK_GROUP:RESCAN,ruvia_postgresql_client,ruvia_postgresql_common,ruvia_postgresql_port,ruvia_postgresql_frontend>")
    else()
        target_link_libraries(ruvia_postgresql_backend INTERFACE
            ruvia_postgresql_client ruvia_postgresql_common ruvia_postgresql_port ruvia_postgresql_frontend)
    endif()
    target_link_libraries(ruvia_postgresql_backend INTERFACE ruvia_openssl_ssl)
    if(WIN32)
        target_link_libraries(ruvia_postgresql_backend INTERFACE ws2_32 secur32 shell32)
    else()
        target_link_libraries(ruvia_postgresql_backend INTERFACE Threads::Threads m)
    endif()
endfunction()
