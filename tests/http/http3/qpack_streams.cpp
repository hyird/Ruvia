#include <array>
#include <span>
#include <variant>

#include "ruvia/http/http3_qpack_streams.h"

#include "test_harness.h"

RUVIA_TEST(http3_qpack_encoder_stream_accepts_zero_capacity_and_empty_fragments) {
    ruvia::http3_qpack_encoder_stream_validator validator;
    const std::array<char, 1> capacity_zero{static_cast<char>(0x20)};
    RUVIA_CHECK((validator.consume(capacity_zero).index() == 0));
    RUVIA_CHECK((validator.consume({}).index() == 0));
    RUVIA_CHECK((validator.consume(capacity_zero).index() == 0));
}

RUVIA_TEST(http3_qpack_encoder_stream_rejects_nonzero_capacity_and_mutations) {
    for (const char instruction : {static_cast<char>(0x21), static_cast<char>(0x80),
             static_cast<char>(0x40), static_cast<char>(0x00)}) {
        ruvia::http3_qpack_encoder_stream_validator validator;
        const std::array<char, 1> bytes_value{instruction};
        const auto result_value = validator.consume(bytes_value);
        RUVIA_CHECK(!(result_value.index() == 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value) == ruvia::http3_qpack_stream_error::encoder_stream_error);
        }
    }
}

RUVIA_TEST(http3_qpack_decoder_stream_rejects_dynamic_state_instructions) {
    for (const char instruction : {static_cast<char>(0x80), static_cast<char>(0x00)}) {
        ruvia::http3_qpack_decoder_stream_validator validator;
        const std::array<char, 1> bytes_value{instruction};
        const auto result_value = validator.consume(bytes_value);
        RUVIA_CHECK(!(result_value.index() == 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value) == ruvia::http3_qpack_stream_error::decoder_stream_error);
        }
    }
    ruvia::http3_qpack_decoder_stream_validator empty;
    RUVIA_CHECK((empty.consume({}).index() == 0));
}

RUVIA_TEST(http3_qpack_decoder_stream_accepts_cancellation_without_dynamic_references) {
    ruvia::http3_qpack_decoder_stream_validator decoder;
    constexpr std::array<char, 2> cancellations{0x48, 0x40};
    RUVIA_CHECK((decoder.consume(cancellations).index() == 0));
    constexpr std::array<char, 1> extended_id_prefix{0x7f};
    constexpr std::array<char, 1> extended_id_suffix{0x01};
    RUVIA_CHECK((decoder.consume(extended_id_prefix).index() == 0));
    RUVIA_CHECK((decoder.consume(extended_id_suffix).index() == 0));
    RUVIA_CHECK((decoder.consume({}, false).index() == 0));
}

RUVIA_TEST(http3_qpack_stream_fin_is_critical_closure) {
    ruvia::http3_qpack_encoder_stream_validator encoder;
    ruvia::http3_qpack_decoder_stream_validator decoder;
    const auto encoder_fin = encoder.consume({}, true);
    const auto decoder_fin = decoder.consume({}, true);
    RUVIA_CHECK(!(encoder_fin.index() == 0));
    RUVIA_CHECK(!(decoder_fin.index() == 0));
    if ((encoder_fin.index() != 0)) {
        RUVIA_CHECK(std::get<1>(encoder_fin) == ruvia::http3_qpack_stream_error::closed_critical_stream);
    }
    if ((decoder_fin.index() != 0)) {
        RUVIA_CHECK(std::get<1>(decoder_fin) == ruvia::http3_qpack_stream_error::closed_critical_stream);
    }
}

RUVIA_TEST(http3_qpack_streams_reject_invalid_instruction_on_first_byte) {
    ruvia::http3_qpack_encoder_stream_validator encoder;
    constexpr std::array<char, 1> extended_capacity{static_cast<char>(0x3f)};
    RUVIA_CHECK(std::get<1>(encoder.consume(extended_capacity)) ==
                ruvia::http3_qpack_stream_error::encoder_stream_error);

    ruvia::http3_qpack_decoder_stream_validator decoder;
    constexpr std::array<char, 1> extended_ack{static_cast<char>(0xff)};
    RUVIA_CHECK(std::get<1>(decoder.consume(extended_ack)) ==
                ruvia::http3_qpack_stream_error::decoder_stream_error);

    ruvia::http3_qpack_decoder_stream_validator unfinished;
    constexpr std::array<char, 1> cancellation_prefix{0x7f};
    RUVIA_CHECK((unfinished.consume(cancellation_prefix).index() == 0));
    RUVIA_CHECK(std::get<1>(unfinished.consume({}, true)) ==
                ruvia::http3_qpack_stream_error::decoder_stream_error);
}
