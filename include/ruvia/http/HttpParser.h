#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpParseTypes.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia {

namespace detail {

struct HttpParseResultAccess;

}  // namespace detail

class HttpParseResult final {
public:
    [[nodiscard]] HttpParseStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] HttpParseError error() const noexcept {
        return error_;
    }

    [[nodiscard]] const HttpRequest& request() const noexcept {
        return request_;
    }

    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend struct detail::HttpParseResultAccess;

    HttpParseResult(
        HttpParseStatus status,
        HttpParseError error,
        HttpRequest request,
        std::size_t consumedBytes) noexcept
        : status_(status),
          error_(error),
          request_(std::move(request)),
          consumedBytes_(consumedBytes) {}

    HttpParseStatus status_{HttpParseStatus::kIncomplete};
    HttpParseError error_{HttpParseError::kNone};
    HttpRequest request_;
    std::size_t consumedBytes_{0};
};

namespace detail {

struct HttpParseResultAccess final {
    [[nodiscard]] static HttpParseResult make(
        HttpParseStatus status,
        HttpParseError error,
        HttpRequest request,
        std::size_t consumedBytes) noexcept {
        return HttpParseResult(status, error, std::move(request), consumedBytes);
    }

    [[nodiscard]] static HttpRequest& request(HttpParseResult& result) noexcept {
        return result.request_;
    }
};

}  // namespace detail

class HttpParser final {
public:
    [[nodiscard]] HttpParseResult parse(std::string_view buffer) const noexcept;
};

}  // namespace ruvia
