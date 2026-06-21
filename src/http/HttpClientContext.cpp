#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "ruvia/http/Context.h"
#include "client/HttpClientInternal.h"
#include "client/HttpClientPool.h"

namespace ruvia {

Task<FetchResponse> Context::fetch(
    std::string_view alias,
    std::string_view path,
    FetchOptions options) {
    if (httpClients_ == nullptr) {
        throw std::logic_error(
            "no http client registered; call App::useHttpClient before run()");
    }
    auto* pool = httpClients_->get(alias);
    if (pool == nullptr) {
        throw std::logic_error("http client alias not found");
    }
    co_return co_await pool->fetch(path, options, resource());
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_HTTP_CLIENT
