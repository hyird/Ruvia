#include <array>
#include <memory_resource>

#include "ruvia/http/Http3ControlStream.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"

#include "test_harness.h"

RUVIA_TEST(http3_local_critical_streams_emit_independent_fragmentable_prefixes) {
    const auto created = ruvia::Http3LocalCriticalStreams::create(
        {.maxFieldSectionSize = 0});
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    const auto& streams = *created;
    const auto control = streams.controlPrefix();
    const auto encoder = streams.qpackEncoderPrefix();
    const auto decoder = streams.qpackDecoderPrefix();
    RUVIA_CHECK_EQ(control.size(), std::size_t{9});
    RUVIA_CHECK_EQ(encoder.size(), std::size_t{1});
    RUVIA_CHECK_EQ(decoder.size(), std::size_t{1});
    RUVIA_CHECK(control[0] == 0);  // Control stream type.

    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3PeerStreams peerStreams(ruvia::Http3PeerRole::kClient, &resource);
    const std::array<std::span<const char>, 3> prefixes{control, encoder, decoder};
    const std::array<std::uint64_t, 3> ids{3, 7, 11};
    const std::array<ruvia::Http3PeerStreamKind, 3> kinds{
        ruvia::Http3PeerStreamKind::kControl,
        ruvia::Http3PeerStreamKind::kQpackEncoder,
        ruvia::Http3PeerStreamKind::kQpackDecoder};
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        auto result = peerStreams.feed(ids[i], prefixes[i].first(1));
        RUVIA_CHECK(result.has_value());
        if (result) {
            RUVIA_CHECK(result->kind == kinds[i]);
            RUVIA_CHECK_EQ(result->consumed, std::size_t{1});
        }
        for (const char byte : prefixes[i].subspan(1)) {
            result = peerStreams.feed(ids[i], std::span<const char>(&byte, 1));
            RUVIA_CHECK(result.has_value());
            if (result) {
                RUVIA_CHECK(result->kind == kinds[i]);
            }
        }
    }

    ruvia::Http3ControlStream controlParser(ruvia::Http3ControlRole::kClient, &resource);
    for (const char byte : control.subspan(1)) {
        RUVIA_CHECK(controlParser.feed(std::span<const char>(&byte, 1), false) ==
                    ruvia::Http3ControlStreamStatus::kNeedMoreData);
    }
    RUVIA_CHECK(controlParser.peerSettings().has_value());
    if (controlParser.peerSettings()) {
        RUVIA_CHECK_EQ(controlParser.peerSettings()->qpackMaxTableCapacity, std::uint64_t{0});
        RUVIA_CHECK_EQ(controlParser.peerSettings()->qpackBlockedStreams, std::uint64_t{0});
        RUVIA_CHECK(controlParser.peerSettings()->maxFieldSectionSize == std::uint64_t{0});
    }
}

RUVIA_TEST(http3_local_critical_streams_encode_absent_and_explicit_zero_limits) {
    const auto defaults = ruvia::Http3LocalCriticalStreams::create();
    const auto explicitZero = ruvia::Http3LocalCriticalStreams::create(
        {.maxFieldSectionSize = 0});
    const auto fieldSectionLimit = ruvia::Http3LocalCriticalStreams::create(
        {.maxFieldSectionSize = 4096});
    RUVIA_CHECK(defaults.has_value());
    RUVIA_CHECK(explicitZero.has_value());
    RUVIA_CHECK(fieldSectionLimit.has_value());
    if (defaults && explicitZero && fieldSectionLimit) {
        RUVIA_CHECK_EQ(defaults->controlPrefix().size(), std::size_t{7});
        RUVIA_CHECK_EQ(explicitZero->controlPrefix().size(), std::size_t{9});
        RUVIA_CHECK_EQ(fieldSectionLimit->controlPrefix().size(), std::size_t{10});
        RUVIA_CHECK(defaults->controlPrefix().size() <= 1 + 16 + 48);
        RUVIA_CHECK(explicitZero->controlPrefix().size() <= 1 + 16 + 48);
        RUVIA_CHECK(fieldSectionLimit->controlPrefix().size() <= 1 + 16 + 48);
        RUVIA_CHECK_EQ(defaults->qpackEncoderPrefix().size(), std::size_t{1});
        RUVIA_CHECK_EQ(defaults->qpackDecoderPrefix().size(), std::size_t{1});

        std::pmr::monotonic_buffer_resource resource;
        ruvia::Http3ControlStream parser(ruvia::Http3ControlRole::kClient, &resource);
        const auto prefix = fieldSectionLimit->controlPrefix();
        RUVIA_CHECK(parser.feed(prefix.subspan(1), false) ==
                    ruvia::Http3ControlStreamStatus::kNeedMoreData);
        RUVIA_CHECK(parser.peerSettings().has_value());
        if (parser.peerSettings()) {
            RUVIA_CHECK(parser.peerSettings()->maxFieldSectionSize == std::uint64_t{4096});
        }
    }
}

RUVIA_TEST(http3_local_critical_streams_reject_nonzero_qpack_limits) {
    const auto capacity = ruvia::Http3LocalCriticalStreams::create(
        {.qpackMaxTableCapacity = 1});
    const auto blocked = ruvia::Http3LocalCriticalStreams::create(
        {.qpackBlockedStreams = 1});
    RUVIA_CHECK(!capacity.has_value());
    RUVIA_CHECK(!blocked.has_value());
    if (!capacity) {
        RUVIA_CHECK(capacity.error() ==
                    ruvia::Http3LocalCriticalStreamsError::kUnsupportedQpackConfiguration);
    }
    if (!blocked) {
        RUVIA_CHECK(blocked.error() ==
                    ruvia::Http3LocalCriticalStreamsError::kUnsupportedQpackConfiguration);
    }
}
