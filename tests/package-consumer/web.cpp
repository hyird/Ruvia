#include <ruvia/web/App.h>
#include <ruvia/web/AppHook.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/HttpServerOptions.h>
#include <ruvia/web/MiddlewareRuntime.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/RouteModes.h>
#include <ruvia/web/detail/ContextValues.h>
#include <ruvia/web/detail/ValidatedValues.h>
#include <ruvia/web/detail/model/Parser.h>

#ifdef RUVIA_ENABLE_JWT
#include <ruvia/web/auth/Jwt.h>
#endif
#ifdef RUVIA_ENABLE_MARIADB
#include <ruvia/web/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/Redis.h>
#endif

int main() {
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
