#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"

namespace {

static_assert(!std::default_initializable<ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(sizeof(ruvia::detail::Http2BufferedResponseWriteResult) <= 4);
static_assert(
    std::same_as<decltype(std::declval<const ruvia::detail::Http2BufferedResponseWriteResult&>()
                         .committedStatus()),
        std::optional<ruvia::HttpStatusCode>>);

}  // namespace

RUVIA_TEST(http2_buffered_response_write_result_preserves_terminal_cause) {
    using Result = ruvia::detail::Http2BufferedResponseWriteResult;

    const auto completed = Result::makeCompleted(ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(completed.completed() != nullptr);
    RUVIA_CHECK(completed.peerAbortedBeforeCommit() == nullptr);
    RUVIA_CHECK(completed.peerAbortedAfterCommit() == nullptr);
    RUVIA_CHECK(completed.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(completed.failedAfterCommit() == nullptr);
    RUVIA_CHECK_EQ(completed.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kMultiStatus});

    const auto peerBefore = Result::makePeerAbortedBeforeCommit();
    RUVIA_CHECK(peerBefore.peerAbortedBeforeCommit() != nullptr);
    RUVIA_CHECK(!peerBefore.committedStatus().has_value());

    const auto peerAfter = Result::makePeerAbortedAfterCommit(ruvia::http_status::kAlreadyReported);
    RUVIA_CHECK(peerAfter.peerAbortedAfterCommit() != nullptr);
    RUVIA_CHECK_EQ(peerAfter.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::http_status::kAlreadyReported});

    const auto failedBefore = Result::makeFailedBeforeCommit();
    RUVIA_CHECK(failedBefore.failedBeforeCommit() != nullptr);
    RUVIA_CHECK(!failedBefore.committedStatus().has_value());

    const auto failedAfter = Result::makeFailedAfterCommit(ruvia::HttpStatusCode::fromValue(209));
    RUVIA_CHECK(failedAfter.failedAfterCommit() != nullptr);
    RUVIA_CHECK_EQ(failedAfter.committedStatus(),
        std::optional<ruvia::HttpStatusCode>{ruvia::HttpStatusCode::fromValue(209)});
}
