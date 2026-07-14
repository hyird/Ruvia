#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/server/Http2BufferedResponseWrite.h"

namespace {

static_assert(!std::default_initializable<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(std::is_trivially_copyable_v<
    ruvia::detail::Http2BufferedResponseWriteResult>);
static_assert(sizeof(ruvia::detail::Http2BufferedResponseWriteResult) <= 4);
static_assert(std::same_as<
    decltype(std::declval<const
        ruvia::detail::Http2BufferedResponseWriteResult&>()
        .committedStatus()),
    std::optional<std::uint16_t>>);

}  // namespace

RUVIA_TEST(http2_buffered_response_write_result_is_only_committed_status) {
    using Result = ruvia::detail::Http2BufferedResponseWriteResult;

    const auto committed = Result::committed(207);
    RUVIA_CHECK_EQ(
        committed.committedStatus(),
        std::optional<std::uint16_t>{207});

    const auto uncommitted = Result::uncommitted();
    RUVIA_CHECK(!uncommitted.committedStatus().has_value());
}
