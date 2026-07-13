#include "test_harness.h"

#include <concepts>
#include <cstdint>

#include "ruvia/web/detail/server/Http2BufferedResponseWrite.h"

namespace {

template <typename Alternative>
concept HasStatus = requires(const Alternative& value) {
    { value.status() } -> std::same_as<std::uint16_t>;
};

template <typename Alternative>
concept HasSubmitError = requires(const Alternative& value) {
    { value.error() } ->
        std::same_as<ruvia::detail::Http2ResponseHeadSubmitError>;
};

static_assert(!std::default_initializable<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(HasStatus<
    ruvia::detail::Http2BufferedResponseWriteCompleted>);
static_assert(HasStatus<
    ruvia::detail::Http2BufferedResponseWritePeerAbortedAfterCommit>);
static_assert(HasStatus<
    ruvia::detail::Http2BufferedResponseWriteFailedAfterCommit>);
static_assert(!HasStatus<
    ruvia::detail::Http2BufferedResponseWritePeerAbortedBeforeCommit>);
static_assert(!HasStatus<
    ruvia::detail::Http2BufferedResponseWriteFailedBeforeCommit>);
static_assert(HasSubmitError<
    ruvia::detail::Http2BufferedResponseWriteFailedBeforeCommit>);

}  // namespace

RUVIA_TEST(http2_buffered_response_write_result_owns_only_committed_status) {
    using Result = ruvia::detail::Http2BufferedResponseWriteResult;

    const auto completed = Result::makeCompleted(207);
    RUVIA_CHECK(completed.completed() != nullptr);
    RUVIA_CHECK_EQ(completed.completed()->status(), std::uint16_t{207});
    RUVIA_CHECK(completed.peerAbortedBeforeCommit() == nullptr);
    RUVIA_CHECK(completed.peerAbortedAfterCommit() == nullptr);
    RUVIA_CHECK(completed.failedBeforeCommit() == nullptr);
    RUVIA_CHECK(completed.failedAfterCommit() == nullptr);

    const auto peerBefore = Result::makePeerAbortedBeforeCommit();
    RUVIA_CHECK(peerBefore.peerAbortedBeforeCommit() != nullptr);
    RUVIA_CHECK(peerBefore.completed() == nullptr);

    const auto peerAfter = Result::makePeerAbortedAfterCommit(206);
    RUVIA_CHECK(peerAfter.peerAbortedAfterCommit() != nullptr);
    RUVIA_CHECK_EQ(
        peerAfter.peerAbortedAfterCommit()->status(),
        std::uint16_t{206});

    const auto failedBefore = Result::makeFailedBeforeCommit(
        ruvia::detail::Http2ResponseHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(failedBefore.failedBeforeCommit() != nullptr);
    RUVIA_CHECK(
        failedBefore.failedBeforeCommit()->error() ==
        ruvia::detail::Http2ResponseHeadSubmitError::kInvalidMessage);

    const auto failedAfter = Result::makeFailedAfterCommit(503);
    RUVIA_CHECK(failedAfter.failedAfterCommit() != nullptr);
    RUVIA_CHECK_EQ(
        failedAfter.failedAfterCommit()->status(),
        std::uint16_t{503});
}
