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

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view queryString() const noexcept {
        return queryString_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] std::span<const HttpHeaderView> headers() const noexcept {
        return std::span<const HttpHeaderView>(headers_.data(), headerCount_);
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;

private:
    friend struct detail::HttpRequestAccess;

    static constexpr std::size_t kCachedHeaderSlots = 25;

    HttpRequest() noexcept = default;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    std::string_view method_;
    HttpKnownMethod knownMethod_{HttpKnownMethod::kUnknown};
    std::string_view target_;
    std::string_view path_;
    std::string_view queryString_;
    HttpProtocolVersion protocolVersion_{HttpProtocolVersion::kHttp11};
    std::array<HttpHeaderView, kMaxHttpHeaderFields> headers_{};
    std::size_t headerCount_{0};
    std::uint32_t cachedHeaderBits_{0};
    std::array<std::string_view, kCachedHeaderSlots> cachedHeaders_{};
    std::string_view body_;
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia
