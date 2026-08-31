find_package(OpenSSL REQUIRED)

if(RUVIA_ENABLE_MARIADB)
    find_package(unofficial-libmariadb CONFIG REQUIRED)
    ruvia_deduplicate_link_interface(unofficial::libmariadb)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    find_package(PostgreSQL REQUIRED)
    ruvia_deduplicate_link_interface(PostgreSQL::PostgreSQL)
endif()
if(RUVIA_ENABLE_REDIS)
    find_package(hiredis CONFIG REQUIRED)
endif()

ruvia_collect_cpp_sources(RUVIA_WEB_SOURCES src)
list(FILTER RUVIA_WEB_SOURCES EXCLUDE REGEX "/src/(db|redis)/")
list(FILTER RUVIA_WEB_SOURCES EXCLUDE REGEX "/src/auth/Jwt[^/]*\\.cpp$")

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbClient.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbHandle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbHostResolution.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbMigration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPreparedStatement.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbSlotSocket.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbStreamResult.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbTransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbTypes.cpp)
endif()

if(RUVIA_ENABLE_MARIADB)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/Db.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbMysqlRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPoolLifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbSql.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPostgreSql.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/PgDb.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/PgPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/PgPoolLifecycle.cpp)
endif()

if(RUVIA_ENABLE_REDIS)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/Redis.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisCommandArgs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandleCommandOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandleAdvanced.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandleHashList.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandleHelpers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisHandleSetSorted.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisPool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisPoolLifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisPoolSlots.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisProtocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisReplyParsers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisTransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/RedisTypes.cpp)
endif()

if(RUVIA_ENABLE_JWT)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/Jwt.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/JwtCommon.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/JwtCrypto.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/JwtEncoding.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/JwtJson.cpp)
endif()

add_library(ruvia-web ${RUVIA_WEB_SOURCES})
add_library(ruvia::web ALIAS ruvia-web)

set_target_properties(ruvia-web PROPERTIES
    EXPORT_NAME web
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

ruvia_configure_library(ruvia-web)
ruvia_configure_runtime_library(ruvia-web)

target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_WEB)

target_link_libraries(ruvia-web
    PUBLIC
        ruvia::core
        ruvia::http
        # TLS (ALPN/SNI + the socket client/server drivers) and HMAC cookie
        # signing are Web-layer capabilities, so Web owns the direct OpenSSL
        # dependency while the sans-I/O HTTP target remains independent.
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(RUVIA_ENABLE_MARIADB)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_MARIADB)
    target_link_libraries(ruvia-web PUBLIC unofficial::libmariadb)
endif()

if(RUVIA_ENABLE_POSTGRESQL)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_POSTGRESQL)
    target_link_libraries(ruvia-web PUBLIC PostgreSQL::PostgreSQL)
    # vcpkg's static libpq splits internal frontend symbols between pgcommon
    # and pgport. GNU linkers need to rescan that cyclic pair; Apple ld only
    # needs the two static archives to be propagated after libpq.
    if(PostgreSQL_pgcommon_LIBRARY_DEBUG AND
       PostgreSQL_pgcommon_LIBRARY_RELEASE AND PostgreSQL_pgport_LIBRARY_DEBUG AND
       PostgreSQL_pgport_LIBRARY_RELEASE)
        string(CONCAT _ruvia_postgresql_pgcommon
            "$<$<CONFIG:Debug>:${PostgreSQL_pgcommon_LIBRARY_DEBUG}>"
            "$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgcommon_LIBRARY_RELEASE}>")
        string(CONCAT _ruvia_postgresql_pgport
            "$<$<CONFIG:Debug>:${PostgreSQL_pgport_LIBRARY_DEBUG}>"
            "$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgport_LIBRARY_RELEASE}>")
        if(MINGW OR (UNIX AND NOT APPLE))
            string(CONCAT _ruvia_postgresql_link_group
                "$<LINK_GROUP:RESCAN,"
                "${_ruvia_postgresql_pgcommon}"
                ","
                "${_ruvia_postgresql_pgport}"
                ">")
            target_link_libraries(ruvia-web PUBLIC "${_ruvia_postgresql_link_group}")
        else()
            target_link_libraries(ruvia-web PUBLIC
                "${_ruvia_postgresql_pgcommon}"
                "${_ruvia_postgresql_pgport}")
        endif()
    endif()
endif()

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_DATABASE)
endif()

if(RUVIA_ENABLE_REDIS)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_REDIS)
    target_link_libraries(ruvia-web PUBLIC hiredis::hiredis)
endif()

if(RUVIA_ENABLE_JWT)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_JWT)
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
