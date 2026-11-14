set(ruvia_web_sources
    src/app.cpp
    src/app_run.cpp
    src/app_runtime_control.cpp
    src/client_happy_eyeballs.cpp
    src/client_host_resolver.cpp
    src/client_tcp_transport.cpp
    src/client_tls.cpp
    src/connection_scanner.cpp
    src/context.cpp
    src/controller.cpp
    src/document_root_publisher.cpp
    src/env.cpp
    src/file_response.cpp
    src/http1_client_exchange.cpp
    src/http1_server_session.cpp
    src/http_client.cpp
    src/http_client_advertisement.cpp
    src/http_client_connection.cpp
    src/http_client_pool.cpp
    src/http_client_request.cpp
    src/http_client_response.cpp
    src/http_client_upload_tunnel.cpp
    src/http_client_websocket.cpp
    src/http_error.cpp
    src/http_request_admission.cpp
    src/http_server.cpp
    src/http_tunnel.cpp
    src/http_tunnel_session.cpp
    src/middleware.cpp
    src/rate_limiting.cpp
    src/request_stream.cpp
    src/response_coding_policy.cpp
    src/response_file.cpp
    src/route_plan.cpp
    src/route_table.cpp
    src/router.cpp
    src/security_middleware.cpp
    src/server_connection.cpp
    src/server_listener.cpp
    src/server_options.cpp
    src/server_socket_stream.cpp
    src/session.cpp
    src/static_root.cpp
    src/streaming.cpp
    src/tcp_client.cpp
    src/test_app.cpp
    src/tls.cpp
    src/tls_server.cpp
    src/web_worker_runtime.cpp
    src/websocket.cpp
    src/windows_acceptor.cpp
    src/worker_capabilities.cpp
    src/worker_connections.cpp
)
if(RUVIA_ENABLE_HTTP2)
    list(APPEND ruvia_web_sources
        src/http2_client.cpp
        src/http2_session.cpp
    )
endif()
if(RUVIA_ENABLE_HTTP3)
    list(APPEND ruvia_web_sources
        src/http3_client.cpp
        src/http3_endpoint.cpp
        src/http3_session.cpp
    )
endif()
# Keep the shared database sources whenever either driver is enabled; only the
# driver-specific translation units and disabled features are omitted.
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_sources
        src/db_client.cpp
        src/db_migration.cpp
        src/db_pool.cpp
        src/db_query.cpp
        src/db_query_compiler.cpp
        src/db_repository.cpp
        src/db_result.cpp
        src/db_schema.cpp
        src/db_session.cpp
        src/db_slot_socket.cpp
        src/db_transaction.cpp
    )
    # The database query-result cache stores rows in Redis.
    if(RUVIA_ENABLE_REDIS)
        list(APPEND ruvia_web_sources src/db_query_cache.cpp)
    endif()
endif()
if(RUVIA_ENABLE_MARIADB)
    list(APPEND ruvia_web_sources
        src/mariadb_driver.cpp
        src/mariadb_prepared.cpp
        src/mariadb_sql_text.cpp
    )
endif()
if(RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_sources src/postgresql_driver.cpp)
endif()
if(RUVIA_ENABLE_REDIS)
    list(APPEND ruvia_web_sources
        src/redis_client.cpp
        src/redis_codec.cpp
        src/redis_orm.cpp
        src/redis_pool.cpp
        src/redis_session.cpp
        src/redis_subscription.cpp
    )
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
    include/callback_ref.h
    include/context.h
    include/controller.h
    include/entity_storage.h
    include/env.h
    include/fixed_string.h
    include/http_client.h
    include/http_client_types.h
    include/http_error.h
    include/http_server.h
    include/http_tunnel.h
    include/integer_parse.h
    include/json_codec.h
    include/middleware.h
    include/model.h
    include/rate_limiting.h
    include/request_stream.h
    include/router.h
    include/security_middleware.h
    include/server_connection.h
    include/server_observation.h
    include/session.h
    include/static_root.h
    include/streaming.h
    include/tcp_client.h
    include/test_app.h
    include/tls_config.h
    include/websocket.h
    include/worker_state.h
)
if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    list(APPEND ruvia_web_headers
        include/db.h
        include/db_entity.h
        include/db_entity_codec.h
        include/db_migration.h
        include/db_query.h
        include/db_schema.h
        include/db_session.h
        include/db_sql_scan.h
    )
endif()
if(RUVIA_ENABLE_REDIS)
    list(APPEND ruvia_web_headers
        include/redis.h
        include/redis_session.h
    )
endif()
if(RUVIA_ENABLE_JWT)
    list(APPEND ruvia_web_headers include/jwt.h)
endif()
ruvia_install_public_headers(web ${ruvia_web_headers})

