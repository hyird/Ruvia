#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/http/context/ContextResponseState.h"
#include "ruvia/web/detail/http/context/ContextSessionState.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"
#include "ruvia/web/detail/http/request/RequestQueryValues.h"

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
    ContextRequestStorage(ContextRequestBodySource bodySource, ContextResponseOutput output,
        std::pmr::memory_resource* resource)
        : requestBodySource(bodySource),
          responseOutput(output),
          responseState(resource),
          sessionState(resource) {}

    ContextRequestStorage(const ContextRequestStorage&) = delete;
    ContextRequestStorage& operator=(const ContextRequestStorage&) = delete;

    ContextRequestBodySource requestBodySource;
    ContextResponseOutput responseOutput;
    ContextResponseState responseState;
    ContextSessionState sessionState;
    RequestBindings requestBindings;

    std::optional<std::pmr::string> decodedBody;
    std::optional<RequestFieldCache> headers;
    std::optional<RequestQueryCache> query;
    std::optional<RequestNameValueList> cookies;
    std::optional<RequestFieldCache> routeParams;
    bool queryInvalid{false};
    bool routeParamsInvalid{false};
};

}  // namespace ruvia::detail
