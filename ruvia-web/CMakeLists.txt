set(RUVIA_WEB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Dotenv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvPaths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/App.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/WebWorker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppHttpConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppIntegrationConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ruvia.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextErrorResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextFileResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextRequestFacade.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextRequestForm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextSessionState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Csrf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Health.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/HttpRuntimeFacades.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ScopedOperation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/MultipartReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/RateLimit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Streaming.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CookieSignature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticFiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/HttpCors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseCompression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Http2BufferedResponseWrite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/Http2SansIoSendWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/SecurityHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpFileWrite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpConnectionState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpServerAccept.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpServerDocumentRoot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpServerLifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/ControllerRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/Router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDynamicIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDynamicIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterMiddlewareDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterRegistration.cpp
)

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbHandle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbMigration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbSlotSocket.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbTypes.cpp)
endif()

if(RUVIA_ENABLE_MARIADB)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/Db.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbMysqlRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPoolLifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/DbPoolSlots.cpp
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
        # TLS (ALPN/SNI + the socket client/server drivers) and HMAC cookie signing are
        # WEB-layer capabilities, so web links OpenSSL directly. Previously it came in
        # transitively via ruvia-http, which had a dead OpenSSL link -- removing that
        # dead link surfaced web's real, direct dependency.
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

# Install the always-available Web contracts without leaking opt-in feature
# contracts into a feature-off package. Feature subtrees are installed below
# together with the public API that makes them usable.
ruvia_install_public_header_tree(web include/ruvia/web/detail
    PATTERN "auth" EXCLUDE
    PATTERN "db" EXCLUDE
    PATTERN "redis" EXCLUDE)

ruvia_install_public_headers(web
    include/ruvia/web/App.h
    include/ruvia/web/AppHook.h
    include/ruvia/web/Dotenv.h
    include/ruvia/web/Error.h
    include/ruvia/web/RateLimitRule.h
    include/ruvia/web/StaticFiles.h
    include/ruvia/web/ConnInfo.h
    include/ruvia/web/Context.h
    include/ruvia/web/ContextRequest.h
    include/ruvia/web/Controller.h
    include/ruvia/web/Csrf.h
    include/ruvia/web/ErrorHandlers.h
    include/ruvia/web/Health.h
    include/ruvia/web/ServerConfig.h
    include/ruvia/web/Middleware.h
    include/ruvia/web/Model.h
    include/ruvia/web/ModelJson.h
    include/ruvia/web/ModelObject.h
    include/ruvia/web/ModelTypes.h
    include/ruvia/web/MultipartReader.h
    include/ruvia/web/Next.h
    include/ruvia/web/RateLimit.h
    include/ruvia/web/RequestFields.h
    include/ruvia/web/SecurityHeaders.h
    include/ruvia/web/Session.h
    include/ruvia/web/ScopedOperation.h
    include/ruvia/web/Streaming.h
    include/ruvia/web/Validation.h
    include/ruvia/web/ValidationTypes.h
    include/ruvia/web/WebSocket.h
    include/ruvia/web/WebWorker.h
    include/ruvia/web/Router.h
)

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_install_public_header_tree(web include/ruvia/web/db)
    ruvia_install_public_headers(web
        include/ruvia/web/detail/db/DbBackend.h
        include/ruvia/web/detail/db/DbConfigValidation.h
        include/ruvia/web/detail/db/DbInternal.h
        include/ruvia/web/detail/db/DbMigrationValidation.h
        include/ruvia/web/detail/db/DbOperationState.h
        include/ruvia/web/detail/db/DbPoolDeadline.h
        include/ruvia/web/detail/db/DbResultAccess.h
        include/ruvia/web/detail/db/DbSlotSocket.h
        include/ruvia/web/detail/db/DbUtils.h
        include/ruvia/web/detail/db/DbValueAccess.h)
endif()

if(RUVIA_ENABLE_MARIADB)
    ruvia_install_public_headers(web
        include/ruvia/web/detail/db/DbMysqlRuntime.h
        include/ruvia/web/detail/db/DbSql.h)
endif()

if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_install_public_headers(web
        include/ruvia/web/detail/db/DbPostgreSql.h)
endif()

if(RUVIA_ENABLE_REDIS)
    ruvia_install_public_header_tree(web include/ruvia/web/redis)
    ruvia_install_public_header_tree(web include/ruvia/web/detail/redis)
endif()

if(RUVIA_ENABLE_JWT)
    ruvia_install_public_header_tree(web include/ruvia/web/auth)
    ruvia_install_public_header_tree(web include/ruvia/web/detail/auth)
endif()
