#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"

namespace ruvia::detail {

class Http1BufferedResponseWriteCompleted final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http1BufferedResponseWriteResult;

    explicit constexpr Http1BufferedResponseWriteCompleted(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

class Http1BufferedResponseWriteFailedBeforeCommit final {
public:
    [[nodiscard]] const std::error_code& error() const noexcept {
        return error_;
    }

private:
    friend class Http1BufferedResponseWriteResult;

    explicit Http1BufferedResponseWriteFailedBeforeCommit(
        std::error_code error) noexcept
        : error_(error) {}

    std::error_code error_;
};

class Http1BufferedResponseWriteFailedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::error_code& error() const noexcept {
        return error_;
    }

private:
    friend class Http1BufferedResponseWriteResult;

    Http1BufferedResponseWriteFailedAfterCommit(
        std::uint16_t status,
        std::error_code error) noexcept
        : status_(status),
          error_(error) {}

    std::uint16_t status_;
    std::error_code error_;
};

// A buffered HTTP/1 write has exactly one terminal outcome. A final response
// status exists only after the complete response head has reached the transport;
// a partial head is not an HTTP response and therefore carries no status. The
// committed alternatives obtain their status from the exact write plan used to
// serialize the head rather than reconstructing it from a mutable response.
class Http1BufferedResponseWriteResult final {
public:
    [[nodiscard]] const Http1BufferedResponseWriteCompleted*
    completed() const noexcept {
        return std::get_if<Http1BufferedResponseWriteCompleted>(&value_);
    }

    [[nodiscard]] const Http1BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const noexcept {
        return std::get_if<Http1BufferedResponseWriteFailedBeforeCommit>(
            &value_);
    }

    [[nodiscard]] const Http1BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const noexcept {
        return std::get_if<Http1BufferedResponseWriteFailedAfterCommit>(
            &value_);
    }

private:
    friend Http1BufferedResponseWriteResult
    classifyHttp1BufferedResponseWrite(
        const Http1BufferedResponsePlan&,
        std::size_t,
        std::error_code,
        std::size_t) noexcept;

    using Value = std::variant<
        Http1BufferedResponseWriteCompleted,
        Http1BufferedResponseWriteFailedBeforeCommit,
        Http1BufferedResponseWriteFailedAfterCommit>;

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeCompleted(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteCompleted(status));
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedBeforeCommit(std::error_code error) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteFailedBeforeCommit(error));
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedAfterCommit(
        std::uint16_t status,
        std::error_code error) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteFailedAfterCommit(status, error));
    }

    template <typename Alternative>
    explicit Http1BufferedResponseWriteResult(
        Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

// `bytesTransferred` is the composed async-write prefix accepted before `error`.
// Only a prefix containing the entire serialized head commits a final status.
[[nodiscard]] inline Http1BufferedResponseWriteResult
classifyHttp1BufferedResponseWrite(
    const Http1BufferedResponsePlan& plan,
    std::size_t responseHeadBytes,
    std::error_code error,
    std::size_t bytesTransferred) noexcept {
    const auto status = plan.writePlan().responseStatus();
    if (!error) {
        if (responseHeadBytes == 0 ||
            bytesTransferred < responseHeadBytes) {
            return Http1BufferedResponseWriteResult::makeFailedBeforeCommit(
                std::make_error_code(std::errc::io_error));
        }
        return Http1BufferedResponseWriteResult::makeCompleted(status);
    }
    if (responseHeadBytes != 0 &&
        bytesTransferred >= responseHeadBytes) {
        return Http1BufferedResponseWriteResult::makeFailedAfterCommit(
            status,
            error);
    }
    return Http1BufferedResponseWriteResult::makeFailedBeforeCommit(error);
}

}  // namespace ruvia::detail
