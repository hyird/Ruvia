#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

#include "http2/http2_remote_content_state.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_remote_content_accounting_result;
using ruvia::detail::http2_remote_content_allowed_known_length;
using ruvia::detail::http2_remote_content_allowed_without_length;
using ruvia::detail::http2_remote_content_metadata_only_known_length;
using ruvia::detail::http2_remote_content_metadata_only_without_length;
using ruvia::detail::http2_remote_content_state;

}  // namespace

RUVIA_TEST(http2_remote_content_allowance_and_length_alternatives_are_explicit) {
    http2_remote_content_state content;
    RUVIA_CHECK(content.allowed_without_length() != nullptr);
    RUVIA_CHECK(content.allowed_known_length() == nullptr);
    RUVIA_CHECK(content.metadata_only_without_length() == nullptr);
    RUVIA_CHECK(content.metadata_only_known_length() == nullptr);
    RUVIA_CHECK_EQ(content.allowed_without_length()->received_bytes(), std::size_t{0});
    RUVIA_CHECK(content.terminal_length_valid());

    RUVIA_CHECK(content.declare_known_length(0));
    RUVIA_CHECK(content.allowed_without_length() == nullptr);
    const auto* known = content.allowed_known_length();
    RUVIA_CHECK(known != nullptr);
    RUVIA_CHECK_EQ(known->declared_length(), std::size_t{0});
    RUVIA_CHECK(content.terminal_length_valid());
    RUVIA_CHECK(content.declare_known_length(0));
    RUVIA_CHECK(!content.declare_known_length(1));
}

RUVIA_TEST(http2_remote_content_metadata_only_preserves_representation_length) {
    http2_remote_content_state absent;
    RUVIA_CHECK(absent.select_metadata_only());
    RUVIA_CHECK(absent.metadata_only_without_length() != nullptr);
    RUVIA_CHECK(absent.select_metadata_only());
    RUVIA_CHECK(absent.account(1) == http2_remote_content_accounting_result::content_forbidden);
    RUVIA_CHECK(absent.account(0) == http2_remote_content_accounting_result::accepted);

    http2_remote_content_state known;
    RUVIA_CHECK(known.declare_known_length(42));
    RUVIA_CHECK(known.select_metadata_only());
    const auto* metadata = known.metadata_only_known_length();
    RUVIA_CHECK(metadata != nullptr);
    RUVIA_CHECK_EQ(metadata->declared_length(), std::size_t{42});
    RUVIA_CHECK(known.terminal_length_valid());
    RUVIA_CHECK(known.declare_known_length(42));
    RUVIA_CHECK(!known.declare_known_length(43));
}

RUVIA_TEST(http2_remote_content_accounting_is_atomic) {
    http2_remote_content_state content;
    RUVIA_CHECK(content.declare_known_length(100));
    RUVIA_CHECK(content.account(50) == http2_remote_content_accounting_result::accepted);
    RUVIA_CHECK_EQ(content.allowed_known_length()->received_bytes(), std::size_t{50});
    RUVIA_CHECK(!content.terminal_length_valid());

    RUVIA_CHECK(content.account(51) == http2_remote_content_accounting_result::declared_length_exceeded);
    RUVIA_CHECK_EQ(content.allowed_known_length()->received_bytes(), std::size_t{50});
    RUVIA_CHECK(content.account(50) == http2_remote_content_accounting_result::accepted);
    RUVIA_CHECK_EQ(content.allowed_known_length()->received_bytes(), std::size_t{100});
    RUVIA_CHECK(content.terminal_length_valid());
    RUVIA_CHECK(content.account(1) == http2_remote_content_accounting_result::declared_length_exceeded);
    RUVIA_CHECK_EQ(content.allowed_known_length()->received_bytes(), std::size_t{100});
}

RUVIA_TEST(http2_remote_content_counter_overflow_is_atomic) {
    http2_remote_content_state content;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(content.account(maximum) == http2_remote_content_accounting_result::accepted);
    RUVIA_CHECK_EQ(content.allowed_without_length()->received_bytes(), maximum);
    RUVIA_CHECK(content.account(1) == http2_remote_content_accounting_result::counter_overflow);
    RUVIA_CHECK_EQ(content.allowed_without_length()->received_bytes(), maximum);
    RUVIA_CHECK(content.terminal_length_valid());
}

RUVIA_TEST(http2_remote_content_rejects_late_semantic_transitions) {
    http2_remote_content_state content;
    RUVIA_CHECK(content.account(1) == http2_remote_content_accounting_result::accepted);
    RUVIA_CHECK(!content.declare_known_length(1));
    RUVIA_CHECK(!content.select_metadata_only());
    RUVIA_CHECK(content.allowed_without_length() != nullptr);
    RUVIA_CHECK_EQ(content.allowed_without_length()->received_bytes(), std::size_t{1});
}
