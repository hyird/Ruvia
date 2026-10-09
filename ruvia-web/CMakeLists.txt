ruvia_fetch_web_dependencies("${RUVIA_ENABLE_MARIADB}" "${RUVIA_ENABLE_POSTGRESQL}" "${RUVIA_ENABLE_REDIS}")

ruvia_collect_cpp_sources(RUVIA_WEB_SOURCES src)
# Keep the shared database sources whenever either driver is enabled; only the
# driver-specific translation units and disabled feature directories are omitted.
if(NOT RUVIA_ENABLE_MARIADB AND NOT RUVIA_ENABLE_POSTGRESQL)
    list(FILTER RUVIA_WEB_SOURCES EXCLUDE REGEX "/src/db/")
else()
    if(NOT RUVIA_ENABLE_MARIADB)
        list(FILTER RUVIA_WEB_SOURCES EXCLUDE
            REGEX "/src/db/(Db|DbMysqlRuntime|DbPoolIo|DbPoolLifecycle|DbSql)\\.cpp$")
    endif()
    if(NOT RUVIA_ENABLE_POSTGRESQL)
        list(FILTER RUVIA_WEB_SOURCES EXCLUDE
            REGEX "/src/db/(DbPostgreSql|Pg[^/]*)\\.cpp$")
    endif()
endif()
if(NOT RUVIA_ENABLE_REDIS)
    list(FILTER RUVIA_WEB_SOURCES EXCLUDE REGEX "/src/redis/")
endif()
if(NOT RUVIA_ENABLE_JWT)
    list(FILTER RUVIA_WEB_SOURCES EXCLUDE REGEX "/src/auth/Jwt[^/]*\\.cpp$")
endif()

add_library(ruvia-web ${RUVIA_WEB_SOURCES})

ruvia_configure_library(ruvia-web web)
ruvia_configure_runtime_library(ruvia-web)

target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_WEB)

target_link_libraries(ruvia-web
    PUBLIC
        ruvia::core
        ruvia::http
    PRIVATE
        # TLS (ALPN/SNI + the socket client/server drivers) and HMAC cookie
        # signing are Web-layer capabilities, so Web owns the direct OpenSSL
        # dependency while the sans-I/O HTTP target remains independent.
        ruvia_dependencies_web
)

if(WIN32)
    target_link_libraries(ruvia-web PRIVATE Ws2_32)
endif()

foreach(_ruvia_feature IN ITEMS MARIADB POSTGRESQL REDIS JWT)
    if(RUVIA_ENABLE_${_ruvia_feature})
        target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_${_ruvia_feature})
    endif()
endforeach()

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_DATABASE)
endif()

ruvia_install_target(ruvia-web web)

set(_ruvia_web_excluded_header_directories)
set(_ruvia_web_excluded_headers)
if(NOT RUVIA_ENABLE_MARIADB AND NOT RUVIA_ENABLE_POSTGRESQL)
    list(APPEND _ruvia_web_excluded_header_directories db)
endif()
if(NOT RUVIA_ENABLE_REDIS)
    if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
        file(GLOB _ruvia_redis_runtime_headers CONFIGURE_DEPENDS
            RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ruvia/web/redis/*.h")
        list(REMOVE_ITEM _ruvia_redis_runtime_headers "include/ruvia/web/redis/RedisTypes.h")
        list(APPEND _ruvia_web_excluded_headers ${_ruvia_redis_runtime_headers})
    else()
        list(APPEND _ruvia_web_excluded_header_directories redis)
    endif()
endif()
if(NOT RUVIA_ENABLE_JWT)
    list(APPEND _ruvia_web_excluded_header_directories auth)
endif()

ruvia_install_public_header_closure(web include/ruvia/web
    EXCLUDE_DIRECTORIES ${_ruvia_web_excluded_header_directories}
    EXCLUDE_HEADERS ${_ruvia_web_excluded_headers})

ruvia_install_export(web)
