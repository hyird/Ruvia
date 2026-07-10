cmake_minimum_required(VERSION 3.25)
project(ruvia_package_consumer LANGUAGES CXX)

set(RUVIA_PACKAGE_COMPONENTS "" CACHE STRING "Comma-separated Ruvia components to consume")
if(RUVIA_PACKAGE_COMPONENTS STREQUAL "")
    message(FATAL_ERROR "RUVIA_PACKAGE_COMPONENTS must name at least one component")
endif()
string(REPLACE "," ";" _ruvia_components "${RUVIA_PACKAGE_COMPONENTS}")

find_package(ruvia CONFIG REQUIRED COMPONENTS ${_ruvia_components})

foreach(_ruvia_component IN LISTS _ruvia_components)
    add_executable(ruvia_consumer_${_ruvia_component} ${_ruvia_component}.cpp)
    target_link_libraries(ruvia_consumer_${_ruvia_component} PRIVATE ruvia::${_ruvia_component})
endforeach()
