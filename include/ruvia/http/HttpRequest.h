#pragma once

#include "ruvia/http/HttpCommon.h"

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ruvia {

class HttpRequest;

namespace detail {

struct HttpRequestAccess;

}  // namespace detail

class HttpRequest final {
public:
    [[nodiscard]] HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view target() const noexcept {
        return target_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::optional<std::pmr::string> decodedPath() const;

    [[nodiscard]] std::string_view queryString() const noexcept {
        return queryString_;
    }

    [[nodiscard]] std::string_view httpVersion() const noexcept {
        return httpVersion_;
    }

    [[nodiscard]] std::span<const HttpHeaderView> headers() const noexcept {
        return std::span<const HttpHeaderView>(headers_.data(), headerCount_);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] QueryValue query(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    // The verified client certificate subject DN for a mutual-TLS connection,
    // or empty when no client certificate was presented (or no TLS).
    [[nodiscard]] std::string_view clientCertificate() const noexcept {
        return clientCertificate_;
    }

    // True when the request arrived over a TLS connection (https / h2).
    [[nodiscard]] bool isSecure() const noexcept {
        return secure_;
    }

private:
    friend struct detail::HttpRequestAccess;

    static constexpr std::size_t kCachedHeaderSlots = 24;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    HttpMethod method_{HttpMethod::kUnknown};
    std::string_view target_;
    std::string_view path_;
    std::string_view queryString_;
    std::string_view httpVersion_{"HTTP/1.1"};
    std::array<HttpHeaderView, kMaxRequestHeaders> headers_{};
    std::size_t headerCount_{0};
    std::uint32_t cachedHeaderBits_{0};
    std::array<std::string_view, kCachedHeaderSlots> cachedHeaders_{};
    std::string_view body_;
    std::string_view remoteAddress_;
    std::string_view clientCertificate_;
    bool secure_{false};
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia
