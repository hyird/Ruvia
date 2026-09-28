#include <array>
#include <span>

#include "ruvia/http/Http3QpackStreams.h"

#include "test_harness.h"

RUVIA_TEST(http3_qpack_encoder_stream_accepts_zero_capacity_and_empty_fragments) {
    ruvia::Http3QpackEncoderStreamValidator validator;
    const std::array<char, 1> capacityZero{static_cast<char>(0x20)};
    RUVIA_CHECK(validator.consume(capacityZero).has_value());
    RUVIA_CHECK(validator.consume({}).has_value());
    RUVIA_CHECK(validator.consume(capacityZero).has_value());
}

RUVIA_TEST(http3_qpack_encoder_stream_rejects_nonzero_capacity_and_mutations) {
    for (const char instruction : {static_cast<char>(0x21), static_cast<char>(0x80),
             static_cast<char>(0x40), static_cast<char>(0x00)}) {
        ruvia::Http3QpackEncoderStreamValidator validator;
        const std::array<char, 1> bytes{instruction};
        const auto result = validator.consume(bytes);
        RUVIA_CHECK(!result.has_value());
        if (!result) {
            RUVIA_CHECK(result.error() == ruvia::Http3QpackStreamError::kEncoderStreamError);
        }
    }
}

RUVIA_TEST(http3_qpack_decoder_stream_rejects_dynamic_state_instructions) {
    for (const char instruction : {static_cast<char>(0x80), static_cast<char>(0x00)}) {
        ruvia::Http3QpackDecoderStreamValidator validator;
        const std::array<char, 1> bytes{instruction};
        const auto result = validator.consume(bytes);
        RUVIA_CHECK(!result.has_value());
        if (!result) {
            RUVIA_CHECK(result.error() == ruvia::Http3QpackStreamError::kDecoderStreamError);
        }
    }
    ruvia::Http3QpackDecoderStreamValidator empty;
    RUVIA_CHECK(empty.consume({}).has_value());
}

RUVIA_TEST(http3_qpack_decoder_stream_accepts_cancellation_without_dynamic_references) {
    ruvia::Http3QpackDecoderStreamValidator decoder;
    constexpr std::array<char, 2> cancellations{0x48, 0x40};
    RUVIA_CHECK(decoder.consume(cancellations).has_value());
    constexpr std::array<char, 1> extendedIdPrefix{0x7f};
    constexpr std::array<char, 1> extendedIdSuffix{0x01};
    RUVIA_CHECK(decoder.consume(extendedIdPrefix).has_value());
    RUVIA_CHECK(decoder.consume(extendedIdSuffix).has_value());
    RUVIA_CHECK(decoder.consume({}, false).has_value());
}

RUVIA_TEST(http3_qpack_stream_fin_is_critical_closure) {
    ruvia::Http3QpackEncoderStreamValidator encoder;
    ruvia::Http3QpackDecoderStreamValidator decoder;
    const auto encoderFin = encoder.consume({}, true);
    const auto decoderFin = decoder.consume({}, true);
    RUVIA_CHECK(!encoderFin.has_value());
    RUVIA_CHECK(!decoderFin.has_value());
    if (!encoderFin) {
        RUVIA_CHECK(encoderFin.error() == ruvia::Http3QpackStreamError::kClosedCriticalStream);
    }
    if (!decoderFin) {
        RUVIA_CHECK(decoderFin.error() == ruvia::Http3QpackStreamError::kClosedCriticalStream);
    }
}

RUVIA_TEST(http3_qpack_streams_reject_invalid_instruction_on_first_byte) {
    ruvia::Http3QpackEncoderStreamValidator encoder;
    constexpr std::array<char, 1> extendedCapacity{static_cast<char>(0x3f)};
    RUVIA_CHECK(encoder.consume(extendedCapacity).error() ==
                ruvia::Http3QpackStreamError::kEncoderStreamError);

    ruvia::Http3QpackDecoderStreamValidator decoder;
    constexpr std::array<char, 1> extendedAck{static_cast<char>(0xff)};
    RUVIA_CHECK(decoder.consume(extendedAck).error() ==
                ruvia::Http3QpackStreamError::kDecoderStreamError);

    ruvia::Http3QpackDecoderStreamValidator unfinished;
    constexpr std::array<char, 1> cancellationPrefix{0x7f};
    RUVIA_CHECK(unfinished.consume(cancellationPrefix).has_value());
    RUVIA_CHECK(unfinished.consume({}, true).error() ==
                ruvia::Http3QpackStreamError::kDecoderStreamError);
}
