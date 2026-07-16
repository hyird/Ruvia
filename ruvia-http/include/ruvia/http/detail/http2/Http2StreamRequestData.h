#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2HeaderList.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {

class Http2StreamRequestData final {
public:
    explicit Http2StreamRequestData(std::pmr::memory_resource* resource = nullptr)
        : Http2StreamRequestData(HttpResolvedPmrResourceTag{}, httpPmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept {
        return knownMethod_;
    }

    void assignMethod(std::string_view method) {
        method_.assign(method.data(), method.size());
        knownMethod_ = classifyHttpMethod(method);
    }

    [[nodiscard]] std::string_view scheme() const noexcept {
        return scheme_;
    }

    void assignScheme(std::string_view value) {
        scheme_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view authority() const noexcept {
        return authority_;
    }

    void assignAuthority(std::string_view value) {
        authority_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    void assignPath(std::string_view value) {
        path_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view protocol() const noexcept {
        return protocol_;
    }

    void assignProtocol(std::string_view value) {
        protocol_.assign(value.data(), value.size());
    }

    [[nodiscard]] std::string_view cookie() const noexcept {
        return cookie_;
    }

    [[nodiscard]] bool appendCookieHeaderValue(
        std::string_view value,
        bool hasExistingCookie) {
        constexpr std::string_view kCookieSeparator = "; ";
        const auto separatorBytes = hasExistingCookie ? kCookieSeparator.size() : 0;
        if (value.size() > kMaxHttpHeaderBytes ||
            cookie_.size() > kMaxHttpHeaderBytes - separatorBytes ||
            cookie_.size() + separatorBytes > kMaxHttpHeaderBytes - value.size()) {
            return false;
        }

        if (hasExistingCookie) {
            cookie_.append(kCookieSeparator.data(), kCookieSeparator.size());
        }
        if (!value.empty()) {
            cookie_.append(value.data(), value.size());
        }
        return true;
    }

    [[nodiscard]] bool headersFull() const noexcept {
        return headers_.full();
    }

    [[nodiscard]] std::size_t headerCount() const noexcept {
        return headers_.size();
    }

    [[nodiscard]] Http2StoredHeaderView headerAt(std::size_t index) const noexcept {
        return headers_.at(index);
    }

    [[nodiscard]] bool appendHeader(
        std::string_view name,
        std::string_view value,
        RequestHeaderKind kind) {
        return headers_.append(name, value, kind);
    }

private:
    Http2StreamRequestData(HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : method_(resource),
          scheme_(resource),
          authority_(resource),
          path_(resource),
          protocol_(resource),
          cookie_(resource),
          headers_(resource) {}

    std::pmr::string method_;
    HttpKnownMethod knownMethod_{HttpKnownMethod::kUnknown};
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::string path_;
    std::pmr::string protocol_;
    std::pmr::string cookie_;
    Http2HeaderList headers_;
};

}  // namespace ruvia::detail
