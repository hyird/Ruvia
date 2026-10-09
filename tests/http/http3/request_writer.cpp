#include <array>
#include <string>
#include <variant>

#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_request_writer.h"

#include "test_harness.h"

RUVIA_TEST(http3_request_trailers_normalize_validate_and_use_connection_qpack) {
    const std::array fields_value{ruvia::http3_field_section_field_view{"X-Checksum", "hash"}};
    for (const bool dynamic : {false, true}) {
        ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 1});
        ruvia::http3_qpack_decoder decoder({.max_table_capacity_ = 256, .max_blocked_streams_ = 1});
        const auto bytes_value = dynamic ? ruvia::encode_http3_request_trailers(encoder, 0, fields_value) : ruvia::encode_http3_request_trailers(fields_value);
        RUVIA_CHECK((bytes_value.index() == 0));
        if ((bytes_value.index() != 0)) {
            continue;
        }
        RUVIA_CHECK((decoder.consume_encoder(encoder.pending_encoder_output())).index() == 0);
        std::string name, value;
        struct fields {
            std::string& name_;
            std::string& value_;
        } capture_value{name, value};
        const auto result_value = decoder.decode(0, std::get<0>(bytes_value), [](void* opaque, ruvia::http3_field_section_field_view field) {
            auto& captured_value=*static_cast<fields*>(opaque);captured_value.name_=field.name_;captured_value.value_=field.value_;return true; }, &capture_value);
        RUVIA_CHECK((result_value.index() == 0) && std::get<0>(result_value).status_ == ruvia::http3_qpack_decode_status::decoded);
        RUVIA_CHECK_EQ(name, std::string("x-checksum"));
        RUVIA_CHECK_EQ(value, std::string("hash"));
    }
    for (const auto name : {":status", "Host", "Content-Length", "Authorization", "Trailer", "Connection"}) {
        const std::array invalid{ruvia::http3_field_section_field_view{name, "value"}};
        RUVIA_CHECK((ruvia::encode_http3_request_trailers(invalid).index() != 0));
    }
    RUVIA_CHECK((ruvia::encode_http3_request_trailers(fields_value, {.max_encoded_bytes_ = 1}).index() != 0));
    RUVIA_CHECK((ruvia::encode_http3_request_trailers(fields_value, {.max_decoded_bytes_ = 1}).index() != 0));
    RUVIA_CHECK((ruvia::encode_http3_request_trailers(fields_value, {.max_fields_ = 0}).index() != 0));
    ruvia::http3_data_write_plan plan(ruvia::http3_client_request_body_plan{.expected_length_ = 3});
    const auto chunk = plan.plan_chunk(std::span("abc", 3), false);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK((plan.commit_payload(3, false)).index() == 0);
    RUVIA_CHECK(plan.fin_allowed());
    const auto finishing = plan.plan_chunk({}, true);
    RUVIA_CHECK((finishing.index() == 0) && !std::get<0>(finishing).emits_data_);
    RUVIA_CHECK((plan.commit_payload(0, true)).index() == 0);
    RUVIA_CHECK((plan.plan_chunk({}, false).index() != 0));
}
