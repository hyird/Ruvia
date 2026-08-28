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

set(RUVIA_WEB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/App.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppHttpConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppIntegrationConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppListenerOptions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppRunCoordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Dotenv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvPaths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Testing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/WebWorker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auth/CookieSignature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/ClientTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientHandle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientHttp1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientHttp2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientPool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/HttpClientRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/client/WebSocketClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextCookies.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextConnInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextFileResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextRedirect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextRequestFacade.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextRequestForm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextRequestFormParse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextSessionState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/context/ContextStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/ControllerRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/controller/ControllerRouteBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Csrf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/FileConditionalRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Health.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/HttpCors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/HttpRuntimeFacades.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/MultipartReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/SecurityHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/StaticFileTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/StaticFileVariant.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/StaticRootConfigStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/StaticRoot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2BufferedResponseWrite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2SansIoSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2SansIoSessionEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http2/Http2SansIoSendWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/integration/WorkerCapabilities.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/integration/WorkerDataState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ratelimit/RateLimitDecision.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/PathSegments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/Router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDynamicIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterDynamicIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterErrorDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterMiddlewareDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterStreamDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/RouterUrl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ruvia.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpConnectionState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpFileWrite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpResponseCompression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/HttpServerListener.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/TrustedProxies.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/WebWorkerRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server/WebWorkerRuntimeAccept.cpp
)

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
ruvia_assert_target_file_ownership(ruvia-web)
