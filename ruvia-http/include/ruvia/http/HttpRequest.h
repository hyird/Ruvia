#pragma once

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolVersion.h"

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

namespace ruvia {

enum class HttpRequestTargetForm : std::uint8_t {
    kOrigin,
    kAbsolute,
    kAuthority,
    kAsterisk,
    // HTTP/2 carries target components as pseudo-fields rather than one
    // request-target token, so none of the HTTP/1 wire forms applies.
    kHttp2,
};

class HttpRequest;

namespace detail {

struct HttpRequestAccess;

}  // namespace detail

class HttpRequest final {
public:
    [[nodiscard]] std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept {
        return knownMethod_;
    }

    [[nodiscard]] std::string_view target() const noexcept {
        return target_;
    }

    [[nodiscard]] std::string_view scheme() const noexcept {
        return scheme_;
    }

    [[nodiscard]] std::string_view authority() const noexcept {
        return authority_;
    }

    [[nodiscard]] HttpRequestTargetForm targetForm() const noexcept {
        return targetForm_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view queryString() const noexcept {
        return queryString_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    // Semantic regular fields, not a byte-for-byte copy of the wire header
    // block. HTTP/1 preserves accepted field-name spelling but removes field
    // framing/OWS and replaces a conflicting Host value for absolute- or
    // authority-form targets. HTTP/2 names are lowercase, pseudo-fields are
    // exposed through method()/scheme()/authority()/target(), split Cookie
    // fields are coalesced, and Host is synthesized from a valid :authority
    // when absent. Repeated regular fields retain wire order.
    [[nodiscard]] std::span<const HttpHeaderView> headers() const& noexcept {
        return std::span<const HttpHeaderView>(headers_.data(), headerCount_);
    }
    [[nodiscard]] std::span<const HttpHeaderView> headers() const&& = delete;

    // Case-insensitive semantic lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
    // Exact raw lookup over the encoded query string. Both `rawName` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> lastRawQueryValue(std::string_view rawName) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;

private:
    friend struct detail::HttpRequestAccess;

    static constexpr std::size_t kCachedHeaderSlots = 25;

    HttpRequest() noexcept = default;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    std::string_view method_;
    HttpKnownMethod knownMethod_{HttpKnownMethod::kUnknown};
    std::string_view target_;
    std::string_view scheme_;
    std::string_view authority_;
    std::string_view path_;
    std::string_view queryString_;
    HttpProtocolVersion protocolVersion_{HttpProtocolVersion::kHttp11};
    HttpRequestTargetForm targetForm_{HttpRequestTargetForm::kOrigin};
    std::array<HttpHeaderView, kMaxHttpHeaderFields> headers_{};
    std::size_t headerCount_{0};
    std::uint32_t cachedHeaderBits_{0};
    std::array<std::string_view, kCachedHeaderSlots> cachedHeaders_{};
    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

// Every member is trivially copyable, so a move of HttpRequest is a full
// memcpy, and construction value-initializes both fixed arrays. That cost is
// paid once per HTTP/1 request and once per in-flight HTTP/2 stream, whose
// coroutine frame carries the request for the stream's whole lifetime -- a
// frame above kTaskFrameCacheMaxBlockBytes skips the frame cache entirely.
// The bulk is headers_ (kMaxHttpHeaderFields * sizeof(HttpHeaderView)) plus
// cachedHeaders_. Weigh that before adding a field or raising either bound.
static_assert(sizeof(HttpRequest) <= 2624);

}  // namespace ruvia
