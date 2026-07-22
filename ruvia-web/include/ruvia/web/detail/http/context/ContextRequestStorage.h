#pragma once

#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/http/request/RequestQueryValues.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ruvia::detail {

// One typed owner for every lazily materialized Context request cache. Context
// allocates this aggregate once from RequestMemory; individual values remain
// explicit alternatives and require neither erased cleanup callbacks nor one
// arena allocation per C++ object.
struct RequestFieldCache final {
    RequestFieldCache(
        std::pmr::vector<std::pmr::string>&& ownedStorage,
        RequestNameValueList&& ownedFields) noexcept
        : storage(std::move(ownedStorage)),
          fields(std::move(ownedFields)) {}

    std::pmr::vector<std::pmr::string> storage;
    RequestNameValueList fields;
};

class ContextRequestStorage final {
public:
    ContextRequestStorage() = default;

    ContextRequestStorage(const ContextRequestStorage&) = delete;
    ContextRequestStorage& operator=(const ContextRequestStorage&) = delete;

    std::optional<std::pmr::string> decodedBody;
    std::optional<RequestFieldCache> headers;
    std::optional<RequestQueryCache> query;
    std::optional<RequestNameValueList> cookies;
    std::optional<RequestFieldCache> routeParams;
    // Malformed percent encoding is terminal for the corresponding typed cache.
    // Remember it so repeated API calls cannot rescan attacker-controlled input.
    bool queryInvalid{false};
    bool routeParamsInvalid{false};
};

}  // namespace ruvia::detail
