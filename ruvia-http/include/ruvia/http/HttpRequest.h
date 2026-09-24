#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/Attributes.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpParseError.h"
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

    // Releases owned header descriptors and clears all borrowed message views.
    // Useful when a runtime reuses a request object before its input storage or
    // PMR resource is retired.
    void reset() noexcept {
        headers_ = {};
        method_ = {};
        knownMethod_ = HttpKnownMethod::kUnknown;
        target_ = {};
        scheme_ = {};
        authority_ = {};
        path_ = {};
        queryString_ = {};
        protocolVersion_ = HttpProtocolVersion::kHttp11;
        targetForm_ = HttpRequestTargetForm::kOrigin;
        cachedHeaders_.fill(0);
        body_ = {};
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

    [[nodiscard]] std::span<const std::byte> bodyBytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    [[nodiscard]] std::span<const std::byte> bodyBytes() const&& = delete;

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

    static constexpr std::size_t kCachedHeaderSlots = 29;

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

// Constructs a semantic request directly, without parsing a wire-format head.
// Header names/values and body bytes are borrowed from the caller; the exact
// header span count is used to allocate descriptors from `resource`. The caller
// must keep all supplied text/body storage and the resource alive while the
// returned request is in use. Invalid method, target, or header fields are
// reported as HttpParseError; body framing fields are intentionally not
// interpreted because this API represents an already-parsed request.
[[nodiscard]] std::pair<HttpRequest, std::optional<HttpParseError>> makeParsedHttpRequest(
    std::string_view method, std::string_view target, std::span<const HttpHeaderView> headers,
    std::span<const std::byte> body, std::pmr::memory_resource* resource);

}  // namespace ruvia
