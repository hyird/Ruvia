#include <algorithm>
#include <cstddef>
#include <span>
#include <variant>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/http3_critical_stream_output.h"

#include "test_harness.h"

RUVIA_TEST(http3CriticalStreamOutputKeepsThreePrefixesIndependentUntilAccepted) {
    using Output = ruvia::http3_critical_stream_output;
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    Output output(std::get<0>(prefixes));
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(!output.acknowledge(Output::stream_kind::control, 1));

    const auto control = output.next(Output::stream_kind::control);
    const auto encoder = output.next(Output::stream_kind::qpack_encoder);
    const auto decoder = output.next(Output::stream_kind::qpack_decoder);
    RUVIA_CHECK(control.size() > 2);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(control.front()), 0U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(encoder.front()), 2U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoder.front()), 3U);
    RUVIA_CHECK(!output.acknowledge(Output::stream_kind::control, control.size() + 1));
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, 0));
    RUVIA_CHECK(!output.queue_goaway(ruvia::kHttp3VarIntMax + 1));
    RUVIA_CHECK(!output.queue_goaway(17));
    RUVIA_CHECK(output.queue_goaway(16));
    RUVIA_CHECK(!output.queue_goaway(20));
    const auto retry = output.next(Output::stream_kind::control);
    RUVIA_CHECK(retry.data() == control.data() && retry.size() == control.size());
    RUVIA_CHECK(std::equal(retry.begin(), retry.end(), control.begin()));
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, 1));
    const auto remaining = output.next(Output::stream_kind::control);
    RUVIA_CHECK(remaining.data() == control.data() + 1);
    RUVIA_CHECK_EQ(remaining.size(), control.size() - 1);
    RUVIA_CHECK(!output.complete(Output::stream_kind::control));
    RUVIA_CHECK_EQ(output.next(Output::stream_kind::qpack_encoder).data(), encoder.data());

    RUVIA_CHECK(output.acknowledge(Output::stream_kind::qpack_encoder, encoder.size()));
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::qpack_decoder, decoder.size()));
    RUVIA_CHECK(output.complete(Output::stream_kind::qpack_encoder));
    RUVIA_CHECK(output.complete(Output::stream_kind::qpack_decoder));
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, remaining.size()));
    RUVIA_CHECK(!output.complete());
    const auto goaway = output.next(Output::stream_kind::control);
    const auto decoded = ruvia::decodeHttp3Frame(goaway);
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).type ==
                                              static_cast<std::uint64_t>(ruvia::Http3FrameType::kGoaway));
    if ((decoded.index() == 0)) {
        const auto identifier = ruvia::decodeHttp3VarInt(std::get<0>(decoded).payload);
        RUVIA_CHECK((identifier.index() == 0) && std::get<0>(identifier).value == 16);
        RUVIA_CHECK_EQ(std::get<0>(decoded).encodedBytes, goaway.size());
    }
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, 0));
    const auto goawayRetry = output.next(Output::stream_kind::control);
    RUVIA_CHECK(goawayRetry.data() == goaway.data() && goawayRetry.size() == goaway.size());
    RUVIA_CHECK(std::equal(goawayRetry.begin(), goawayRetry.end(), goaway.begin()));
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, 1));
    const auto goawayRemaining = output.next(Output::stream_kind::control);
    RUVIA_CHECK(goawayRemaining.data() == goaway.data() + 1);
    RUVIA_CHECK_EQ(goawayRemaining.size(), goaway.size() - 1);
    RUVIA_CHECK(output.acknowledge(Output::stream_kind::control, goawayRemaining.size()));
    RUVIA_CHECK(output.complete());
    RUVIA_CHECK(output.next(Output::stream_kind::control).empty());
    RUVIA_CHECK(output.next(Output::stream_kind::qpack_encoder).empty());
    RUVIA_CHECK(output.next(Output::stream_kind::qpack_decoder).empty());
    RUVIA_CHECK(!output.acknowledge(Output::stream_kind::control, 0));
}
