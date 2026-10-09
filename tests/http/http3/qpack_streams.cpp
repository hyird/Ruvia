#include <array>
#include <span>
#include <variant>

#include "ruvia/http/Http3QpackStreams.h"

#include "test_harness.h"

RUVIA_TEST(http3_qpack_encoder_stream_accepts_zero_capacity_and_empty_fragments) {
    ruvia::Http3QpackEncoderStreamValidator validator;
    const std::array<char, 1> capacityZero{static_cast<char>(0x20)};
    RUVIA_CHECK((validator.consume(capacityZero).index() == 0));
    RUVIA_CHECK((validator.consume({}).index() == 0));
    RUVIA_CHECK((validator.consume(capacityZero).index() == 0));
}

RUVIA_TEST(http3_qpack_encoder_stream_rejects_nonzero_capacity_and_mutations) {
    for (const char instruction : {static_cast<char>(0x21), static_cast<char>(0x80),
             static_cast<char>(0x40), static_cast<char>(0x00)}) {
        ruvia::Http3QpackEncoderStreamValidator validator;
        const std::array<char, 1> bytes{instruction};
        const auto result = validator.consume(bytes);
        RUVIA_CHECK(!(result.index() == 0));
        if ((result.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result) == ruvia::Http3QpackStreamError::kEncoderStreamError);
        }
    }
}

RUVIA_TEST(http3_qpack_decoder_stream_rejects_dynamic_state_instructions) {
    for (const char instruction : {static_cast<char>(0x80), static_cast<char>(0x00)}) {
        ruvia::Http3QpackDecoderStreamValidator validator;
        const std::array<char, 1> bytes{instruction};
        const auto result = validator.consume(bytes);
        RUVIA_CHECK(!(result.index() == 0));
        if ((result.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result) == ruvia::Http3QpackStreamError::kDecoderStreamError);
        }
    }
    ruvia::Http3QpackDecoderStreamValidator empty;
    RUVIA_CHECK((empty.consume({}).index() == 0));
}

RUVIA_TEST(http3_qpack_decoder_stream_accepts_cancellation_without_dynamic_references) {
    ruvia::Http3QpackDecoderStreamValidator decoder;
    constexpr std::array<char, 2> cancellations{0x48, 0x40};
    RUVIA_CHECK((decoder.consume(cancellations).index() == 0));
    constexpr std::array<char, 1> extendedIdPrefix{0x7f};
    constexpr std::array<char, 1> extendedIdSuffix{0x01};
    RUVIA_CHECK((decoder.consume(extendedIdPrefix).index() == 0));
    RUVIA_CHECK((decoder.consume(extendedIdSuffix).index() == 0));
    RUVIA_CHECK((decoder.consume({}, false).index() == 0));
}

RUVIA_TEST(http3_qpack_stream_fin_is_critical_closure) {
    ruvia::Http3QpackEncoderStreamValidator encoder;
    ruvia::Http3QpackDecoderStreamValidator decoder;
    const auto encoderFin = encoder.consume({}, true);
    const auto decoderFin = decoder.consume({}, true);
    RUVIA_CHECK(!(encoderFin.index() == 0));
    RUVIA_CHECK(!(decoderFin.index() == 0));
    if ((encoderFin.index() != 0)) {
        RUVIA_CHECK(std::get<1>(encoderFin) == ruvia::Http3QpackStreamError::kClosedCriticalStream);
    }
    if ((decoderFin.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decoderFin) == ruvia::Http3QpackStreamError::kClosedCriticalStream);
    }
}

RUVIA_TEST(http3_qpack_streams_reject_invalid_instruction_on_first_byte) {
    ruvia::Http3QpackEncoderStreamValidator encoder;
    constexpr std::array<char, 1> extendedCapacity{static_cast<char>(0x3f)};
    RUVIA_CHECK(std::get<1>(encoder.consume(extendedCapacity)) ==
                ruvia::Http3QpackStreamError::kEncoderStreamError);

    ruvia::Http3QpackDecoderStreamValidator decoder;
    constexpr std::array<char, 1> extendedAck{static_cast<char>(0xff)};
    RUVIA_CHECK(std::get<1>(decoder.consume(extendedAck)) ==
                ruvia::Http3QpackStreamError::kDecoderStreamError);

    ruvia::Http3QpackDecoderStreamValidator unfinished;
    constexpr std::array<char, 1> cancellationPrefix{0x7f};
    RUVIA_CHECK((unfinished.consume(cancellationPrefix).index() == 0));
    RUVIA_CHECK(std::get<1>(unfinished.consume({}, true)) ==
                ruvia::Http3QpackStreamError::kDecoderStreamError);
}
