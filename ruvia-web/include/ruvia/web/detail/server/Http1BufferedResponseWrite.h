#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"

namespace ruvia::detail {

// The session has only two decisions after a buffered HTTP/1 write: whether the
// full operation completed (and the connection may remain reusable), and whether
// the complete final head reached the transport (and therefore owns a loggable
// status). The transport error itself is consumed by the writer and never crosses
// this boundary.
class Http1BufferedResponseWriteResult final {
public:
    [[nodiscard]] constexpr bool completed() const noexcept {
        return completed_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t>
    committedStatus() const noexcept {
        return committedStatus_;
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
        return Http1BufferedResponseWriteResult(true, status);
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedBeforeCommit() noexcept {
        return Http1BufferedResponseWriteResult(false, std::nullopt);
    }

    [[nodiscard]] static Http1BufferedResponseWriteResult
    makeFailedAfterCommit(std::uint16_t status) noexcept {
        return Http1BufferedResponseWriteResult(false, status);
    }

    constexpr Http1BufferedResponseWriteResult(
        bool completed,
        std::optional<std::uint16_t> committedStatus) noexcept
        : committedStatus_(committedStatus),
          completed_(completed) {}

    std::optional<std::uint16_t> committedStatus_;
    bool completed_;
};

static_assert(std::is_trivially_copyable_v<Http1BufferedResponseWriteResult>);
static_assert(sizeof(Http1BufferedResponseWriteResult) <= 8);

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
