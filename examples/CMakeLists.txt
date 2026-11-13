# Each executable is independent. Enable optional backends at configure time.
add_custom_target(ruvia_examples_web)

function(ruvia_add_example name)
    add_executable(${name} ${ARGN})
    add_dependencies(ruvia_examples_web ${name})
    target_link_libraries(${name} PRIVATE ruvia::web)
    if(MSVC)
        target_compile_options(${name} PRIVATE /bigobj /FS)
    endif()
endfunction()

ruvia_add_example(ruvia_example_basic_http web/basic_http.cpp)
ruvia_add_example(ruvia_example_api_surface web/api_surface.cpp)
ruvia_add_example(ruvia_example_models_validation web/models_validation.cpp)
ruvia_add_example(ruvia_example_streaming web/streaming.cpp)
ruvia_add_example(ruvia_example_files_static web/files_static.cpp)
target_compile_definitions(ruvia_example_files_static PRIVATE
    RUVIA_EXAMPLES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")
ruvia_add_example(ruvia_example_websocket web/websocket.cpp)
ruvia_add_example(ruvia_example_http_client web/http_client.cpp)
ruvia_add_example(ruvia_example_runtime_config web/runtime_config.cpp)
ruvia_add_example(ruvia_example_testing web/testing.cpp)
ruvia_add_example(ruvia_example_request_policies web/request_policies.cpp)

if(RUVIA_ENABLE_JWT)
    ruvia_add_example(ruvia_example_auth_jwt web/auth_jwt.cpp)
endif()

if(RUVIA_ENABLE_MARIADB OR RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_example(ruvia_example_database web/database.cpp)
    ruvia_add_example(ruvia_example_sql_queries web/sql_queries.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL AND RUVIA_ENABLE_REDIS)
    ruvia_add_example(ruvia_example_event_loop_data web/event_loop_data.cpp)
endif()

if(RUVIA_ENABLE_POSTGRESQL)
    ruvia_add_example(ruvia_example_orm web/orm.cpp)
endif()

if(RUVIA_ENABLE_REDIS)
    ruvia_add_example(ruvia_example_sessions web/sessions.cpp)
    ruvia_add_example(ruvia_example_redis web/redis.cpp)
endif()
