#include <array>
#include <memory_resource>
#include <variant>

#include "ruvia/http/Http3ControlStream.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"

#include "test_harness.h"

RUVIA_TEST(http3_local_critical_streams_emit_independent_fragmentable_prefixes) {
    const auto created = ruvia::Http3LocalCriticalStreams::create(
        {.maxFieldSectionSize = 0});
    RUVIA_CHECK((created.index() == 0));
    if ((created.index() != 0)) {
        return;
    }
    const auto& streams = std::get<0>(created);
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
        RUVIA_CHECK((result.index() == 0));
        if ((result.index() == 0)) {
            RUVIA_CHECK(std::get<0>(result).kind == kinds[i]);
            RUVIA_CHECK_EQ(std::get<0>(result).consumed, std::size_t{1});
        }
        for (const char byte : prefixes[i].subspan(1)) {
            result = peerStreams.feed(ids[i], std::span<const char>(&byte, 1));
            RUVIA_CHECK((result.index() == 0));
            if ((result.index() == 0)) {
                RUVIA_CHECK(std::get<0>(result).kind == kinds[i]);
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
    const auto connectEnabled = ruvia::Http3LocalCriticalStreams::create(
        {.enableConnectProtocol = true});
    RUVIA_CHECK((defaults.index() == 0));
    RUVIA_CHECK((explicitZero.index() == 0));
    RUVIA_CHECK((fieldSectionLimit.index() == 0));
    RUVIA_CHECK((connectEnabled.index() == 0));
    if ((defaults.index() == 0) && (explicitZero.index() == 0) && (fieldSectionLimit.index() == 0) && (connectEnabled.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(defaults).controlPrefix().size(), std::size_t{7});
        RUVIA_CHECK_EQ(std::get<0>(explicitZero).controlPrefix().size(), std::size_t{9});
        RUVIA_CHECK_EQ(std::get<0>(fieldSectionLimit).controlPrefix().size(), std::size_t{10});
        RUVIA_CHECK_EQ(std::get<0>(connectEnabled).controlPrefix().size(), std::size_t{9});
        RUVIA_CHECK(std::get<0>(defaults).controlPrefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(explicitZero).controlPrefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(fieldSectionLimit).controlPrefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK(std::get<0>(connectEnabled).controlPrefix().size() <= 1 + 16 + 64);
        RUVIA_CHECK_EQ(std::get<0>(defaults).qpackEncoderPrefix().size(), std::size_t{1});
        RUVIA_CHECK_EQ(std::get<0>(defaults).qpackDecoderPrefix().size(), std::size_t{1});

        std::pmr::monotonic_buffer_resource resource;
        ruvia::Http3ControlStream parser(ruvia::Http3ControlRole::kClient, &resource);
        const auto prefix = std::get<0>(fieldSectionLimit).controlPrefix();
        RUVIA_CHECK(parser.feed(prefix.subspan(1), false) ==
                    ruvia::Http3ControlStreamStatus::kNeedMoreData);
        RUVIA_CHECK(parser.peerSettings().has_value());
        if (parser.peerSettings()) {
            RUVIA_CHECK(parser.peerSettings()->maxFieldSectionSize == std::uint64_t{4096});
        }

        ruvia::Http3ControlStream connectParser(ruvia::Http3ControlRole::kClient, &resource);
        const auto connectPrefix = std::get<0>(connectEnabled).controlPrefix();
        RUVIA_CHECK(connectParser.feed(connectPrefix.subspan(1), false) ==
                    ruvia::Http3ControlStreamStatus::kNeedMoreData);
        RUVIA_CHECK(connectParser.peerSettings().has_value());
        if (connectParser.peerSettings()) {
            RUVIA_CHECK(connectParser.peerSettings()->enableConnectProtocol);
        }
    }
}

RUVIA_TEST(http3_local_critical_streams_advertise_dynamic_qpack_and_datagrams) {
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create(
        {.qpackMaxTableCapacity = 4096, .qpackBlockedStreams = 16, .enableConnectProtocol = true, .h3Datagram = true});
    RUVIA_CHECK((prefixes.index() == 0));
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http3ControlStream control(ruvia::Http3ControlRole::kClient, &resource);
    RUVIA_CHECK(control.feed(std::get<0>(prefixes).controlPrefix().subspan(1), false) == ruvia::Http3ControlStreamStatus::kNeedMoreData);
    RUVIA_CHECK(control.peerSettings().has_value());
    RUVIA_CHECK_EQ(control.peerSettings()->qpackMaxTableCapacity, 4096u);
    RUVIA_CHECK_EQ(control.peerSettings()->qpackBlockedStreams, 16u);
    RUVIA_CHECK(control.peerSettings()->h3Datagram);
}
