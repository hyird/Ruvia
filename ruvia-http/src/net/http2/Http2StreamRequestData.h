#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "Http2HeaderList.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {

class Http2StreamBodyQueue;

class Http2StreamRequestData final {
public:
    explicit Http2StreamRequestData(std::pmr::memory_resource* resource = nullptr)
        : Http2StreamRequestData(HttpResolvedPmrResourceTag{}, httpPmrResourceOrDefault(resource)) {}

    [[nodiscard]] HttpMethod method() const noexcept {
        return method_;
    }

    void setMethod(HttpMethod method) noexcept {
        method_ = method;
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

    [[nodiscard]] std::size_t bodySize() const noexcept {
        return body_.size();
    }

    [[nodiscard]] bool bodyEmpty() const noexcept {
        return body_.empty();
    }

    [[nodiscard]] std::string_view bodyView() const noexcept {
        return body_;
    }

    void appendBody(std::string_view value) {
        body_.append(value.data(), value.size());
    }

    void assignBody(std::string_view value) {
        body_.assign(value.data(), value.size());
    }

    void clearBody() noexcept {
        body_.clear();
    }

    [[nodiscard]] std::pmr::string& responseCompressionScratch() noexcept {
        return body_;
    }

    void moveBodyToQueue(Http2StreamBodyQueue& queue);

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
        : authority_(resource),
          path_(resource),
          cookie_(resource),
          body_(resource),
          headers_(resource) {}

    HttpMethod method_{HttpMethod::kUnknown};
    std::pmr::string authority_;
    std::pmr::string path_;
    std::pmr::string cookie_;
    std::pmr::string body_;
    Http2HeaderList headers_;
};

}  // namespace ruvia::detail
