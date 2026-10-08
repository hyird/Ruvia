#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"

#include "context/ContextResponseState.h"
#include "context/ContextServices.h"
#include "context/ContextSessionState.h"
#include "http/RequestQueryValues.h"

namespace ruvia::detail {

struct RequestFieldCache final {
    RequestFieldCache(std::pmr::vector<std::pmr::string>&& ownedStorage,
        RequestNameValueList&& ownedFields) noexcept
        : storage(std::move(ownedStorage)),
          fields(std::move(ownedFields)) {}

    std::pmr::vector<std::pmr::string> storage;
    RequestNameValueList fields;
};

// One arena object owns every request-local Context value that is not a
// borrowed pointer: lazy caches, response/session machines, and typed bindings.
class ContextRequestStorage final {
public:
    ContextRequestStorage(ContextServices service_view, std::pmr::memory_resource* resource)
        : services(service_view),
          responseState(resource),
          sessionState(resource) {}

    ContextRequestStorage(const ContextRequestStorage&) = delete;
    ContextRequestStorage& operator=(const ContextRequestStorage&) = delete;

    ContextServices services;
    ContextResponseState responseState;
    ContextSessionState sessionState;
    RequestBindings requestBindings;

    std::optional<std::pmr::string> decodedBody;
    std::optional<RequestNameValueList> headers;
    std::optional<RequestQueryCache> query;
    std::optional<RequestNameValueList> cookies;
    std::optional<RequestFieldCache> routeParams;
    // Monotonic: survives buffered error construction by middleware after the
    // handshake has started, when returning to HTTP response mode is impossible.
    bool webSocketHandshakeStarted{false};
    bool tunnelHandshakeStarted{false};
    bool queryInvalid{false};
    bool routeParamsInvalid{false};
};

}  // namespace ruvia::detail
