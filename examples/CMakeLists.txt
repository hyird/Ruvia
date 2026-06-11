function(ruvia_add_example name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ruvia::ruvia)
    target_compile_definitions(${name}
        PRIVATE
            RUVIA_EXAMPLES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

ruvia_add_example(ruvia_example_basic_http basic_http.cpp)
ruvia_add_example(ruvia_example_api_surface api_surface.cpp)
ruvia_add_example(ruvia_example_models_validation models_validation.cpp)
ruvia_add_example(ruvia_example_streaming streaming.cpp)
ruvia_add_example(ruvia_example_files_static files_static.cpp)
ruvia_add_example(ruvia_example_websocket websocket.cpp)
ruvia_add_example(ruvia_example_runtime_config runtime_config.cpp)

if(RUVIA_ENABLE_JWT)
    ruvia_add_example(ruvia_example_auth_jwt auth_jwt.cpp)
endif()

if(RUVIA_ENABLE_MARIADB)
    ruvia_add_example(ruvia_example_database database.cpp)
endif()

if(RUVIA_ENABLE_REDIS)
    ruvia_add_example(ruvia_example_redis redis.cpp)
endif()
