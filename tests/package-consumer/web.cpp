#include <ruvia/app/App.h>
#include <ruvia/app/AppHook.h>
#include <ruvia/http/Controller.h>
#include <ruvia/http/HttpClientRuntime.h>
#include <ruvia/http/HttpServerOptions.h>
#include <ruvia/http/MiddlewareRuntime.h>
#include <ruvia/http/Model.h>
#include <ruvia/http/RouteModes.h>
#include <ruvia/http/detail/ContextValues.h>
#include <ruvia/http/detail/ValidatedValues.h>
#include <ruvia/http/detail/model/Parser.h>

#ifdef RUVIA_ENABLE_JWT
#include <ruvia/auth/Jwt.h>
#endif
#ifdef RUVIA_ENABLE_MARIADB
#include <ruvia/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/redis/Redis.h>
#endif

int main() {
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
