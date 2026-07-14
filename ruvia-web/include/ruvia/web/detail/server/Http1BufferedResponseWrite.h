#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"

namespace ruvia::detail {

enum class Http1BufferedResponseWriteOutcome : std::uint8_t {
    kCompleted,
    kFailedBeforeCommit,
    kFailedAfterCommit
};

// The writer collapses the transport error and complete-head byte boundary into
// one terminal outcome. A status is committed in exactly two of the three states;
// callers must not reconstruct that state from a completion flag plus an optional.
class Http1BufferedResponseWriteResult final {
public:
    [[nodiscard]] constexpr Http1BufferedResponseWriteOutcome
    outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t>
    committedStatus() const noexcept {
        return outcome_ ==
                Http1BufferedResponseWriteOutcome::kFailedBeforeCommit
            ? std::nullopt
            : std::optional<std::uint16_t>(status_);
    }

private:
    friend Http1BufferedResponseWriteResult
    classifyHttp1BufferedResponseWrite(
        const Http1BufferedResponsePlan&,
        std::size_t,
        std::error_code,
        std::size_t) noexcept;

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeCompleted(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteOutcome::kCompleted,
            status);
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedBeforeCommit() noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteOutcome::kFailedBeforeCommit,
            0);
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(
            Http1BufferedResponseWriteOutcome::kFailedAfterCommit,
            status);
    }

    constexpr Http1BufferedResponseWriteResult(
        Http1BufferedResponseWriteOutcome outcome,
        std::uint16_t status) noexcept
        : status_(status),
          outcome_(outcome) {}

    std::uint16_t status_;
    Http1BufferedResponseWriteOutcome outcome_;
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
