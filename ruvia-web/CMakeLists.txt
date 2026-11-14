set(ruvia_web_sources
    src/app.cpp
    src/context_response.cpp
    src/http1_server_session.cpp
    src/http_client_exchange.cpp
    src/http_client_pool.cpp
    src/http_server.cpp
    src/router.cpp
    src/worker_connections.cpp
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_web_sources src/http2_sans_io_session_engine.cpp)
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_web_sources src/http3_session.cpp)
endif()
# Keep the shared database sources whenever either driver is enabled; only the
# driver-specific translation units and disabled features are omitted.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_sources
        src/db_pool.cpp
        src/db_query.cpp
        src/db_session.cpp
    )
endif()
if(RUVIA_ENABLE_MARIADB)
    list(APPEND ruvia_web_sources src/mariadb_driver.cpp)
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_sources src/postgresql_driver.cpp)
endif()
# The database query-result cache stores rows in Redis.
if(RUVIA_ENABLE_REDIS)
    list(APPEND ruvia_web_sources src/redis_pool.cpp)
endif()
if(RUVIA_ENABLE_JWT)
    list(APPEND ruvia_web_sources src/jwt.cpp)
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

ruvia_install_component(ruvia-web web)

set(ruvia_web_headers
    include/app.h
    include/context.h
    include/controller.h
    include/http_client.h
    include/http_client_types.h
    include/http_server.h
    include/middleware.h
    include/model.h
    include/streaming.h
)
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_headers
        include/db.h
        include/db_query.h
        include/db_session.h
    )
endif()
if(RUVIA_ENABLE_REDIS)
    list(APPEND ruvia_web_headers include/redis.h)
endif()
if(RUVIA_ENABLE_JWT)
    list(APPEND ruvia_web_headers include/jwt.h)
endif()
ruvia_install_public_headers(web ${ruvia_web_headers})

