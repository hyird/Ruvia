#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

#include "ruvia/http/detail/http2/Http2RemoteContentState.h"

namespace {

using ruvia::detail::Http2RemoteContentAccountingResult;
using ruvia::detail::Http2RemoteContentAllowedKnownLength;
using ruvia::detail::Http2RemoteContentAllowedWithoutLength;
using ruvia::detail::Http2RemoteContentMetadataOnlyKnownLength;
using ruvia::detail::Http2RemoteContentMetadataOnlyWithoutLength;
using ruvia::detail::Http2RemoteContentState;

template <typename T>
concept HasDeclaredLength = requires(const T& value) {
    { value.declaredLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasStaleCheckAcceptSplit = requires(T& value) {
    value.checkAccept(std::size_t{1});
    value.accept(std::size_t{1});
};

template <typename T>
concept HasStaleLengthTuple = requires(const T& value) {
    value.hasContentLength();
    value.contentLength();
};

template <typename T>
concept HasReceivedBytes = requires(const T& value) {
    { value.receivedBytes() } -> std::same_as<std::size_t>;
};

static_assert(std::default_initializable<Http2RemoteContentState>);
static_assert(!std::default_initializable<
    Http2RemoteContentAllowedWithoutLength>);
static_assert(!std::default_initializable<
    Http2RemoteContentAllowedKnownLength>);
static_assert(!std::default_initializable<
    Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!std::default_initializable<
    Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasDeclaredLength<Http2RemoteContentState>);
static_assert(!HasReceivedBytes<Http2RemoteContentState>);
static_assert(HasReceivedBytes<Http2RemoteContentAllowedWithoutLength>);
static_assert(HasReceivedBytes<Http2RemoteContentAllowedKnownLength>);
static_assert(!HasReceivedBytes<
    Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(!HasReceivedBytes<
    Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasDeclaredLength<Http2RemoteContentAllowedWithoutLength>);
static_assert(HasDeclaredLength<Http2RemoteContentAllowedKnownLength>);
static_assert(!HasDeclaredLength<
    Http2RemoteContentMetadataOnlyWithoutLength>);
static_assert(HasDeclaredLength<
    Http2RemoteContentMetadataOnlyKnownLength>);
static_assert(!HasStaleCheckAcceptSplit<Http2RemoteContentState>);
static_assert(!HasStaleLengthTuple<Http2RemoteContentState>);

}  // namespace

RUVIA_TEST(http2_remote_content_allowance_and_length_alternatives_are_explicit) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.allowedWithoutLength() != nullptr);
    RUVIA_CHECK(content.allowedKnownLength() == nullptr);
    RUVIA_CHECK(content.metadataOnlyWithoutLength() == nullptr);
    RUVIA_CHECK(content.metadataOnlyKnownLength() == nullptr);
    RUVIA_CHECK_EQ(
        content.allowedWithoutLength()->receivedBytes(), std::size_t{0});
    RUVIA_CHECK(content.terminalLengthValid());

    RUVIA_CHECK(content.declareKnownLength(0));
    RUVIA_CHECK(content.allowedWithoutLength() == nullptr);
    const auto* known = content.allowedKnownLength();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{0});
    RUVIA_CHECK(content.terminalLengthValid());
    RUVIA_CHECK(content.declareKnownLength(0));
    RUVIA_CHECK(!content.declareKnownLength(1));
}

RUVIA_TEST(http2_remote_content_metadata_only_preserves_representation_length) {
    Http2RemoteContentState absent;
    RUVIA_CHECK(absent.selectMetadataOnly());
    RUVIA_CHECK(absent.metadataOnlyWithoutLength() != nullptr);
    RUVIA_CHECK(absent.selectMetadataOnly());
    RUVIA_CHECK(absent.account(1) ==
        Http2RemoteContentAccountingResult::kContentForbidden);
    RUVIA_CHECK(absent.account(0) ==
        Http2RemoteContentAccountingResult::kAccepted);

    Http2RemoteContentState known;
    RUVIA_CHECK(known.declareKnownLength(42));
    RUVIA_CHECK(known.selectMetadataOnly());
    const auto* metadata = known.metadataOnlyKnownLength();
    RUVIA_CHECK(metadata != nullptr);
    RUVIA_CHECK_EQ(metadata->declaredLength(), std::size_t{42});
    RUVIA_CHECK(known.terminalLengthValid());
    RUVIA_CHECK(known.declareKnownLength(42));
    RUVIA_CHECK(!known.declareKnownLength(43));
}

RUVIA_TEST(http2_remote_content_accounting_is_atomic) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.declareKnownLength(100));
    RUVIA_CHECK(content.account(50) ==
        Http2RemoteContentAccountingResult::kAccepted);
    RUVIA_CHECK_EQ(
        content.allowedKnownLength()->receivedBytes(), std::size_t{50});
    RUVIA_CHECK(!content.terminalLengthValid());

    RUVIA_CHECK(content.account(51) ==
        Http2RemoteContentAccountingResult::kDeclaredLengthExceeded);
    RUVIA_CHECK_EQ(
        content.allowedKnownLength()->receivedBytes(), std::size_t{50});
    RUVIA_CHECK(content.account(50) ==
        Http2RemoteContentAccountingResult::kAccepted);
    RUVIA_CHECK_EQ(
        content.allowedKnownLength()->receivedBytes(), std::size_t{100});
    RUVIA_CHECK(content.terminalLengthValid());
    RUVIA_CHECK(content.account(1) ==
        Http2RemoteContentAccountingResult::kDeclaredLengthExceeded);
    RUVIA_CHECK_EQ(
        content.allowedKnownLength()->receivedBytes(), std::size_t{100});
}

RUVIA_TEST(http2_remote_content_counter_overflow_is_atomic) {
    Http2RemoteContentState content;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(content.account(maximum) ==
        Http2RemoteContentAccountingResult::kAccepted);
    RUVIA_CHECK_EQ(
        content.allowedWithoutLength()->receivedBytes(), maximum);
    RUVIA_CHECK(content.account(1) ==
        Http2RemoteContentAccountingResult::kCounterOverflow);
    RUVIA_CHECK_EQ(
        content.allowedWithoutLength()->receivedBytes(), maximum);
    RUVIA_CHECK(content.terminalLengthValid());
}

RUVIA_TEST(http2_remote_content_rejects_late_semantic_transitions) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.account(1) ==
        Http2RemoteContentAccountingResult::kAccepted);
    RUVIA_CHECK(!content.declareKnownLength(1));
    RUVIA_CHECK(!content.selectMetadataOnly());
    RUVIA_CHECK(content.allowedWithoutLength() != nullptr);
    RUVIA_CHECK_EQ(
        content.allowedWithoutLength()->receivedBytes(), std::size_t{1});
}
