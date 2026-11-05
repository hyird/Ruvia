# vcpkg's static libpq splits frontend symbols between the cyclic
# pgcommon/pgport pair, which GNU linkers must rescan. The archive paths belong
# to whichever environment ran find_package(PostgreSQL): the Ruvia build uses
# this for its build tree and ruviaConfig.cmake reruns it after the consumer's
# own find_dependency(PostgreSQL), so no installed file names a build path.
# The result is empty when that environment has no static pgcommon/pgport split.
function(ruvia_postgresql_link_group out)
    set(_ruvia_link_group)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND PostgreSQL_pgcommon_LIBRARY_RELEASE AND PostgreSQL_pgport_LIBRARY_RELEASE)
        set(_ruvia_pgcommon_debug "${PostgreSQL_pgcommon_LIBRARY_DEBUG}")
        if(NOT _ruvia_pgcommon_debug)
            set(_ruvia_pgcommon_debug "${PostgreSQL_pgcommon_LIBRARY_RELEASE}")
        endif()
        set(_ruvia_pgport_debug "${PostgreSQL_pgport_LIBRARY_DEBUG}")
        if(NOT _ruvia_pgport_debug)
            set(_ruvia_pgport_debug "${PostgreSQL_pgport_LIBRARY_RELEASE}")
        endif()
        set(_ruvia_link_group
            "$<LINK_GROUP:RESCAN,$<$<CONFIG:Debug>:${_ruvia_pgcommon_debug}>$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgcommon_LIBRARY_RELEASE}>,$<$<CONFIG:Debug>:${_ruvia_pgport_debug}>$<$<NOT:$<CONFIG:Debug>>:${PostgreSQL_pgport_LIBRARY_RELEASE}>>")
    endif()
    set(${out} "${_ruvia_link_group}" PARENT_SCOPE)
endfunction()
