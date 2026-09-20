#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/Attributes.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/request/HttpRequestHeaderBlock.h"

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
    // Owns the compact descriptor block; field text still borrows protocol input.
    // The block's PMR resource must outlive the request. Moving transfers it.
    HttpRequest(const HttpRequest&) = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;
    HttpRequest(HttpRequest&&) noexcept = default;
    HttpRequest& operator=(HttpRequest&&) noexcept = default;

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
    [[nodiscard]] std::span<const HttpHeaderView> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_.fields();
    }
    [[nodiscard]] std::span<const HttpHeaderView> headers() const&& = delete;

    // Case-insensitive semantic lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
    // Exact raw lookup over the encoded query string. Both `rawName` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> lastRawQueryValue(
        std::string_view rawName) const noexcept;
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
    detail::HttpRequestHeaderBlock headers_{};
    // One-based indices; zero means absent. No duplicated string views.
    std::array<std::uint8_t, kCachedHeaderSlots> cachedHeaders_{};
    std::span<const std::byte> body_{};
    std::pmr::memory_resource* resource_{nullptr};
};

// Header descriptors live in the caller-selected PMR resource rather than the
// request's coroutine frame. The protocol field-count limit is independent of
// this object's layout.

}  // namespace ruvia
