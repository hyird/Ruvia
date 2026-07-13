#pragma once

#include "ruvia/http/detail/HttpResponseFileBody.h"
#include "ruvia/core/Task.h"

#include <asio/ip/tcp.hpp>
#include <system_error>
#include <utility>
#include <variant>

namespace ruvia::detail {

class HttpFileZeroCopyCompleted final {};
class HttpFileZeroCopyUnavailable final {};

class HttpFileZeroCopyFailed final {
public:
    [[nodiscard]] const std::error_code& error() const noexcept {
        return error_;
    }

private:
    friend class HttpFileZeroCopyResult;

    explicit HttpFileZeroCopyFailed(std::error_code error) noexcept
        : error_(error) {}

    std::error_code error_;
};

// A zero-copy attempt has three exclusive outcomes. Unavailable means the
// current platform has no native implementation and the caller may use the
// buffered file path; a real open/socket failure must never masquerade as that
// capability decision.
class HttpFileZeroCopyResult final {
public:
    [[nodiscard]] const HttpFileZeroCopyCompleted* completed()
        const noexcept {
        return std::get_if<HttpFileZeroCopyCompleted>(&value_);
    }

    [[nodiscard]] const HttpFileZeroCopyUnavailable* unavailable()
        const noexcept {
        return std::get_if<HttpFileZeroCopyUnavailable>(&value_);
    }

    [[nodiscard]] const HttpFileZeroCopyFailed* failed() const noexcept {
        return std::get_if<HttpFileZeroCopyFailed>(&value_);
    }

private:
    friend Task<HttpFileZeroCopyResult> writeFileZeroCopy(
        asio::ip::tcp::socket&,
        ResponseFileBody);

    using Value = std::variant<
        HttpFileZeroCopyCompleted,
        HttpFileZeroCopyUnavailable,
        HttpFileZeroCopyFailed>;

    [[nodiscard]] static HttpFileZeroCopyResult makeCompleted() noexcept {
        return HttpFileZeroCopyResult(HttpFileZeroCopyCompleted{});
    }

    [[nodiscard]] static HttpFileZeroCopyResult makeUnavailable() noexcept {
        return HttpFileZeroCopyResult(HttpFileZeroCopyUnavailable{});
    }

    [[nodiscard]] static HttpFileZeroCopyResult makeFailed(
        std::error_code error) noexcept {
        return HttpFileZeroCopyResult(HttpFileZeroCopyFailed(error));
    }

    template <typename Alternative>
    explicit HttpFileZeroCopyResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

Task<HttpFileZeroCopyResult> writeFileZeroCopy(
    asio::ip::tcp::socket& socket,
    ResponseFileBody file);

}  // namespace ruvia::detail
