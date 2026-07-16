#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"

namespace ruvia::detail {

class Http1BufferedResponseWriteResult;

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
private:
    friend class Http1BufferedResponseWriteResult;
    constexpr Http1BufferedResponseWriteFailedBeforeCommit() noexcept = default;
};

class Http1BufferedResponseWriteFailedAfterCommit final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http1BufferedResponseWriteResult;

    explicit constexpr Http1BufferedResponseWriteFailedAfterCommit(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

// The complete-head byte boundary produces exactly one terminal alternative.
// Only alternatives reached after a complete head own a committed status, so a
// failed-before-commit result cannot carry meaningless status storage.
class Http1BufferedResponseWriteResult final {
public:
    [[nodiscard]] constexpr const Http1BufferedResponseWriteCompleted*
    completed() const & noexcept {
        return std::get_if<Http1BufferedResponseWriteCompleted>(&value_);
    }
    const Http1BufferedResponseWriteCompleted* completed() const && = delete;

    [[nodiscard]] constexpr const Http1BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const & noexcept {
        return std::get_if<Http1BufferedResponseWriteFailedBeforeCommit>(
            &value_);
    }
    const Http1BufferedResponseWriteFailedBeforeCommit*
    failedBeforeCommit() const && = delete;

    [[nodiscard]] constexpr const Http1BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const & noexcept {
        return std::get_if<Http1BufferedResponseWriteFailedAfterCommit>(&value_);
    }
    const Http1BufferedResponseWriteFailedAfterCommit*
    failedAfterCommit() const && = delete;

    [[nodiscard]] constexpr std::optional<std::uint16_t>
    committedStatus() const noexcept {
        if (const auto* value = completed()) {
            return value->status();
        }
        if (const auto* value = failedAfterCommit()) {
            return value->status();
        }
        return std::nullopt;
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

    [[nodiscard]] static constexpr Http1BufferedResponseWriteResult
    makeCompleted(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteCompleted(status));
    }

    [[nodiscard]] static constexpr Http1BufferedResponseWriteResult
    makeFailedBeforeCommit() noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteFailedBeforeCommit());
    }

    [[nodiscard]] static constexpr Http1BufferedResponseWriteResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteFailedAfterCommit(status));
    }

    template <typename Alternative>
    explicit constexpr Http1BufferedResponseWriteResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    Value value_;
};

static_assert(std::is_trivially_copyable_v<Http1BufferedResponseWriteResult>);
static_assert(sizeof(Http1BufferedResponseWriteResult) <= 4);

// `bytesTransferred` is the composed async-write prefix accepted before `error`.
// Only a prefix containing the entire serialized head commits a final status.
[[nodiscard]] inline Http1BufferedResponseWriteResult
classifyHttp1BufferedResponseWrite(
    const Http1BufferedResponsePlan& plan,
    std::size_t responseHeadBytes,
    std::error_code error,
    std::size_t bytesTransferred) noexcept {
    const auto status = plan.responseStatus();
    if (!error) {
        if (responseHeadBytes == 0 ||
            bytesTransferred < responseHeadBytes) {
            return Http1BufferedResponseWriteResult::makeFailedBeforeCommit();
        }
        return Http1BufferedResponseWriteResult::makeCompleted(status);
    }
    if (responseHeadBytes != 0 &&
        bytesTransferred >= responseHeadBytes) {
        return Http1BufferedResponseWriteResult::makeFailedAfterCommit(status);
    }
    return Http1BufferedResponseWriteResult::makeFailedBeforeCommit();
}

}  // namespace ruvia::detail
