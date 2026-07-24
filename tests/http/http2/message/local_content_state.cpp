#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>

#include "ruvia/http/detail/http2/message/Http2LocalContentState.h"

namespace {

using ruvia::detail::Http2LocalContentCheck;
using ruvia::detail::Http2LocalContentKnownLength;
using ruvia::detail::Http2LocalContentState;

template <typename T>
concept HasLocalContentMode = requires(const T& content) { content.mode(); };

template <typename T>
concept HasDeclaredLength = requires(const T& content) {
    { content.declaredLength() } -> std::same_as<std::uint64_t>;
};

static_assert(!HasLocalContentMode<Http2LocalContentState>);
static_assert(!HasDeclaredLength<Http2LocalContentState>);
static_assert(!HasDeclaredLength<ruvia::detail::Http2LocalContentUnset>);
static_assert(!HasDeclaredLength<ruvia::detail::Http2LocalContentForbidden>);
static_assert(!HasDeclaredLength<ruvia::detail::Http2LocalContentUnbounded>);
static_assert(HasDeclaredLength<Http2LocalContentKnownLength>);
static_assert(std::default_initializable<Http2LocalContentState>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentUnset>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentForbidden>);
static_assert(!std::default_initializable<ruvia::detail::Http2LocalContentUnbounded>);
static_assert(!std::default_initializable<Http2LocalContentKnownLength>);
static_assert(!std::constructible_from<Http2LocalContentKnownLength, std::uint64_t>);

}  // namespace

RUVIA_TEST(http2_local_content_known_length_preflight_is_transactional) {
    Http2LocalContentState state;
    state.beginKnownLength(5);

    const auto* knownLength = state.knownLength();
    RUVIA_CHECK(state.unset() == nullptr);
    RUVIA_CHECK(state.forbidden() == nullptr);
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(knownLength != nullptr);
    if (knownLength != nullptr) {
        RUVIA_CHECK_EQ(knownLength->declaredLength(), std::uint64_t{5});
    }
    RUVIA_CHECK(state.checkAccept(3, true) == Http2LocalContentCheck::kLengthIncomplete);
    RUVIA_CHECK(state.checkAccept(6, false) == Http2LocalContentCheck::kLengthExceeded);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{0});

    RUVIA_CHECK(state.checkAccept(3, false) == Http2LocalContentCheck::kAccepted);
    state.accept(3);
    state.commit(2);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{3});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{2});
    RUVIA_CHECK(!state.lengthComplete());

    RUVIA_CHECK(state.checkAccept(3, true) == Http2LocalContentCheck::kLengthExceeded);
    RUVIA_CHECK(state.checkAccept(2, true) == Http2LocalContentCheck::kAccepted);
    state.accept(2);
    state.commit(3);
    RUVIA_CHECK(state.lengthComplete());
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{5});
}
RUVIA_TEST(http2_local_content_alternatives_are_explicit) {
    Http2LocalContentState state;
    RUVIA_CHECK(state.unset() != nullptr);
    RUVIA_CHECK(state.forbidden() == nullptr);
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(state.knownLength() == nullptr);
    RUVIA_CHECK(!state.lengthComplete());
    RUVIA_CHECK(state.checkAccept(0, true) == Http2LocalContentCheck::kNotStarted);

    state.beginUnbounded();
    RUVIA_CHECK(state.unset() == nullptr);
    RUVIA_CHECK(state.unbounded() != nullptr);
    RUVIA_CHECK(state.knownLength() == nullptr);
    RUVIA_CHECK(state.checkAccept(7, true) == Http2LocalContentCheck::kAccepted);
    state.accept(7);
    state.commit(4);
    RUVIA_CHECK(state.lengthComplete());
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{7});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{4});

    state.beginForbidden();
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(state.forbidden() != nullptr);
    RUVIA_CHECK(state.knownLength() == nullptr);
    RUVIA_CHECK(state.checkAccept(0, true) == Http2LocalContentCheck::kForbidden);
    RUVIA_CHECK(state.checkAccept(1, false) == Http2LocalContentCheck::kForbidden);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{0});
}
