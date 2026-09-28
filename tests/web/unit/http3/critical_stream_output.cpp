#include <algorithm>
#include <cstddef>
#include <span>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamOutput.h"

#include "test_harness.h"

RUVIA_TEST(http3CriticalStreamOutputKeepsThreePrefixesIndependentUntilAccepted) {
    using Output = ruvia::detail::Http3CriticalStreamOutput;
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        return;
    }
    Output output(*prefixes);
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(!output.acknowledge(Output::Kind::kControl, 1));

    const auto control = output.next(Output::Kind::kControl);
    const auto encoder = output.next(Output::Kind::kQpackEncoder);
    const auto decoder = output.next(Output::Kind::kQpackDecoder);
    RUVIA_CHECK(control.size() > 2);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(control.front()), 0U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(encoder.front()), 2U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoder.front()), 3U);
    RUVIA_CHECK(!output.acknowledge(Output::Kind::kControl, control.size() + 1));
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, 0));
    RUVIA_CHECK(!output.queueGoaway(ruvia::kHttp3VarIntMax + 1));
    RUVIA_CHECK(!output.queueGoaway(17));
    RUVIA_CHECK(output.queueGoaway(16));
    RUVIA_CHECK(!output.queueGoaway(20));
    const auto retry = output.next(Output::Kind::kControl);
    RUVIA_CHECK(retry.data() == control.data() && retry.size() == control.size());
    RUVIA_CHECK(std::equal(retry.begin(), retry.end(), control.begin()));
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, 1));
    const auto remaining = output.next(Output::Kind::kControl);
    RUVIA_CHECK(remaining.data() == control.data() + 1);
    RUVIA_CHECK_EQ(remaining.size(), control.size() - 1);
    RUVIA_CHECK(!output.complete(Output::Kind::kControl));
    RUVIA_CHECK_EQ(output.next(Output::Kind::kQpackEncoder).data(), encoder.data());

    RUVIA_CHECK(output.acknowledge(Output::Kind::kQpackEncoder, encoder.size()));
    RUVIA_CHECK(output.acknowledge(Output::Kind::kQpackDecoder, decoder.size()));
    RUVIA_CHECK(output.complete(Output::Kind::kQpackEncoder));
    RUVIA_CHECK(output.complete(Output::Kind::kQpackDecoder));
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, remaining.size()));
    RUVIA_CHECK(!output.complete());
    const auto goaway = output.next(Output::Kind::kControl);
    const auto decoded = ruvia::decodeHttp3Frame(goaway);
    RUVIA_CHECK(decoded && decoded->type ==
                               static_cast<std::uint64_t>(ruvia::Http3FrameType::kGoaway));
    if (decoded) {
        const auto identifier = ruvia::decodeHttp3VarInt(decoded->payload);
        RUVIA_CHECK(identifier && identifier->value == 16);
        RUVIA_CHECK_EQ(decoded->encodedBytes, goaway.size());
    }
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, 0));
    const auto goawayRetry = output.next(Output::Kind::kControl);
    RUVIA_CHECK(goawayRetry.data() == goaway.data() && goawayRetry.size() == goaway.size());
    RUVIA_CHECK(std::equal(goawayRetry.begin(), goawayRetry.end(), goaway.begin()));
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, 1));
    const auto goawayRemaining = output.next(Output::Kind::kControl);
    RUVIA_CHECK(goawayRemaining.data() == goaway.data() + 1);
    RUVIA_CHECK_EQ(goawayRemaining.size(), goaway.size() - 1);
    RUVIA_CHECK(output.acknowledge(Output::Kind::kControl, goawayRemaining.size()));
    RUVIA_CHECK(output.complete());
    RUVIA_CHECK(output.next(Output::Kind::kControl).empty());
    RUVIA_CHECK(output.next(Output::Kind::kQpackEncoder).empty());
    RUVIA_CHECK(output.next(Output::Kind::kQpackDecoder).empty());
    RUVIA_CHECK(!output.acknowledge(Output::Kind::kControl, 0));
}
