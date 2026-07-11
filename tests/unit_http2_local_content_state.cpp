#include "test_harness.h"

#include <cstddef>
#include <cstdint>

#include "ruvia/http/detail/http2/Http2LocalContentState.h"

namespace {

using ruvia::detail::Http2LocalContentCheck;
using ruvia::detail::Http2LocalContentMode;
using ruvia::detail::Http2LocalContentState;

}  // namespace

RUVIA_TEST(http2_local_content_known_length_preflight_is_transactional) {
    Http2LocalContentState state;
    state.beginKnownLength(5);

    RUVIA_CHECK(state.mode() == Http2LocalContentMode::kKnownLength);
    RUVIA_CHECK(state.checkAccept(3, true) ==
        Http2LocalContentCheck::kLengthIncomplete);
    RUVIA_CHECK(state.checkAccept(6, false) ==
        Http2LocalContentCheck::kLengthExceeded);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{0});

    RUVIA_CHECK(state.checkAccept(3, false) == Http2LocalContentCheck::kAccepted);
    state.accept(3);
    state.commit(2);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{3});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{2});
    RUVIA_CHECK(!state.lengthComplete());

    RUVIA_CHECK(state.checkAccept(3, true) ==
        Http2LocalContentCheck::kLengthExceeded);
    RUVIA_CHECK(state.checkAccept(2, true) == Http2LocalContentCheck::kAccepted);
    state.accept(2);
    state.commit(3);
    RUVIA_CHECK(state.lengthComplete());
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{5});
}
RUVIA_TEST(http2_local_content_unbounded_and_forbidden_modes_are_explicit) {
    Http2LocalContentState state;
    RUVIA_CHECK(state.mode() == Http2LocalContentMode::kUnset);

    state.beginUnbounded();
    RUVIA_CHECK(state.checkAccept(7, true) == Http2LocalContentCheck::kAccepted);
    state.accept(7);
    state.commit(4);
    RUVIA_CHECK(state.lengthComplete());
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{7});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{4});

    state.beginForbidden();
    RUVIA_CHECK(state.mode() == Http2LocalContentMode::kForbidden);
    RUVIA_CHECK(state.checkAccept(0, true) == Http2LocalContentCheck::kForbidden);
    RUVIA_CHECK(state.checkAccept(1, false) == Http2LocalContentCheck::kForbidden);
    RUVIA_CHECK_EQ(state.acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committedBytes(), std::uint64_t{0});
}
