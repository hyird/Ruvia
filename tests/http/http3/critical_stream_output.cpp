#include <algorithm>
#include <cstddef>
#include <span>
#include <variant>

#include "ruvia/http/http3_critical_stream_output.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_local_critical_streams.h"

#include "test_harness.h"

RUVIA_TEST(http3_critical_stream_output_keeps_three_prefixes_independent_until_accepted) {
    using output_type = ruvia::http3_critical_stream_output;
    const auto prefixes = ruvia::http3_local_critical_streams::create();
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    output_type output(std::get<0>(prefixes));
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(!output.acknowledge(output_type::stream_kind::control, 1));

    const auto control = output.next(output_type::stream_kind::control);
    const auto encoder = output.next(output_type::stream_kind::qpack_encoder);
    const auto decoder = output.next(output_type::stream_kind::qpack_decoder);
    RUVIA_CHECK(control.size() > 2);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(control.front()), 0U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(encoder.front()), 2U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(decoder.front()), 3U);
    RUVIA_CHECK(!output.acknowledge(output_type::stream_kind::control, control.size() + 1));
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, 0));
    RUVIA_CHECK(!output.queue_goaway(ruvia::http3_var_int_max + 1));
    RUVIA_CHECK(!output.queue_goaway(17));
    RUVIA_CHECK(output.queue_goaway(16));
    RUVIA_CHECK(!output.queue_goaway(20));
    const auto retry = output.next(output_type::stream_kind::control);
    RUVIA_CHECK(retry.data() == control.data() && retry.size() == control.size());
    RUVIA_CHECK(std::equal(retry.begin(), retry.end(), control.begin()));
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, 1));
    const auto remaining = output.next(output_type::stream_kind::control);
    RUVIA_CHECK(remaining.data() == control.data() + 1);
    RUVIA_CHECK_EQ(remaining.size(), control.size() - 1);
    RUVIA_CHECK(!output.complete(output_type::stream_kind::control));
    RUVIA_CHECK_EQ(output.next(output_type::stream_kind::qpack_encoder).data(), encoder.data());

    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::qpack_encoder, encoder.size()));
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::qpack_decoder, decoder.size()));
    RUVIA_CHECK(output.complete(output_type::stream_kind::qpack_encoder));
    RUVIA_CHECK(output.complete(output_type::stream_kind::qpack_decoder));
    RUVIA_CHECK(!output.complete());
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, remaining.size()));
    RUVIA_CHECK(!output.complete());
    const auto goaway = output.next(output_type::stream_kind::control);
    const auto decoded = ruvia::decode_http3_frame(goaway);
    RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).type_ ==
                                              static_cast<std::uint64_t>(ruvia::http3_frame_type::goaway));
    if ((decoded.index() == 0)) {
        const auto identifier = ruvia::decode_http3_var_int(std::get<0>(decoded).payload_);
        RUVIA_CHECK((identifier.index() == 0) && std::get<0>(identifier).value_ == 16);
        RUVIA_CHECK_EQ(std::get<0>(decoded).encoded_bytes_, goaway.size());
    }
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, 0));
    const auto goaway_retry = output.next(output_type::stream_kind::control);
    RUVIA_CHECK(goaway_retry.data() == goaway.data() && goaway_retry.size() == goaway.size());
    RUVIA_CHECK(std::equal(goaway_retry.begin(), goaway_retry.end(), goaway.begin()));
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, 1));
    const auto goaway_remaining = output.next(output_type::stream_kind::control);
    RUVIA_CHECK(goaway_remaining.data() == goaway.data() + 1);
    RUVIA_CHECK_EQ(goaway_remaining.size(), goaway.size() - 1);
    RUVIA_CHECK(output.acknowledge(output_type::stream_kind::control, goaway_remaining.size()));
    RUVIA_CHECK(output.complete());
    RUVIA_CHECK(output.next(output_type::stream_kind::control).empty());
    RUVIA_CHECK(output.next(output_type::stream_kind::qpack_encoder).empty());
    RUVIA_CHECK(output.next(output_type::stream_kind::qpack_decoder).empty());
    RUVIA_CHECK(!output.acknowledge(output_type::stream_kind::control, 0));
}
