#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include "ruvia/http/Attributes.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {
struct HttpClientResponseHeadAccess;
}  // namespace ruvia::detail

namespace ruvia {

// A response-side signal for an external request-body writer. The same signal
// is emitted by HTTP/1 response plans and HTTP/2 response events.
enum class HttpClientRequestContentSignal : std::uint8_t {
    kContinue,
    kExchangeComplete,
};

class HttpClientResponseHead final {
public:
    HttpClientResponseHead(const HttpClientResponseHead&) = delete;
    HttpClientResponseHead& operator=(const HttpClientResponseHead&) = delete;
    HttpClientResponseHead(HttpClientResponseHead&&) noexcept = default;
    HttpClientResponseHead& operator=(HttpClientResponseHead&&) = delete;

    [[nodiscard]] HttpStatusCode status() const noexcept {
        return status_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] std::span<const HttpHeader> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    [[nodiscard]] std::span<const HttpHeader> headers() const&& = delete;

    // Transfers the owned parsed fields to a consumer that retains the head's
    // metadata but needs to extend the fields' lifetime independently.
    [[nodiscard]] std::pmr::vector<HttpHeader> takeHeaders() && noexcept {
        return std::move(headers_);
    }

private:
    friend struct detail::HttpClientResponseHeadAccess;

    HttpClientResponseHead(HttpStatusCode status, HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource)
        : HttpClientResponseHead(detail::HttpResolvedPmrResourceTag{}, status, protocolVersion,
              detail::httpPmrResourceOrDefault(resource)) {}

    HttpClientResponseHead(detail::HttpResolvedPmrResourceTag, HttpStatusCode status,
        HttpProtocolVersion protocolVersion, std::pmr::memory_resource* resource)
        : status_(status),
          protocolVersion_(protocolVersion),
          headers_(resource) {}

    HttpStatusCode status_;
    HttpProtocolVersion protocolVersion_;
    std::pmr::vector<HttpHeader> headers_;
};

}  // namespace ruvia
