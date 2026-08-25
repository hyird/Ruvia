#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientTypes.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientRegistry;
}

class Context;

class HttpClientHandle final : private detail::ScopedCapabilityNode {
public:
    HttpClientHandle(const HttpClientHandle& other);
    HttpClientHandle& operator=(const HttpClientHandle&) = delete;

    [[nodiscard]] HttpClientHandle withOptions(OperationOptions options) const;
    [[nodiscard]] ScopedOperation<HttpClientResponse> send(const HttpClientRequestView& request) const;
    [[nodiscard]] HttpClientStats stats() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] HttpScheme scheme() const;
private:
    friend class detail::HttpClientRegistry;
    friend class Context;
    friend class WebWorkerContext;
    HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    detail::HttpClientPool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    OperationOptions options_;
};

}  // namespace ruvia
