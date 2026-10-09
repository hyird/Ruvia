#include <concepts>
#include <cstddef>
#include <cstdint>

#include "http2/http2_local_content_state.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_local_content_check;
using ruvia::detail::http2_local_content_known_length;
using ruvia::detail::http2_local_content_state;

}  // namespace

RUVIA_TEST(http2_local_content_known_length_preflight_is_transactional) {
    http2_local_content_state state;
    state.begin_known_length(5);

    const auto* known_length = state.known_length();
    RUVIA_CHECK(state.unset() == nullptr);
    RUVIA_CHECK(state.forbidden() == nullptr);
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(known_length != nullptr);
    if (known_length != nullptr) {
        RUVIA_CHECK_EQ(known_length->declared_length(), std::uint64_t{5});
    }
    RUVIA_CHECK(state.check_accept(3, true) == http2_local_content_check::length_incomplete);
    RUVIA_CHECK(state.check_accept(6, false) == http2_local_content_check::length_exceeded);
    RUVIA_CHECK_EQ(state.accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committed_bytes(), std::uint64_t{0});

    RUVIA_CHECK(state.check_accept(3, false) == http2_local_content_check::accepted);
    state.accept(3);
    state.commit(2);
    RUVIA_CHECK_EQ(state.accepted_bytes(), std::uint64_t{3});
    RUVIA_CHECK_EQ(state.committed_bytes(), std::uint64_t{2});
    RUVIA_CHECK(!state.length_complete());

    RUVIA_CHECK(state.check_accept(3, true) == http2_local_content_check::length_exceeded);
    RUVIA_CHECK(state.check_accept(2, true) == http2_local_content_check::accepted);
    state.accept(2);
    state.commit(3);
    RUVIA_CHECK(state.length_complete());
    RUVIA_CHECK_EQ(state.accepted_bytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(state.committed_bytes(), std::uint64_t{5});
}
RUVIA_TEST(http2_local_content_alternatives_are_explicit) {
    http2_local_content_state state;
    RUVIA_CHECK(state.unset() != nullptr);
    RUVIA_CHECK(state.forbidden() == nullptr);
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(state.known_length() == nullptr);
    RUVIA_CHECK(!state.length_complete());
    RUVIA_CHECK(state.check_accept(0, true) == http2_local_content_check::not_started);

    state.begin_unbounded();
    RUVIA_CHECK(state.unset() == nullptr);
    RUVIA_CHECK(state.unbounded() != nullptr);
    RUVIA_CHECK(state.known_length() == nullptr);
    RUVIA_CHECK(state.check_accept(7, true) == http2_local_content_check::accepted);
    state.accept(7);
    state.commit(4);
    RUVIA_CHECK(state.length_complete());
    RUVIA_CHECK_EQ(state.accepted_bytes(), std::uint64_t{7});
    RUVIA_CHECK_EQ(state.committed_bytes(), std::uint64_t{4});

    state.begin_forbidden();
    RUVIA_CHECK(state.unbounded() == nullptr);
    RUVIA_CHECK(state.forbidden() != nullptr);
    RUVIA_CHECK(state.known_length() == nullptr);
    RUVIA_CHECK(state.check_accept(0, true) == http2_local_content_check::forbidden);
    RUVIA_CHECK(state.check_accept(1, false) == http2_local_content_check::forbidden);
    RUVIA_CHECK_EQ(state.accepted_bytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(state.committed_bytes(), std::uint64_t{0});
}
