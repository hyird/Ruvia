ruvia_collect_cpp_sources(ruvia_web_sources src)
# Keep the shared database sources whenever either driver is enabled; only the
# driver-specific translation units and disabled feature directories are omitted.
if(NOT RUVIA_ENABLE_MARIADB AND NOT RUVIA_ENABLE_POSTGRESQL)
    list(FILTER ruvia_web_sources EXCLUDE REGEX "/src/db/")
else()
    if(NOT RUVIA_ENABLE_MARIADB)
        list(FILTER ruvia_web_sources EXCLUDE
            REGEX "/src/db/(mariadb_driver|mariadb_statements|db_mysql_runtime|db_sql)\\.cpp$")
    endif()
    if(NOT RUVIA_ENABLE_POSTGRESQL)
        list(FILTER ruvia_web_sources EXCLUDE
            REGEX "/src/db/(db_postgresql|postgresql_driver)\\.cpp$")
    endif()
endif()
# The database query-result cache stores rows in Redis.
if(NOT RUVIA_ENABLE_REDIS)
    list(FILTER ruvia_web_sources EXCLUDE REGEX "/src/redis/")
    list(FILTER ruvia_web_sources EXCLUDE REGEX "/src/db/db_query(_result)?_cache\\.cpp$")
endif()
if(NOT RUVIA_ENABLE_JWT)
    list(FILTER ruvia_web_sources EXCLUDE REGEX "/src/auth/jwt[^/]*\\.cpp$")
endif()

add_library(ruvia-web STATIC ${ruvia_web_sources})

ruvia_configure_library(ruvia-web web)

target_link_libraries(ruvia-web
    PUBLIC
        ruvia::core
        ruvia::http
    PRIVATE
        # TLS (ALPN/SNI + the socket client/server drivers) and HMAC cookie
        # signing are Web-layer capabilities, so Web owns the direct OpenSSL
        # dependency while the sans-I/O HTTP target remains independent.
        ${RUVIA_WEB_DEPENDENCIES}
)
target_compile_definitions(ruvia-web PRIVATE ${RUVIA_OPENSSL_DEFINITIONS})

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
if(NOT RUVIA_ENABLE_MARIADB AND NOT RUVIA_ENABLE_POSTGRESQL)
    list(APPEND _ruvia_web_excluded_header_directories db)
endif()
if(NOT RUVIA_ENABLE_REDIS)
    list(APPEND _ruvia_web_excluded_header_directories redis)
endif()
if(NOT RUVIA_ENABLE_JWT)
    list(APPEND _ruvia_web_excluded_header_directories auth)
endif()

ruvia_install_public_header_closure(web include/ruvia/web
    EXCLUDE_DIRECTORIES ${_ruvia_web_excluded_header_directories})

ruvia_install_export(web)
