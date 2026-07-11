#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

#include "ruvia/http/detail/http2/Http2RemoteContentState.h"

namespace {

using ruvia::detail::Http2RemoteContentCheck;
using ruvia::detail::Http2RemoteContentKnownLength;
using ruvia::detail::Http2RemoteContentState;
using ruvia::detail::Http2RemoteContentWithoutLength;

template <typename T>
concept HasDeclaredLength = requires(const T& value) {
    { value.declaredLength() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasStaleLengthTuple = requires(const T& value) {
    value.hasContentLength();
    value.contentLength();
};

static_assert(std::default_initializable<Http2RemoteContentState>);
static_assert(!std::default_initializable<Http2RemoteContentWithoutLength>);
static_assert(!std::default_initializable<Http2RemoteContentKnownLength>);
static_assert(!HasDeclaredLength<Http2RemoteContentState>);
static_assert(!HasDeclaredLength<Http2RemoteContentWithoutLength>);
static_assert(HasDeclaredLength<Http2RemoteContentKnownLength>);
static_assert(!HasStaleLengthTuple<Http2RemoteContentState>);

}  // namespace

RUVIA_TEST(http2_remote_content_length_alternatives_are_explicit) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.withoutLength() != nullptr);
    RUVIA_CHECK(content.knownLength() == nullptr);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{0});
    RUVIA_CHECK(content.terminalLengthValid());

    RUVIA_CHECK(content.declareKnownLength(0));
    RUVIA_CHECK(content.withoutLength() == nullptr);
    const auto* known = content.knownLength();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declaredLength(), std::size_t{0});
    RUVIA_CHECK(content.terminalLengthValid());
    RUVIA_CHECK(content.declareKnownLength(0));
    RUVIA_CHECK(!content.declareKnownLength(1));
    RUVIA_CHECK_EQ(
        content.knownLength()->declaredLength(), std::size_t{0});
}

RUVIA_TEST(http2_remote_content_acceptance_is_transactional) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.declareKnownLength(100));
    RUVIA_CHECK(content.checkAccept(50) ==
        Http2RemoteContentCheck::kAccepted);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{0});
    content.accept(50);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{50});
    RUVIA_CHECK(!content.terminalLengthValid());

    RUVIA_CHECK(content.checkAccept(51) ==
        Http2RemoteContentCheck::kDeclaredLengthExceeded);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{50});
    RUVIA_CHECK(content.checkAccept(50) ==
        Http2RemoteContentCheck::kAccepted);
    content.accept(50);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{100});
    RUVIA_CHECK(content.terminalLengthValid());
    RUVIA_CHECK(content.checkAccept(1) ==
        Http2RemoteContentCheck::kDeclaredLengthExceeded);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{100});
}

RUVIA_TEST(http2_remote_content_counter_overflow_is_transactional) {
    Http2RemoteContentState content;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(content.checkAccept(maximum) ==
        Http2RemoteContentCheck::kAccepted);
    content.accept(maximum);
    RUVIA_CHECK_EQ(content.receivedBytes(), maximum);
    RUVIA_CHECK(content.checkAccept(1) ==
        Http2RemoteContentCheck::kCounterOverflow);
    RUVIA_CHECK_EQ(content.receivedBytes(), maximum);
    RUVIA_CHECK(content.terminalLengthValid());
}

RUVIA_TEST(http2_remote_content_rejects_late_length_declaration) {
    Http2RemoteContentState content;
    RUVIA_CHECK(content.checkAccept(1) ==
        Http2RemoteContentCheck::kAccepted);
    content.accept(1);
    RUVIA_CHECK(!content.declareKnownLength(1));
    RUVIA_CHECK(content.withoutLength() != nullptr);
    RUVIA_CHECK(content.knownLength() == nullptr);
    RUVIA_CHECK_EQ(content.receivedBytes(), std::size_t{1});
}
