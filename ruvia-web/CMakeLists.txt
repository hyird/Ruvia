set(RUVIA_WEB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Dotenv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/DotenvPaths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/App.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppHttpConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AppIntegrationConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ruvia.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextErrorResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextFileResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextRequest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextResponse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/ContextStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Csrf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Health.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/HttpClientContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/RateLimit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/SecurityHeaders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/Session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpFileZeroCopy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpConnectionState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpServerAccept.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpServerDocumentRoot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/server/HttpServerLifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/Router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterDynamicIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterDynamicIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterIndexBuild.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterMiddlewareDispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/router/core/RouterRegistration.cpp
)

if(RUVIA_ENABLE_MARIADB)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/Db.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbHandle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbMigration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbMysqlRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbPoolLifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbPoolSlots.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbSlotSocket.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbSql.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/db/core/DbTypes.cpp)
endif()

if(RUVIA_ENABLE_REDIS)
    list(APPEND RUVIA_WEB_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/Redis.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisCommandArgs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandleCommandOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandleAdvanced.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandleHashList.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandleHelpers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisHandleSetSorted.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisPipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisPool.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisPoolIo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisPoolLifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisPoolSlots.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisProtocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisReplyParsers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisTransaction.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/redis/core/RedisTypes.cpp)
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

target_include_directories(ruvia-web
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_WEB)

target_link_libraries(ruvia-web
    PUBLIC
        ruvia::http
)

if(RUVIA_ENABLE_MARIADB)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_MARIADB)
    target_link_libraries(ruvia-web PUBLIC unofficial::libmariadb)
endif()

if(RUVIA_ENABLE_REDIS)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_REDIS)
    target_link_libraries(ruvia-web PUBLIC hiredis::hiredis)
endif()

if(RUVIA_ENABLE_JWT)
    target_compile_definitions(ruvia-web PUBLIC RUVIA_ENABLE_JWT)
endif()

ruvia_install_target(ruvia-web web)

ruvia_install_public_headers(web
    include/ruvia/app/App.h
    include/ruvia/app/AppHook.h
    include/ruvia/app/Dotenv.h
    include/ruvia/app/RateLimitRule.h
    include/ruvia/http/Context.h
    include/ruvia/http/Context.inl
    include/ruvia/http/ContextModel.h
    include/ruvia/http/Controller.h
    include/ruvia/http/ControllerDescriptors.h
    include/ruvia/http/ControllerRuntime.h
    include/ruvia/http/ControllerTypes.h
    include/ruvia/http/Csrf.h
    include/ruvia/http/Health.h
    include/ruvia/http/MiddlewareDescriptor.h
    include/ruvia/http/MiddlewareRuntime.h
    include/ruvia/http/Model.h
    include/ruvia/http/ModelJson.h
    include/ruvia/http/ModelObject.h
    include/ruvia/http/ModelTypes.h
    include/ruvia/http/Next.h
    include/ruvia/http/RateLimit.h
    include/ruvia/http/SecurityHeaders.h
    include/ruvia/http/Session.h
    include/ruvia/http/Validation.h
    include/ruvia/http/ValidationTypes.h
    include/ruvia/http/detail/ContextValues.h
    include/ruvia/http/detail/RegistrationResource.h
    include/ruvia/http/detail/ValidatedValues.h
    include/ruvia/http/detail/json/JsonLex.h
    include/ruvia/http/detail/json/JsonLimits.h
    include/ruvia/http/detail/json/JsonNumber.h
    include/ruvia/http/detail/json/JsonObjectFields.h
    include/ruvia/http/detail/json/JsonScanner.h
    include/ruvia/http/detail/json/JsonSkip.h
    include/ruvia/http/detail/json/JsonString.h
    include/ruvia/http/detail/model/FieldAssign.h
    include/ruvia/http/detail/model/FormParser.h
    include/ruvia/http/detail/model/JsonParser.h
    include/ruvia/http/detail/model/JsonWriter.h
    include/ruvia/http/detail/model/MacroCore.h
    include/ruvia/http/detail/model/MacroFieldOps.h
    include/ruvia/http/detail/model/MacroJsonOps.h
    include/ruvia/http/detail/model/Macros.h
    include/ruvia/http/detail/model/ModelOptions.h
    include/ruvia/http/detail/model/Parser.h
    include/ruvia/http/detail/model/PatternCompiler.h
    include/ruvia/http/detail/model/PatternMatcher.h
    include/ruvia/http/detail/model/PatternTypes.h
    include/ruvia/http/detail/model/RegexMatcher.h
    include/ruvia/http/detail/model/RequestFieldVisitors.h
    include/ruvia/http/detail/model/RequestObjectVisitors.h
    include/ruvia/http/detail/model/Rules.h
    include/ruvia/http/detail/model/RuleSupport.h
    include/ruvia/http/detail/model/RuleTypes.h
    include/ruvia/http/detail/model/RuleValidation.h
    include/ruvia/http/detail/model/Traits.h
    include/ruvia/router/Router.h
)

if(RUVIA_ENABLE_MARIADB)
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ruvia/db
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
        COMPONENT web
    )
endif()

if(RUVIA_ENABLE_REDIS)
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ruvia/redis
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
        COMPONENT web
    )
endif()

if(RUVIA_ENABLE_JWT)
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ruvia/auth
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ruvia
        COMPONENT web
    )
endif()
