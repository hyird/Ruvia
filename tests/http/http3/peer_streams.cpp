#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>

#include "ruvia/http/Http3PeerStreams.h"

#include "test_harness.h"

namespace {

using ruvia::Http3PeerRole;
using ruvia::Http3PeerStreamError;
using ruvia::Http3PeerStreamKind;
using ruvia::Http3PeerStreamLimits;
using ruvia::Http3PeerStreams;
using ruvia::Http3StreamIdType;

}  // namespace

RUVIA_TEST(http3_stream_id_helpers_classify_role_direction_and_range) {
    constexpr std::array expected{
        Http3StreamIdType::kClientBidirectional,
        Http3StreamIdType::kServerBidirectional,
        Http3StreamIdType::kClientUnidirectional,
        Http3StreamIdType::kServerUnidirectional,
    };
    for (std::uint64_t streamId = 0; streamId < expected.size(); ++streamId) {
        RUVIA_CHECK(ruvia::http3StreamIdType(streamId) == expected[streamId]);
    }
    RUVIA_CHECK(ruvia::isHttp3RequestStreamId(0));
    RUVIA_CHECK(ruvia::isHttp3RequestStreamId(4));
    RUVIA_CHECK(!ruvia::isHttp3RequestStreamId(1));
    RUVIA_CHECK(!ruvia::isHttp3RequestStreamId(ruvia::kHttp3VarIntMax + 1));
    RUVIA_CHECK(ruvia::isHttp3UnidirectionalStreamId(2));
    RUVIA_CHECK(ruvia::isHttp3UnidirectionalStreamId(3));
    RUVIA_CHECK(!ruvia::isHttp3UnidirectionalStreamId(ruvia::kHttp3VarIntMax + 1));
    RUVIA_CHECK(ruvia::isHttp3PeerUnidirectionalStreamId(Http3PeerRole::kServer, 2));
    RUVIA_CHECK(ruvia::isHttp3PeerUnidirectionalStreamId(Http3PeerRole::kClient, 3));
    RUVIA_CHECK(!ruvia::isHttp3PeerUnidirectionalStreamId(Http3PeerRole::kServer, 3));
    RUVIA_CHECK(ruvia::isHttp3PeerBidirectionalStreamId(Http3PeerRole::kServer, 0));
    RUVIA_CHECK(!ruvia::isHttp3PeerBidirectionalStreamId(Http3PeerRole::kClient, 1));
}

RUVIA_TEST(http3_peer_streams_incrementally_classifies_and_borrows_remainder) {
    std::pmr::monotonic_buffer_resource resource;
    Http3PeerStreams streams(Http3PeerRole::kClient, &resource);
    constexpr std::array<char, 3> typeAndPayload{0x40, 0x02, 'x'};  // QPACK encoder type 2.

    const auto partial = streams.feed(3, std::span<const char>(typeAndPayload).first(1));
    RUVIA_CHECK(partial.has_value());
    if (partial) {
        RUVIA_CHECK(partial->kind == Http3PeerStreamKind::kUnclassified);
        RUVIA_CHECK_EQ(partial->consumed, std::size_t{1});
    }
    const auto classified = streams.feed(3, std::span<const char>(typeAndPayload).subspan(1));
    RUVIA_CHECK(classified.has_value());
    if (classified) {
        RUVIA_CHECK(classified->kind == Http3PeerStreamKind::kQpackEncoder);
        RUVIA_CHECK_EQ(classified->consumed, std::size_t{1});
        RUVIA_CHECK_EQ(classified->remaining.size(), std::size_t{1});
        RUVIA_CHECK(classified->remaining.data() == typeAndPayload.data() + 2);
    }
    constexpr std::array<char, 1> later{'y'};
    const auto next = streams.feed(3, later);
    RUVIA_CHECK(next.has_value());
    if (next) {
        RUVIA_CHECK(next->kind == Http3PeerStreamKind::kQpackEncoder);
        RUVIA_CHECK_EQ(next->consumed, std::size_t{0});
        RUVIA_CHECK(next->remaining.data() == later.data());
    }
}

RUVIA_TEST(http3_peer_streams_enforces_critical_uniqueness_and_termination) {
    std::pmr::monotonic_buffer_resource resource;
    Http3PeerStreams streams(Http3PeerRole::kClient, &resource);
    constexpr std::array<char, 1> control{0};
    RUVIA_CHECK(streams.feed(3, control).has_value());
    const auto duplicate = streams.feed(7, control);
    RUVIA_CHECK(!duplicate);
    if (!duplicate) {
        RUVIA_CHECK(duplicate.error() == Http3PeerStreamError::kStreamCreationError);
    }
    const auto closed = streams.feed(3, {}, true);
    RUVIA_CHECK(!closed);
    if (!closed) {
        RUVIA_CHECK(closed.error() == Http3PeerStreamError::kClosedCriticalStream);
    }
    RUVIA_CHECK(streams.activeStreamCount() == std::size_t{1});
}

RUVIA_TEST(http3_peer_streams_tolerates_early_close_and_releases_noncritical_streams) {
    std::pmr::monotonic_buffer_resource resource;
    Http3PeerStreams streams(Http3PeerRole::kClient, &resource);
    const auto earlyFin = streams.feed(3, {}, true);
    RUVIA_CHECK(earlyFin.has_value());
    if (earlyFin) {
        RUVIA_CHECK(earlyFin->closed);
        RUVIA_CHECK_EQ(earlyFin->consumed, std::size_t{0});
    }

    constexpr std::array<char, 1> unknown{0x21};
    const auto accepted = streams.feed(3, unknown);
    RUVIA_CHECK(accepted.has_value());
    if (accepted) {
        RUVIA_CHECK(accepted->kind == Http3PeerStreamKind::kUnknown);
        RUVIA_CHECK_EQ(accepted->streamType, std::uint64_t{33});
    }
    const auto closed = streams.feed(3, {}, false, true);
    RUVIA_CHECK(closed.has_value());
    if (closed) {
        RUVIA_CHECK(closed->reset);
        RUVIA_CHECK(!closed->fin);
    }
    RUVIA_CHECK_EQ(streams.activeStreamCount(), std::size_t{0});
}

RUVIA_TEST(http3_peer_streams_rejects_critical_stream_closed_with_header_in_same_feed) {
    std::pmr::monotonic_buffer_resource resource;
    Http3PeerStreams streams(Http3PeerRole::kServer, &resource);
    constexpr std::array<char, 1> controlType{0};
    const auto closed = streams.feed(2, controlType, true);
    RUVIA_CHECK(!closed);
    if (!closed) {
        RUVIA_CHECK(closed.error() == Http3PeerStreamError::kClosedCriticalStream);
    }
    const auto reopened = streams.feed(6, controlType);
    RUVIA_CHECK(!reopened);
    if (!reopened) {
        RUVIA_CHECK(reopened.error() == Http3PeerStreamError::kStreamCreationError);
    }
}

RUVIA_TEST(http3_peer_streams_applies_role_and_active_stream_limit) {
    std::pmr::monotonic_buffer_resource resource;
    Http3PeerStreams server(Http3PeerRole::kServer, &resource, {.maxActiveStreams = 1});
    constexpr std::array<char, 1> push{1};
    const auto forbiddenPush = server.feed(2, push);
    RUVIA_CHECK(!forbiddenPush);
    if (!forbiddenPush) {
        RUVIA_CHECK(forbiddenPush.error() == Http3PeerStreamError::kStreamCreationError);
    }
    constexpr std::array<char, 1> unknown{0x21};
    RUVIA_CHECK(server.feed(2, unknown).has_value());
    const auto overLimit = server.feed(6, unknown);
    RUVIA_CHECK(!overLimit);
    if (!overLimit) {
        RUVIA_CHECK(overLimit.error() == Http3PeerStreamError::kExcessiveLoad);
    }
    RUVIA_CHECK(Http3PeerStreams::acceptBidirectional(Http3PeerRole::kServer, 0).has_value());
    const auto serverBidi = Http3PeerStreams::acceptBidirectional(Http3PeerRole::kClient, 1);
    RUVIA_CHECK(!serverBidi);
    if (!serverBidi) {
        RUVIA_CHECK(serverBidi.error() == Http3PeerStreamError::kStreamCreationError);
    }
    RUVIA_CHECK(Http3PeerStreams::acceptBidirectional(Http3PeerRole::kServer, 1).error() ==
                Http3PeerStreamError::kStreamCreationError);
}
