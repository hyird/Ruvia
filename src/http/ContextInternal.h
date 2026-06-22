#pragma once

#include "ruvia/http/Context.h"

#include <array>
#include <cstddef>

namespace ruvia::detail {

struct ContextServices final {
    DbRegistry* db{nullptr};
    RedisRegistry* redis{nullptr};
    BodyReader* bodyReader{nullptr};
    RequestBodyLoader* bodyLoader{nullptr};
    ResponseStreamWriter* responseStream{nullptr};
    WebSocket* webSocket{nullptr};
    HttpClientRegistry* httpClients{nullptr};
};

}  // namespace ruvia::detail

namespace ruvia {

inline Context::Context(
    RequestMemory& memory,
    const HttpRequest& request,
    detail::ContextServices services) noexcept
    : memory_(memory),
      request_(request),
      db_(services.db),
      redis_(services.redis),
      httpClients_(services.httpClients),
      bodyReader_(services.bodyReader),
      bodyLoader_(services.bodyLoader),
      webSocket_(services.webSocket),
      responseStream_(services.responseStream),
      responseStatusText_(memory.resource()),
      responseHeaders_(memory.resource()) {}

inline Context::Context(
    RequestMemory& memory,
    const HttpRequest& request,
    const std::array<RouteParamView, kMaxRouteParams>& params,
    std::size_t paramCount,
    detail::ContextServices services) noexcept
    : memory_(memory),
      request_(request),
      params_(params.data()),
      paramCount_(paramCount < kMaxRouteParams ? paramCount : kMaxRouteParams),
      db_(services.db),
      redis_(services.redis),
      httpClients_(services.httpClients),
      bodyReader_(services.bodyReader),
      bodyLoader_(services.bodyLoader),
      webSocket_(services.webSocket),
      responseStream_(services.responseStream),
      responseStatusText_(memory.resource()),
      responseHeaders_(memory.resource()) {}

}  // namespace ruvia

namespace ruvia::detail {

struct ContextAccess final {
    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        ContextServices services = {}) noexcept {
        return Context(memory, request, services);
    }

    [[nodiscard]] static Context make(
        RequestMemory& memory,
        const HttpRequest& request,
        const std::array<RouteParamView, kMaxRouteParams>& params,
        std::size_t paramCount,
        ContextServices services = {}) noexcept {
        return Context(memory, request, params, paramCount, services);
    }
};

}  // namespace ruvia::detail
