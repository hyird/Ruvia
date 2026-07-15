#pragma once

#include "ruvia/web/RequestFields.h"
#include "ruvia/web/detail/http/RequestQueryValues.h"

#include <list>
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
    explicit ContextRequestStorage(std::pmr::memory_resource* resource)
        : decodedValues_(resource) {}

    ContextRequestStorage(const ContextRequestStorage&) = delete;
    ContextRequestStorage& operator=(const ContextRequestStorage&) = delete;

    std::optional<std::pmr::string> decodedBody;
    std::optional<RequestFieldCache> headers;
    std::optional<RequestQueryCache> query;
    std::optional<RequestNameValueList> cookies;
    std::optional<RequestFieldCache> routeParams;
    [[nodiscard]] std::pmr::string& appendDecodedValue() {
        return decodedValues_.emplace_back();
    }

private:
    // list preserves every returned string_view across later lazy decodes.
    std::pmr::list<std::pmr::string> decodedValues_;
};

}  // namespace ruvia::detail
