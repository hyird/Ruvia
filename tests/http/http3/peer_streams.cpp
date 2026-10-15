#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <variant>

#include "ruvia/http/http3_peer_streams.h"

#include "test_harness.h"

namespace {

using ruvia::http3_peer_role;
using ruvia::http3_peer_stream_error;
using ruvia::http3_peer_stream_kind;
using ruvia::http3_peer_stream_limits;
using ruvia::http3_peer_streams;
using ruvia::http3_stream_id_type;

}  // namespace

RUVIA_TEST(http3_stream_id_helpers_classify_role_direction_and_range) {
    constexpr std::array expected{
        http3_stream_id_type::client_bidirectional,
        http3_stream_id_type::server_bidirectional,
        http3_stream_id_type::client_unidirectional,
        http3_stream_id_type::server_unidirectional,
    };
    for (std::uint64_t stream_id = 0; stream_id < expected.size(); ++stream_id) {
        RUVIA_CHECK(ruvia::http3_stream_id_type(stream_id) == expected[stream_id]);
    }
    RUVIA_CHECK(ruvia::is_http3_request_stream_id(0));
    RUVIA_CHECK(ruvia::is_http3_request_stream_id(4));
    RUVIA_CHECK(!ruvia::is_http3_request_stream_id(1));
    RUVIA_CHECK(!ruvia::is_http3_request_stream_id(ruvia::http3_var_int_max + 1));
    RUVIA_CHECK(ruvia::is_http3_unidirectional_stream_id(2));
    RUVIA_CHECK(ruvia::is_http3_unidirectional_stream_id(3));
    RUVIA_CHECK(!ruvia::is_http3_unidirectional_stream_id(ruvia::http3_var_int_max + 1));
    RUVIA_CHECK(ruvia::is_http3_peer_unidirectional_stream_id(http3_peer_role::server, 2));
    RUVIA_CHECK(ruvia::is_http3_peer_unidirectional_stream_id(http3_peer_role::client, 3));
    RUVIA_CHECK(!ruvia::is_http3_peer_unidirectional_stream_id(http3_peer_role::server, 3));
    RUVIA_CHECK(ruvia::is_http3_peer_bidirectional_stream_id(http3_peer_role::server, 0));
    RUVIA_CHECK(!ruvia::is_http3_peer_bidirectional_stream_id(http3_peer_role::client, 1));
}

RUVIA_TEST(http3_peer_streams_incrementally_classifies_and_borrows_remainder) {
    std::pmr::monotonic_buffer_resource resource;
    http3_peer_streams streams(http3_peer_role::client, &resource);
    constexpr std::array<char, 3> type_and_payload{0x40, 0x02, 'x'};  // QPACK encoder type 2.

    const auto partial = streams.feed(3, std::span<const char>(type_and_payload).first(1));
    RUVIA_CHECK((partial.index() == 0));
    if ((partial.index() == 0)) {
        RUVIA_CHECK(std::get<0>(partial).kind_ == http3_peer_stream_kind::unclassified);
        RUVIA_CHECK_EQ(std::get<0>(partial).consumed_, std::size_t{1});
    }
    const auto classified = streams.feed(3, std::span<const char>(type_and_payload).subspan(1));
    RUVIA_CHECK((classified.index() == 0));
    if ((classified.index() == 0)) {
        RUVIA_CHECK(std::get<0>(classified).kind_ == http3_peer_stream_kind::qpack_encoder);
        RUVIA_CHECK_EQ(std::get<0>(classified).consumed_, std::size_t{1});
        RUVIA_CHECK_EQ(std::get<0>(classified).remaining_.size(), std::size_t{1});
        RUVIA_CHECK(std::get<0>(classified).remaining_.data() == type_and_payload.data() + 2);
    }
    constexpr std::array<char, 1> later{'y'};
    const auto next_value = streams.feed(3, later);
    RUVIA_CHECK((next_value.index() == 0));
    if ((next_value.index() == 0)) {
        RUVIA_CHECK(std::get<0>(next_value).kind_ == http3_peer_stream_kind::qpack_encoder);
        RUVIA_CHECK_EQ(std::get<0>(next_value).consumed_, std::size_t{0});
        RUVIA_CHECK(std::get<0>(next_value).remaining_.data() == later.data());
    }
}

RUVIA_TEST(http3_peer_streams_enforces_critical_uniqueness_and_termination) {
    std::pmr::monotonic_buffer_resource resource;
    http3_peer_streams streams(http3_peer_role::client, &resource);
    constexpr std::array<char, 1> control{0};
    RUVIA_CHECK((streams.feed(3, control).index() == 0));
    const auto duplicate = streams.feed(7, control);
    RUVIA_CHECK((duplicate.index() != 0));
    if ((duplicate.index() != 0)) {
        RUVIA_CHECK(std::get<1>(duplicate) == http3_peer_stream_error::stream_creation_error);
    }
    const auto closed = streams.feed(3, {}, true);
    RUVIA_CHECK((closed.index() != 0));
    if ((closed.index() != 0)) {
        RUVIA_CHECK(std::get<1>(closed) == http3_peer_stream_error::closed_critical_stream);
    }
    RUVIA_CHECK(streams.active_stream_count() == std::size_t{1});
}

RUVIA_TEST(http3_peer_streams_tolerates_early_close_and_releases_noncritical_streams) {
    std::pmr::monotonic_buffer_resource resource;
    http3_peer_streams streams(http3_peer_role::client, &resource);
    const auto early_fin = streams.feed(3, {}, true);
    RUVIA_CHECK((early_fin.index() == 0));
    if ((early_fin.index() == 0)) {
        RUVIA_CHECK(std::get<0>(early_fin).closed_);
        RUVIA_CHECK_EQ(std::get<0>(early_fin).consumed_, std::size_t{0});
    }

    constexpr std::array<char, 1> unknown{0x21};
    const auto accepted = streams.feed(3, unknown);
    RUVIA_CHECK((accepted.index() == 0));
    if ((accepted.index() == 0)) {
        RUVIA_CHECK(std::get<0>(accepted).kind_ == http3_peer_stream_kind::unknown);
        RUVIA_CHECK_EQ(std::get<0>(accepted).stream_type_, std::uint64_t{33});
    }
    const auto closed = streams.feed(3, {}, false, true);
    RUVIA_CHECK((closed.index() == 0));
    if ((closed.index() == 0)) {
        RUVIA_CHECK(std::get<0>(closed).reset_);
        RUVIA_CHECK(!std::get<0>(closed).fin_);
    }
    RUVIA_CHECK_EQ(streams.active_stream_count(), std::size_t{0});
}

RUVIA_TEST(http3_peer_streams_rejects_critical_stream_closed_with_header_in_same_feed) {
    std::pmr::monotonic_buffer_resource resource;
    http3_peer_streams streams(http3_peer_role::server, &resource);
    constexpr std::array<char, 1> control_type_value{0};
    const auto closed = streams.feed(2, control_type_value, true);
    RUVIA_CHECK((closed.index() != 0));
    if ((closed.index() != 0)) {
        RUVIA_CHECK(std::get<1>(closed) == http3_peer_stream_error::closed_critical_stream);
    }
    const auto reopened = streams.feed(6, control_type_value);
    RUVIA_CHECK((reopened.index() != 0));
    if ((reopened.index() != 0)) {
        RUVIA_CHECK(std::get<1>(reopened) == http3_peer_stream_error::stream_creation_error);
    }
}

RUVIA_TEST(http3_peer_streams_applies_role_and_active_stream_limit) {
    std::pmr::monotonic_buffer_resource resource;
    http3_peer_streams server(http3_peer_role::server, &resource, {.max_active_streams_ = 1});
    constexpr std::array<char, 1> push{1};
    const auto forbidden_push = server.feed(2, push);
    RUVIA_CHECK((forbidden_push.index() != 0));
    if ((forbidden_push.index() != 0)) {
        RUVIA_CHECK(std::get<1>(forbidden_push) == http3_peer_stream_error::stream_creation_error);
    }
    constexpr std::array<char, 1> unknown{0x21};
    RUVIA_CHECK((server.feed(2, unknown).index() == 0));
    const auto over_limit = server.feed(6, unknown);
    RUVIA_CHECK((over_limit.index() != 0));
    if ((over_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(over_limit) == http3_peer_stream_error::excessive_load);
    }
    RUVIA_CHECK((http3_peer_streams::accept_bidirectional(http3_peer_role::server, 0).index() == 0));
    const auto server_bidi = http3_peer_streams::accept_bidirectional(http3_peer_role::client, 1);
    RUVIA_CHECK((server_bidi.index() != 0));
    if ((server_bidi.index() != 0)) {
        RUVIA_CHECK(std::get<1>(server_bidi) == http3_peer_stream_error::stream_creation_error);
    }
    RUVIA_CHECK(std::get<1>(http3_peer_streams::accept_bidirectional(http3_peer_role::server, 1)) ==
                http3_peer_stream_error::stream_creation_error);
}
