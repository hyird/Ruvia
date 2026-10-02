#pragma once

#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia {
namespace detail {
class HttpClientResponseState;
}

// Immutable metadata retained by its HttpClientResponse. Field views borrow
// that response and remain valid through subsequent body reads.
class HttpClientInformationalResponse final {
public:
    HttpClientInformationalResponse(const HttpClientInformationalResponse&) = delete;
    HttpClientInformationalResponse& operator=(const HttpClientInformationalResponse&) = delete;
    HttpClientInformationalResponse(HttpClientInformationalResponse&&) noexcept = default;
    HttpClientInformationalResponse& operator=(HttpClientInformationalResponse&&) = delete;
    [[nodiscard]] HttpStatusCode status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::span<const HttpHeader> headers() const& noexcept {
        return headers_;
    }
    std::span<const HttpHeader> headers() const&& = delete;

private:
    friend class detail::HttpClientResponseState;
    HttpClientInformationalResponse(HttpStatusCode status, std::pmr::memory_resource* resource)
        : status_(status),
          headers_(resource) {}
    HttpStatusCode status_;
    std::pmr::vector<HttpHeader> headers_;
};
}  // namespace ruvia
