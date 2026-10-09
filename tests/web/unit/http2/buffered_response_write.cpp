#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "http2/http2_buffered_response_write.h"
#include "test_harness.h"

RUVIA_TEST(http2_buffered_response_write_result_preserves_terminal_cause) {
    using result_type = ruvia::detail::http2_buffered_response_write_result;

    const auto completed = result_type::make_completed(ruvia::http_status::multi_status);
    RUVIA_CHECK(completed.completed() != nullptr);
    RUVIA_CHECK(completed.peer_aborted_before_commit() == nullptr);
    RUVIA_CHECK(completed.peer_aborted_after_commit() == nullptr);
    RUVIA_CHECK(completed.failed_before_commit() == nullptr);
    RUVIA_CHECK(completed.failed_after_commit() == nullptr);
    RUVIA_CHECK_EQ(completed.committed_status(),
        std::optional<ruvia::http_status_code>{ruvia::http_status::multi_status});

    const auto peer_before = result_type::make_peer_aborted_before_commit();
    RUVIA_CHECK(peer_before.peer_aborted_before_commit() != nullptr);
    RUVIA_CHECK(!peer_before.committed_status().has_value());

    const auto peer_after = result_type::make_peer_aborted_after_commit(ruvia::http_status::already_reported);
    RUVIA_CHECK(peer_after.peer_aborted_after_commit() != nullptr);
    RUVIA_CHECK_EQ(peer_after.committed_status(),
        std::optional<ruvia::http_status_code>{ruvia::http_status::already_reported});

    const auto failed_before = result_type::make_failed_before_commit();
    RUVIA_CHECK(failed_before.failed_before_commit() != nullptr);
    RUVIA_CHECK(!failed_before.committed_status().has_value());

    const auto failed_after = result_type::make_failed_after_commit(ruvia::http_status_code::from_value(209));
    RUVIA_CHECK(failed_after.failed_after_commit() != nullptr);
    RUVIA_CHECK_EQ(failed_after.committed_status(),
        std::optional<ruvia::http_status_code>{ruvia::http_status_code::from_value(209)});
}
