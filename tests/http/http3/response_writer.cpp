#include <algorithm>
#include <array>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_response_writer.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

namespace {
struct fields final {
    std::vector<std::string> names_;
    std::vector<std::string> values_;
};

bool collect(void* opaque, ruvia::http3_field_section_field_view field) {
    auto& fields_value = *static_cast<fields*>(opaque);
    fields_value.names_.emplace_back(field.name_);
    fields_value.values_.emplace_back(field.value_);
    return true;
}

class counting_resource final : public std::pmr::memory_resource {
public:
    explicit counting_resource(const void* equality_group = nullptr)
        : equality_group_(equality_group ? equality_group : this) {}

    std::size_t allocations_{};
    std::size_t deallocations_{};

    bool fail_allocations_{};
    std::optional<std::size_t> fail_after_allocations_;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_allocations_ || (fail_after_allocations_ && allocations_ >= *fail_after_allocations_)) {
            throw std::bad_alloc();
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        return result_value;
    }
    void do_deallocate(void* ptr, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* resource = dynamic_cast<const counting_resource*>(&other);
        return resource && equality_group_ == resource->equality_group_;
    }
    const void* equality_group_;
};
}  // namespace

RUVIA_TEST(http3_response_head_forms_release_storage_after_each_allocation_failure) {
    enum class form { raw,
        buffered,
        interim,
        streaming };
    const std::string long_value(80, 'v');
    const std::array headers{
        ruvia::http_header_view{"Date", "Fri, 09 Oct 2026 00:00:00 GMT"},
        ruvia::http_header_view{"X-Mixed-Name", long_value},
        ruvia::http_header_view{"Another-Field", "second"}};
    const std::array raw_fields{
        ruvia::http3_field_section_field_view{"date", headers[0].value()},
        ruvia::http3_field_section_field_view{"x-mixed-name", long_value},
        ruvia::http3_field_section_field_view{"another-field", "second"}};

    for (const auto kind : {form::raw, form::buffered, form::interim, form::streaming}) {
        for (const bool connection_owned : {false, true}) {
            const auto encode = [&](counting_resource& memory) -> std::variant<ruvia::http3_response_head, ruvia::http3_response_head_failure> {
                ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 0, .max_blocked_streams_ = 0});
                constexpr auto method = ruvia::http_known_method::get;
                if (kind == form::raw) {
                    return connection_owned
                               ? ruvia::encode_http3_response_head(encoder, 0, ruvia::http_status::ok, method, raw_fields, {}, &memory)
                               : ruvia::encode_http3_response_head(ruvia::http_status::ok, method, raw_fields, {}, &memory);
                }
                if (kind == form::interim) {
                    const ruvia::http_interim_response_head response(ruvia::http_status::early_hints, headers);
                    return connection_owned
                               ? ruvia::encode_http3_interim_response_head(encoder, 0, response, {}, &memory)
                               : ruvia::encode_http3_interim_response_head(response, {}, &memory);
                }
                ruvia::http_response response;
                for (const auto& header : headers) {
                    response.header(header.name(), header.value());
                }
                if (kind == form::buffered) {
                    response.static_body("body");
                    const auto plan = ruvia::plan_buffered_http_response_write(method, response);
                    return connection_owned
                               ? ruvia::encode_http3_response_head(encoder, 0, response, plan, {}, &memory)
                               : ruvia::encode_http3_response_head(response, plan, {}, &memory);
                }
                auto result_value = connection_owned
                                        ? ruvia::encode_http3_streaming_response_head(encoder, 0, std::move(response), method,
                                              ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none, {}, &memory)
                                        : ruvia::encode_http3_streaming_response_head(std::move(response), method,
                                              ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none, {}, &memory);
                if ((result_value.index() != 0)) {
                    return std::get<1>(result_value);
                }
                return std::move(std::get<0>(result_value).head_);
            };
            counting_resource complete;
            {
                const auto result_value = encode(complete);
                RUVIA_CHECK((result_value.index() == 0));
                if ((result_value.index() == 0)) {
                    fields decoded;
                    RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(result_value).headers_.field_section(), collect, &decoded).index() == 0));
                    const auto count = kind == form::buffered ? 5U : 4U;
                    RUVIA_CHECK_EQ(decoded.names_.size(), count);
                    if (decoded.names_.size() == count) {
                        RUVIA_CHECK_EQ(decoded.names_[0], ":status");
                        RUVIA_CHECK_EQ(decoded.values_[0], kind == form::interim ? "103" : "200");
                        for (std::size_t index = 0; index < raw_fields.size(); ++index) {
                            RUVIA_CHECK_EQ(decoded.names_[index + 1], raw_fields[index].name_);
                            RUVIA_CHECK_EQ(decoded.values_[index + 1], raw_fields[index].value_);
                        }
                        if (kind == form::buffered) {
                            RUVIA_CHECK_EQ(decoded.names_.back(), "content-length");
                            RUVIA_CHECK_EQ(decoded.values_.back(), "4");
                        }
                    }
                }
            }
            RUVIA_CHECK_EQ(complete.allocations_, complete.deallocations_);
            for (std::size_t failure = 0; failure < complete.allocations_; ++failure) {
                counting_resource limited;
                limited.fail_after_allocations_ = failure;
                bool threw = false;
                try {
                    (void)encode(limited);
                } catch (const std::bad_alloc&) {
                    threw = true;
                }
                RUVIA_CHECK(threw);
                RUVIA_CHECK_EQ(limited.allocations_, limited.deallocations_);
            }
        }
    }
}

RUVIA_TEST(http3_response_head_field_errors_precede_aggregate_size_errors_without_qpack_mutation) {
    const auto check = [&](ruvia::http3_field_section_field_view invalid, ruvia::http3_response_head_error expected) {
        const std::array fields_value{
            ruvia::http3_field_section_field_view{"x-first", "new-dynamic-value"},
            invalid};
        ruvia::http3_field_section_limits limits;
        limits.max_decoded_bytes_ = 0;
        for (const bool connection_owned : {false, true}) {
            ruvia::http3_qpack_encoder encoder({});
            const auto initial_output = encoder.pending_encoder_output();
            const std::vector<char> pending_before(initial_output.begin(), initial_output.end());
            const auto result_value = connection_owned
                                          ? ruvia::encode_http3_response_head(encoder, 0, ruvia::http_status::ok,
                                                ruvia::http_known_method::get, fields_value, limits)
                                          : ruvia::encode_http3_response_head(ruvia::http_status::ok,
                                                ruvia::http_known_method::get, fields_value, limits);
            RUVIA_CHECK((result_value.index() != 0));
            if ((result_value.index() != 0)) {
                RUVIA_CHECK(std::get<1>(result_value).kind_ == expected);
            }
            RUVIA_CHECK_EQ(encoder.insert_count(), 0U);
            RUVIA_CHECK(std::ranges::equal(encoder.pending_encoder_output(), pending_before));
        }
    };
    check({"bad name", "value"}, ruvia::http3_response_head_error::invalid_field);
    check({"x-value", "bad\r\nvalue"}, ruvia::http3_response_head_error::invalid_field);
    check({"connection", "close"}, ruvia::http3_response_head_error::forbidden_field);
    check({"content-length", "invalid"}, ruvia::http3_response_head_error::invalid_field);
}

RUVIA_TEST(http3_interim_and_streaming_heads_project_mixed_case_names_in_order) {
    const std::array headers{
        ruvia::http_header_view{"X-Long-Mixed-Case-Header", "first"},
        ruvia::http_header_view{"x-lowercase-header-name", "lower"},
        ruvia::http_header_view{"X-Long-Mixed-Case-Header", "second"}};
    const ruvia::http_interim_response_head interim(ruvia::http_status::early_hints, headers);
    counting_resource interim_resource;
    {
        const auto result_value = ruvia::encode_http3_interim_response_head(interim, {}, &interim_resource);
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            fields decoded;
            RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(result_value).headers_.field_section(), collect, &decoded).index() == 0));
            RUVIA_CHECK_EQ(decoded.names_.size(), 4U);
            if (decoded.names_.size() == 4) {
                RUVIA_CHECK_EQ(decoded.names_[0], ":status");
                RUVIA_CHECK_EQ(decoded.values_[0], "103");
                RUVIA_CHECK_EQ(decoded.names_[1], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values_[1], "first");
                RUVIA_CHECK_EQ(decoded.names_[2], "x-lowercase-header-name");
                RUVIA_CHECK_EQ(decoded.values_[2], "lower");
                RUVIA_CHECK_EQ(decoded.names_[3], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values_[3], "second");
            }
        }
    }
    RUVIA_CHECK(interim_resource.allocations_ > 0);
    RUVIA_CHECK_EQ(interim_resource.allocations_, interim_resource.deallocations_);

    counting_resource streaming_resource;
    {
        ruvia::http_response response;
        response.header("X-Long-Mixed-Case-Header", "first");
        response.header("x-lowercase-header-name", "lower");
        response.header("X-Long-Mixed-Case-Header", "second",
            {.mode_ = ruvia::http_response_header_mode::append});
        const auto result_value = ruvia::encode_http3_streaming_response_head(std::move(response),
            ruvia::http_known_method::get, ruvia::http_response_stream_kind::generic,
            ruvia::http_response_trailer_intent::none, {}, &streaming_resource);
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            fields decoded;
            RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(result_value).head_.headers_.field_section(), collect, &decoded).index() == 0));
            RUVIA_CHECK_EQ(decoded.names_.size(), 5U);
            if (decoded.names_.size() == 5) {
                RUVIA_CHECK_EQ(decoded.names_[0], ":status");
                RUVIA_CHECK_EQ(decoded.values_[0], "200");
                RUVIA_CHECK_EQ(decoded.names_[1], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values_[1], "first");
                RUVIA_CHECK_EQ(decoded.names_[2], "x-lowercase-header-name");
                RUVIA_CHECK_EQ(decoded.values_[2], "lower");
                RUVIA_CHECK_EQ(decoded.names_[3], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values_[3], "second");
                RUVIA_CHECK_EQ(decoded.names_[4], "date");
            }
        }
    }
    RUVIA_CHECK(streaming_resource.allocations_ > 0);
    RUVIA_CHECK_EQ(streaming_resource.allocations_, streaming_resource.deallocations_);
}

RUVIA_TEST(http3_streaming_head_preserves_length_projects_sse_and_trailer_semantics) {
    counting_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Content-Length", "17");
        auto head = ruvia::encode_http3_streaming_response_head(std::move(response), ruvia::http_known_method::get,
            ruvia::http_response_stream_kind::sse, ruvia::http_response_trailer_intent::present, {}, &resource);
        RUVIA_CHECK((head.index() == 0));
        if ((head.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(head).head_.declared_content_length_.value_or(0), 17U);
            RUVIA_CHECK(std::get<0>(head).commit_plan_.head_disposition() == ruvia::http_response_stream_head_disposition::body_open);
            RUVIA_CHECK(std::get<0>(head).commit_plan_.trailer_framing() == ruvia::http_response_stream_trailer_framing::http3_trailing_headers);
            fields fields;
            RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(head).head_.headers_.field_section(), collect, &fields).index() == 0));
            RUVIA_CHECK(std::ranges::find(fields.values_, "text/event-stream") != fields.values_.end());
            RUVIA_CHECK(std::ranges::find(fields.values_, "no-store") != fields.values_.end());
            RUVIA_CHECK(std::ranges::find(fields.names_, "date") != fields.names_.end());
            RUVIA_CHECK(std::ranges::find(fields.names_, "transfer-encoding") == fields.names_.end());
        }
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content}) {
        ruvia::http_response response;
        response.status(status);
        const auto head = ruvia::encode_http3_streaming_response_head(std::move(response), ruvia::http_known_method::head,
            ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
        RUVIA_CHECK((head.index() == 0));
        if ((head.index() == 0)) {
            RUVIA_CHECK(std::get<0>(head).commit_plan_.head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
        }
    }
    ruvia::http_response forbidden;
    forbidden.status(ruvia::http_status::no_content);
    RUVIA_CHECK((ruvia::encode_http3_streaming_response_head(std::move(forbidden), ruvia::http_known_method::get,
                     ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::present)
                     .index() != 0));
}

RUVIA_TEST(http3_response_head_encodes_status_and_fields_for_qpack_decode) {
    const std::array fields_value{ruvia::http3_field_section_field_view{"content-type", "text/plain"}};
    std::pmr::monotonic_buffer_resource resource;
    const auto result_value = ruvia::encode_http3_response_head(
        ruvia::http_status::ok, ruvia::http_known_method::get, fields_value, {}, &resource);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() != 0)) {
        return;
    }
    fields decoded;
    const auto count = ruvia::decode_http3_field_section(std::get<0>(result_value).headers_.field_section(), collect, &decoded);
    RUVIA_CHECK((count.index() == 0));
    RUVIA_CHECK_EQ(decoded.names_.size(), 2U);
    if (decoded.names_.size() == 2) {
        RUVIA_CHECK_EQ(decoded.names_[0], ":status");
        RUVIA_CHECK_EQ(decoded.values_[0], "200");
        RUVIA_CHECK_EQ(decoded.names_[1], "content-type");
        RUVIA_CHECK_EQ(decoded.values_[1], "text/plain");
    }
    RUVIA_CHECK_EQ(std::get<0>(result_value).headers_.decoded_field_section_size(), 96U);
    RUVIA_CHECK(std::get<0>(result_value).body_plan_.status_allows_body());
}

RUVIA_TEST(http3_response_writer_emits_canonical_rfc_static_references) {
    const std::array fields_value{ruvia::http3_field_section_field_view{"content-type", "text/custom"}};
    const auto result_value = ruvia::encode_http3_response_head(
        ruvia::http_status::continue_value, ruvia::http_known_method::get, fields_value);
    RUVIA_CHECK((result_value.index() == 0));
    if ((result_value.index() != 0)) {
        return;
    }

    // :status 100 is static index 63; content-type's static name reference is index 44.
    constexpr std::array<char, 18> canonical_wire{
        '\0', '\0', static_cast<char>(0xff), '\0', static_cast<char>(0x5f),
        static_cast<char>(0x1d), static_cast<char>(0x0b), 't', 'e', 'x', 't', '/', 'c', 'u', 's', 't', 'o', 'm'};
    RUVIA_CHECK_EQ(std::get<0>(result_value).headers_.field_section().size(), canonical_wire.size());
    RUVIA_CHECK(std::equal(std::get<0>(result_value).headers_.field_section().begin(), std::get<0>(result_value).headers_.field_section().end(),
        canonical_wire.begin(), canonical_wire.end()));
}

RUVIA_TEST(http3_interim_response_writer_normalizes_null_resource_and_retains_owned_results) {
    const std::array headers{ruvia::http_header_view{"Link", "</style.css>; rel=preload"}};
    const ruvia::http_interim_response_head response(ruvia::http_status::early_hints, headers);
    const auto static_null = ruvia::encode_http3_interim_response_head(response, {}, nullptr);
    RUVIA_CHECK((static_null.index() == 0));
    if ((static_null.index() == 0)) {
        fields decoded;
        const auto count = ruvia::decode_http3_field_section(std::get<0>(static_null).headers_.field_section(), collect, &decoded);
        RUVIA_CHECK((count.index() == 0));
        RUVIA_CHECK_EQ(decoded.names_.size(), 2U);
        if (decoded.names_.size() == 2) {
            RUVIA_CHECK_EQ(decoded.names_[0], ":status");
            RUVIA_CHECK_EQ(decoded.values_[0], "103");
            RUVIA_CHECK_EQ(decoded.names_[1], "link");
            RUVIA_CHECK_EQ(decoded.values_[1], "</style.css>; rel=preload");
        }
    }

    ruvia::http3_qpack_encoder encoder({.max_table_capacity_ = 0, .max_blocked_streams_ = 0});
    const auto dynamic_null = ruvia::encode_http3_interim_response_head(encoder, 0, response, {}, nullptr);
    RUVIA_CHECK((dynamic_null.index() == 0));
    if ((dynamic_null.index() == 0)) {
        fields decoded;
        const auto count = ruvia::decode_http3_field_section(std::get<0>(dynamic_null).headers_.field_section(), collect, &decoded);
        RUVIA_CHECK((count.index() == 0));
        RUVIA_CHECK_EQ(decoded.names_.size(), 2U);
        if (decoded.names_.size() == 2) {
            RUVIA_CHECK_EQ(decoded.names_[0], ":status");
            RUVIA_CHECK_EQ(decoded.values_[0], "103");
            RUVIA_CHECK_EQ(decoded.names_[1], "link");
            RUVIA_CHECK_EQ(decoded.values_[1], "</style.css>; rel=preload");
        }
    }

    counting_resource static_resource;
    {
        const auto first = ruvia::encode_http3_interim_response_head(response, {}, &static_resource);
        RUVIA_CHECK((first.index() == 0));
        if ((first.index() != 0)) {
            return;
        }
        RUVIA_CHECK(std::get<0>(first).headers_.resource() == &static_resource);
        const std::vector<char> retained(std::get<0>(first).headers_.field_section().begin(), std::get<0>(first).headers_.field_section().end());
        const auto second = ruvia::encode_http3_interim_response_head(response, {}, &static_resource);
        RUVIA_CHECK((second.index() == 0));
        RUVIA_CHECK(std::ranges::equal(std::get<0>(first).headers_.field_section(), retained));
        RUVIA_CHECK(static_resource.allocations_ > static_resource.deallocations_);
    }
    RUVIA_CHECK_EQ(static_resource.allocations_, static_resource.deallocations_);

    counting_resource dynamic_resource;
    {
        ruvia::http3_qpack_encoder resource_encoder({.max_table_capacity_ = 0, .max_blocked_streams_ = 0});
        const auto result_value = ruvia::encode_http3_interim_response_head(resource_encoder, 0, response, {}, &dynamic_resource);
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            RUVIA_CHECK(std::get<0>(result_value).headers_.resource() == &dynamic_resource);
            RUVIA_CHECK(dynamic_resource.allocations_ > dynamic_resource.deallocations_);
        }
    }
    RUVIA_CHECK_EQ(dynamic_resource.allocations_, dynamic_resource.deallocations_);

    const std::array invalid_headers{ruvia::http_header_view{"Connection", "close"}};
    const ruvia::http_interim_response_head invalid_response(ruvia::http_status::early_hints, invalid_headers);
    const auto invalid = ruvia::encode_http3_interim_response_head(invalid_response, {}, nullptr);
    RUVIA_CHECK((invalid.index() != 0));
    if ((invalid.index() != 0)) {
        RUVIA_CHECK(std::get<1>(invalid).kind_ == ruvia::http3_response_head_error::forbidden_field);
    }
}

RUVIA_TEST(http3_response_head_body_plan_suppresses_head_and_bodyless_statuses) {
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content,
             ruvia::http_status::not_modified}) {
        const auto result_value = ruvia::encode_http3_response_head(status, ruvia::http_known_method::head, {});
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(result_value).headers_.decoded_field_section_size(), 42U);
            RUVIA_CHECK(std::get<0>(result_value).body_plan_.body_suppressed());
        }
    }
    const auto no_content = ruvia::encode_http3_response_head(
        ruvia::http_status::no_content, ruvia::http_known_method::get, {});
    RUVIA_CHECK((no_content.index() == 0));
    if ((no_content.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(no_content).headers_.decoded_field_section_size(), 42U);
        RUVIA_CHECK(std::get<0>(no_content).body_plan_.body_suppressed());
    }
}

RUVIA_TEST(http3_response_head_decoded_size_counts_status_and_multiple_fields) {
    const auto status_only = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, {},
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 42, .max_fields_ = 1});
    RUVIA_CHECK((status_only.index() == 0));
    if ((status_only.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(status_only).headers_.decoded_field_section_size(), 42U);
    }
    const auto status_over_limit = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, {},
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 41, .max_fields_ = 1});
    RUVIA_CHECK((status_over_limit.index() != 0));
    if ((status_over_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(status_over_limit).field_section_error_ ==
                    ruvia::http3_field_section_error::field_list_too_large);
    }

    const std::array fields_value{ruvia::http3_field_section_field_view{"x", "y"},
        ruvia::http3_field_section_field_view{"long-name", "value"}};
    const auto exact = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, fields_value,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 122, .max_fields_ = 3});
    RUVIA_CHECK((exact.index() == 0));
    if ((exact.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(exact).headers_.decoded_field_section_size(), 122U);
    }
    const auto over_limit = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, fields_value,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 121, .max_fields_ = 3});
    RUVIA_CHECK((over_limit.index() != 0));
    if ((over_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(over_limit).field_section_error_ ==
                    ruvia::http3_field_section_error::field_list_too_large);
    }

    const std::string large_value(ruvia::max_http_header_bytes + 1, 'v');
    const std::array large_fields{ruvia::http3_field_section_field_view{"x", large_value}};
    const auto large_size = std::size_t{42 + 32 + 1} + large_value.size();
    const auto large = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, large_fields,
        {.max_encoded_bytes_ = large_size * 2, .max_decoded_bytes_ = large_size, .max_fields_ = 2});
    RUVIA_CHECK((large.index() == 0));
    if ((large.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(large).headers_.decoded_field_section_size(), large_size);
    }

    auto source_value = ruvia::encode_http3_response_head(
        ruvia::http_status::ok, ruvia::http_known_method::get, {});
    RUVIA_CHECK((source_value.index() == 0));
    if ((source_value.index() == 0)) {
        auto moved = std::move(std::get<0>(source_value));
        RUVIA_CHECK_EQ(moved.headers_.decoded_field_section_size(), 42U);
        RUVIA_CHECK_EQ(std::get<0>(source_value).headers_.decoded_field_section_size(), 0U);
        RUVIA_CHECK(std::get<0>(source_value).headers_.field_section().empty());
        auto destination = ruvia::encode_http3_response_head(
            ruvia::http_status::ok, ruvia::http_known_method::get, {});
        RUVIA_CHECK((destination.index() == 0));
        if ((destination.index() == 0)) {
            std::get<0>(destination) = std::move(moved);
            RUVIA_CHECK_EQ(std::get<0>(destination).headers_.decoded_field_section_size(), 42U);
            RUVIA_CHECK_EQ(moved.headers_.decoded_field_section_size(), 0U);
            RUVIA_CHECK(moved.headers_.field_section().empty());
        }
    }
}

RUVIA_TEST(http3_response_head_moves_and_extracts_owned_section_without_allocating) {
    counting_resource resource;
    counting_resource equivalent_resource(&resource);
    const std::array source_fields{
        ruvia::http3_field_section_field_view{"x-source", "ssssssssssssssssssssssss"},
        ruvia::http3_field_section_field_view{"content-length", "17"}};
    const std::array destination_fields{
        ruvia::http3_field_section_field_view{"x-destination", "d"},
        ruvia::http3_field_section_field_view{"content-length", "3"}};
    for (auto* destination_resource : {&resource, &equivalent_resource}) {
        {
            auto source_result = ruvia::encode_http3_response_head(
                ruvia::http_status::ok, ruvia::http_known_method::head, source_fields, {}, &resource);
            auto destination_result = ruvia::encode_http3_response_head(
                ruvia::http_status::ok, ruvia::http_known_method::get, destination_fields, {}, destination_resource);
            RUVIA_CHECK((source_result.index() == 0) && (destination_result.index() == 0));
            if ((source_result.index() != 0) || (destination_result.index() != 0)) {
                continue;
            }
            auto& source_value = std::get<0>(source_result);
            auto& destination = std::get<0>(destination_result);
            const auto decoded_size = source_value.headers_.decoded_field_section_size();
            const auto declared_length = source_value.declared_content_length_;
            RUVIA_CHECK_EQ(declared_length.value_or(0), 17U);
            RUVIA_CHECK_EQ(destination.declared_content_length_.value_or(0), 3U);
            RUVIA_CHECK(decoded_size > 0U);
            RUVIA_CHECK(source_value.body_plan_.body_suppressed());
            const auto* bytes_value = source_value.headers_.field_section().data();
            const auto* frame_value = source_value.headers_.frame().data();
            const auto allocations = resource.allocations_ + equivalent_resource.allocations_;
            auto moved = std::move(source_value);
            RUVIA_CHECK(moved.headers_.field_section().data() == bytes_value);
            RUVIA_CHECK_EQ(source_value.headers_.decoded_field_section_size(), 0U);
            RUVIA_CHECK(source_value.headers_.field_section().empty());
            destination = std::move(moved);
            RUVIA_CHECK(destination.headers_.field_section().data() == bytes_value);
            RUVIA_CHECK(destination.headers_.frame().data() == frame_value);
            RUVIA_CHECK(destination.headers_.resource() == destination_resource);
            RUVIA_CHECK_EQ(destination.headers_.decoded_field_section_size(), decoded_size);
            RUVIA_CHECK(destination.body_plan_.body_suppressed());
            RUVIA_CHECK(destination.declared_content_length_ == declared_length);
            RUVIA_CHECK_EQ(moved.headers_.decoded_field_section_size(), 0U);
            RUVIA_CHECK(moved.headers_.field_section().empty());
            auto frame = std::move(destination.headers_);
            RUVIA_CHECK(frame.field_section().data() == bytes_value);
            RUVIA_CHECK_EQ(frame.decoded_field_section_size(), decoded_size);
            RUVIA_CHECK_EQ(destination.headers_.decoded_field_section_size(), 0U);
            RUVIA_CHECK(destination.headers_.field_section().empty());
            RUVIA_CHECK(destination.body_plan_.body_suppressed());
            RUVIA_CHECK(destination.declared_content_length_ == declared_length);
            RUVIA_CHECK_EQ(resource.allocations_ + equivalent_resource.allocations_, allocations);
        }
        RUVIA_CHECK_EQ(resource.allocations_ + equivalent_resource.allocations_,
            resource.deallocations_ + equivalent_resource.deallocations_);
    }
}

RUVIA_TEST(http3_response_head_cross_resource_assignment_preserves_metadata_on_allocation_failure) {
    counting_resource source_resource;
    counting_resource destination_resource;
    {
        const std::string source_field_value(1024, 's');
        const std::array source_fields{
            ruvia::http3_field_section_field_view{"x-source", source_field_value},
            ruvia::http3_field_section_field_view{"content-length", "17"}};
        const std::array destination_fields{
            ruvia::http3_field_section_field_view{"x-destination", "d"},
            ruvia::http3_field_section_field_view{"content-length", "3"}};
        auto source_result = ruvia::encode_http3_response_head(
            ruvia::http_status::ok, ruvia::http_known_method::head, source_fields, {}, &source_resource);
        auto destination_result = ruvia::encode_http3_response_head(
            ruvia::http_status::ok, ruvia::http_known_method::get, destination_fields, {}, &destination_resource);
        RUVIA_CHECK((source_result.index() == 0) && (destination_result.index() == 0));
        if ((source_result.index() != 0) || (destination_result.index() != 0)) {
            return;
        }
        auto& source_value = std::get<0>(source_result);
        auto& destination = std::get<0>(destination_result);
        const auto source_frame = source_value.headers_.frame();
        const std::vector<char> source_snapshot(source_frame.begin(), source_frame.end());
        const auto destination_frame = destination.headers_.frame();
        const std::vector<char> destination_snapshot(destination_frame.begin(), destination_frame.end());
        const auto source_decoded = source_value.headers_.decoded_field_section_size();
        const auto destination_decoded = destination.headers_.decoded_field_section_size();
        const auto source_declared = source_value.declared_content_length_;
        const auto destination_declared = destination.declared_content_length_;
        RUVIA_CHECK_EQ(source_declared.value_or(0), 17U);
        RUVIA_CHECK_EQ(destination_declared.value_or(0), 3U);
        RUVIA_CHECK(source_value.body_plan_.body_suppressed());
        RUVIA_CHECK(!destination.body_plan_.body_suppressed());
        const auto* source_bytes = source_value.headers_.field_section().data();
        const auto* destination_bytes = destination.headers_.field_section().data();
        destination_resource.fail_allocations_ = true;
        bool threw = false;
        try {
            destination = std::move(source_value);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK(destination.headers_.field_section().data() == destination_bytes);
        RUVIA_CHECK(std::ranges::equal(destination.headers_.frame(), destination_snapshot));
        RUVIA_CHECK_EQ(destination.headers_.decoded_field_section_size(), destination_decoded);
        RUVIA_CHECK(!destination.body_plan_.body_suppressed());
        RUVIA_CHECK(destination.declared_content_length_ == destination_declared);
        RUVIA_CHECK(source_value.headers_.field_section().data() == source_bytes);
        RUVIA_CHECK(std::ranges::equal(source_value.headers_.frame(), source_snapshot));
        RUVIA_CHECK_EQ(source_value.headers_.decoded_field_section_size(), source_decoded);
        RUVIA_CHECK(source_value.body_plan_.body_suppressed());
        RUVIA_CHECK(source_value.declared_content_length_ == source_declared);

        destination_resource.fail_allocations_ = false;
        const auto allocations = destination_resource.allocations_;
        destination = std::move(source_value);
        RUVIA_CHECK_EQ(destination_resource.allocations_, allocations + 1);
        RUVIA_CHECK(destination.headers_.resource() == &destination_resource);
        RUVIA_CHECK(std::ranges::equal(destination.headers_.frame(), source_snapshot));
        RUVIA_CHECK_EQ(destination.headers_.decoded_field_section_size(), source_decoded);
        RUVIA_CHECK(destination.body_plan_.body_suppressed());
        RUVIA_CHECK(destination.declared_content_length_ == source_declared);
        RUVIA_CHECK_EQ(source_value.headers_.decoded_field_section_size(), 0U);
        RUVIA_CHECK(source_value.headers_.field_section().empty());
    }
    RUVIA_CHECK_EQ(source_resource.allocations_, source_resource.deallocations_);
    RUVIA_CHECK_EQ(destination_resource.allocations_, destination_resource.deallocations_);
}

RUVIA_TEST(http3_response_head_rejects_http1_fields_and_response_te) {
    for (const auto field : {ruvia::http3_field_section_field_view{"connection", "close"},
             ruvia::http3_field_section_field_view{"transfer-encoding", "chunked"},
             ruvia::http3_field_section_field_view{"te", "gzip"},
             ruvia::http3_field_section_field_view{"te", "trailers"}}) {
        const std::array fields_value{field};
        const auto result_value = ruvia::encode_http3_response_head(
            ruvia::http_status::ok, ruvia::http_known_method::get, fields_value);
        RUVIA_CHECK((result_value.index() != 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value).kind_ == ruvia::http3_response_head_error::forbidden_field);
        }
    }
}

RUVIA_TEST(http3_response_head_rejects_invalid_metadata_before_qpack_encoding) {
    const auto check = [&](std::span<const ruvia::http3_field_section_field_view> fields_value,
                           ruvia::http3_response_head_error expected) {
        for (const bool connection_owned : {false, true}) {
            ruvia::http3_qpack_encoder encoder({});
            const auto pending = encoder.pending_encoder_output();
            const std::vector<char> before(pending.begin(), pending.end());
            const auto result_value = connection_owned
                                          ? ruvia::encode_http3_response_head(encoder, 0,
                                                ruvia::http_status::ok, ruvia::http_known_method::get, fields_value)
                                          : ruvia::encode_http3_response_head(
                                                ruvia::http_status::ok, ruvia::http_known_method::get, fields_value);
            RUVIA_CHECK((result_value.index() != 0));
            if (result_value.index() != 0) {
                RUVIA_CHECK(std::get<1>(result_value).kind_ == expected);
            }
            RUVIA_CHECK(std::ranges::equal(encoder.pending_encoder_output(), before));
        }
    };
    for (const auto invalid : {ruvia::http3_field_section_field_view{"content-type", "text plain"},
             ruvia::http3_field_section_field_view{"content-encoding", "gzip;q=1"},
             ruvia::http3_field_section_field_view{"trailer", "Content-Length"},
             ruvia::http3_field_section_field_view{"host", "example.test"}}) {
        const std::array fields_value{ruvia::http3_field_section_field_view{"x-first", "new-dynamic-value"}, invalid};
        check(fields_value, invalid.name_ == "host" ? ruvia::http3_response_head_error::forbidden_field
                                                    : ruvia::http3_response_head_error::invalid_field);
    }
    const std::array duplicate_type{ruvia::http3_field_section_field_view{"content-type", "text/plain"},
        ruvia::http3_field_section_field_view{"content-type", "text/plain"}};
    check(duplicate_type, ruvia::http3_response_head_error::invalid_field);
}

RUVIA_TEST(http3_response_projection_validates_stable_metadata) {
    for (const auto invalid : {ruvia::http_header_view{"Content-Type", "text plain"},
             ruvia::http_header_view{"Content-Encoding", "gzip;q=1"},
             ruvia::http_header_view{"Trailer", "Content-Length"},
             ruvia::http_header_view{"Host", "example.test"}}) {
        for (const bool streaming : {false, true}) {
            for (const bool connection_owned : {false, true}) {
                ruvia::http3_qpack_encoder encoder({});
                const auto pending = encoder.pending_encoder_output();
                const std::vector<char> before(pending.begin(), pending.end());
                ruvia::http_response response;
                response.header_stable_view("X-First", "new-dynamic-value");
                response.header_stable_view(invalid.name(), invalid.value());
                const auto result_value = [&]() -> std::variant<ruvia::http3_response_head, ruvia::http3_response_head_failure> {
                    if (!streaming) {
                        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
                        return connection_owned ? ruvia::encode_http3_response_head(encoder, 0, response, plan)
                                                : ruvia::encode_http3_response_head(response, plan);
                    }
                    auto encoded = connection_owned
                                       ? ruvia::encode_http3_streaming_response_head(encoder, 0, std::move(response),
                                             ruvia::http_known_method::get, ruvia::http_response_stream_kind::generic,
                                             ruvia::http_response_trailer_intent::none)
                                       : ruvia::encode_http3_streaming_response_head(std::move(response),
                                             ruvia::http_known_method::get, ruvia::http_response_stream_kind::generic,
                                             ruvia::http_response_trailer_intent::none);
                    if (encoded.index() != 0) {
                        return std::get<1>(encoded);
                    }
                    return std::move(std::get<0>(encoded).head_);
                }();
                RUVIA_CHECK((result_value.index() != 0));
                if (result_value.index() != 0) {
                    RUVIA_CHECK(std::get<1>(result_value).kind_ ==
                                (invalid.name() == "Host" ? ruvia::http3_response_head_error::forbidden_field
                                                          : ruvia::http3_response_head_error::invalid_field));
                }
                RUVIA_CHECK(std::ranges::equal(encoder.pending_encoder_output(), before));
            }
        }
    }
}

RUVIA_TEST(http3_response_metadata_round_trips_through_message_decoder) {
    const std::array fields_value{ruvia::http3_field_section_field_view{"content-type", "application/json; charset=utf-8"},
        ruvia::http3_field_section_field_view{"content-encoding", "gzip, br"},
        ruvia::http3_field_section_field_view{"trailer", "etag, x-checksum"}};
    for (const bool connection_owned : {false, true}) {
        ruvia::http3_qpack_encoder encoder({});
        const auto encoded = connection_owned
                                 ? ruvia::encode_http3_response_head(encoder, 0, ruvia::http_status::ok,
                                       ruvia::http_known_method::get, fields_value)
                                 : ruvia::encode_http3_response_head(ruvia::http_status::ok,
                                       ruvia::http_known_method::get, fields_value);
        RUVIA_CHECK((encoded.index() == 0));
        if (encoded.index() != 0) {
            continue;
        }
        const auto section = std::get<0>(encoded).headers_.field_section();
        if (connection_owned) {
            ruvia::http3_qpack_decoder decoder({});
            RUVIA_CHECK((decoder.consume_encoder(encoder.pending_encoder_output()).index() == 0));
            const auto decoded = ruvia::decode_http3_message_head(decoder, 0, section,
                ruvia::http3_message_head_kind::response, std::pmr::get_default_resource());
            RUVIA_CHECK((decoded.index() == 0));
            if (decoded.index() == 0) {
                const auto* head = std::get_if<ruvia::http3_message_head>(&std::get<0>(decoded));
                RUVIA_CHECK(head != nullptr);
                if (head != nullptr) {
                    RUVIA_CHECK_EQ(head->status_, 200U);
                    RUVIA_CHECK_EQ(head->headers_.size(), fields_value.size());
                }
            }
        } else {
            const auto decoded = ruvia::decode_http3_message_head(section,
                ruvia::http3_message_head_kind::response);
            RUVIA_CHECK((decoded.index() == 0));
            if (decoded.index() == 0) {
                RUVIA_CHECK_EQ(std::get<0>(decoded).status_, 200U);
                RUVIA_CHECK_EQ(std::get<0>(decoded).headers_.size(), fields_value.size());
            }
        }
    }
}

RUVIA_TEST(http3_response_head_validates_content_length_and_status_rules) {
    const std::array valid_length{ruvia::http3_field_section_field_view{"content-length", "18446744073709551615"}};
    const auto valid = ruvia::encode_http3_response_head(
        ruvia::http_status::ok, ruvia::http_known_method::get, valid_length);
    RUVIA_CHECK((valid.index() == 0));

    const std::array duplicate_equal{
        ruvia::http3_field_section_field_view{"content-length", "00042"},
        ruvia::http3_field_section_field_view{"content-length", "42"}};
    RUVIA_CHECK((ruvia::encode_http3_response_head(
                     ruvia::http_status::ok, ruvia::http_known_method::get, duplicate_equal)
                     .index() == 0));

    for (const auto value : {"", "+1", "-1", " 1", "1 ", "1, 1", "18446744073709551616"}) {
        const std::array fields_value{ruvia::http3_field_section_field_view{"content-length", value}};
        const auto result_value = ruvia::encode_http3_response_head(
            ruvia::http_status::ok, ruvia::http_known_method::get, fields_value);
        RUVIA_CHECK((result_value.index() != 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value).kind_ == ruvia::http3_response_head_error::invalid_field);
        }
    }

    const std::array conflicting{
        ruvia::http3_field_section_field_view{"content-length", "42"},
        ruvia::http3_field_section_field_view{"content-length", "43"}};
    const auto mismatch = ruvia::encode_http3_response_head(
        ruvia::http_status::ok, ruvia::http_known_method::get, conflicting);
    RUVIA_CHECK((mismatch.index() != 0));
    if ((mismatch.index() != 0)) {
        RUVIA_CHECK(std::get<1>(mismatch).kind_ == ruvia::http3_response_head_error::invalid_field);
    }

    for (const auto status : {ruvia::http_status::continue_value, ruvia::http_status::no_content}) {
        const std::array fields_value{ruvia::http3_field_section_field_view{"content-length", "0"}};
        const auto result_value = ruvia::encode_http3_response_head(status, ruvia::http_known_method::get, fields_value);
        RUVIA_CHECK((result_value.index() != 0));
        if ((result_value.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result_value).kind_ == ruvia::http3_response_head_error::invalid_field);
        }
    }
    const std::array length_for304{ruvia::http3_field_section_field_view{"content-length", "12"}};
    RUVIA_CHECK((ruvia::encode_http3_response_head(
                     ruvia::http_status::not_modified, ruvia::http_known_method::get, length_for304)
                     .index() == 0));

    const auto switching = ruvia::encode_http3_response_head(
        ruvia::http_status::switching_protocols, ruvia::http_known_method::get, {});
    RUVIA_CHECK((switching.index() != 0));
    if ((switching.index() != 0)) {
        RUVIA_CHECK(std::get<1>(switching).kind_ == ruvia::http3_response_head_error::unsupported_status);
    }
}

RUVIA_TEST(http3_response_writer_projects_buffered_response_and_canonical_length) {
    ruvia::http_response response;
    response.body("payload");
    response.header("X-MiXeD", "ok");
    response.header("Set-Cookie", "a=1", {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "b=2", {.mode_ = ruvia::http_response_header_mode::append});
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto encoded = ruvia::encode_http3_response_head(response, plan);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() != 0)) {
        return;
    }
    fields decoded;
    const auto count = ruvia::decode_http3_field_section(std::get<0>(encoded).headers_.field_section(), collect, &decoded);
    RUVIA_CHECK((count.index() == 0));
    RUVIA_CHECK(std::find(decoded.names_.begin(), decoded.names_.end(), "x-mixed") != decoded.names_.end());
    RUVIA_CHECK(std::count(decoded.names_.begin(), decoded.names_.end(), "set-cookie") == 2);
    RUVIA_CHECK(std::find(decoded.values_.begin(), decoded.values_.end(), "7") != decoded.values_.end());
    RUVIA_CHECK(std::find(decoded.names_.begin(), decoded.names_.end(), "date") != decoded.names_.end());
    RUVIA_CHECK_EQ(std::get<0>(encoded).headers_.decoded_field_section_size(), 285U);

    response.body("change");
    RUVIA_CHECK((ruvia::encode_http3_response_head(response, plan).index() != 0));
}

RUVIA_TEST(http3_response_writer_preserves_one_application_date) {
    ruvia::http_response response;
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.body("ok");
    const auto plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto encoded = ruvia::encode_http3_response_head(response, plan);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() != 0)) {
        return;
    }
    fields decoded;
    RUVIA_CHECK(
        (ruvia::decode_http3_field_section(std::get<0>(encoded).headers_.field_section(), collect, &decoded).index() == 0));
    RUVIA_CHECK_EQ(std::count(decoded.names_.begin(), decoded.names_.end(), "date"), 1);
    const auto date = std::find(decoded.names_.begin(), decoded.names_.end(), "date");
    RUVIA_CHECK(date != decoded.names_.end());
    if (date != decoded.names_.end()) {
        const auto index = static_cast<std::size_t>(date - decoded.names_.begin());
        RUVIA_CHECK_EQ(decoded.values_[index], "Wed, 21 Oct 2015 07:28:00 GMT");
    }
}

RUVIA_TEST(http3_response_writer_reports_final_automatic_and_explicit_content_length_size) {
    ruvia::http_response automatic_response;
    automatic_response.body("payload");
    const auto automatic_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get,
        automatic_response);
    const auto automatic = ruvia::encode_http3_response_head(automatic_response, automatic_plan,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 154, .max_fields_ = 3});
    RUVIA_CHECK((automatic.index() == 0));
    if ((automatic.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(automatic).headers_.decoded_field_section_size(), 154U);
    }
    const auto automatic_over_limit = ruvia::encode_http3_response_head(automatic_response, automatic_plan,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 153, .max_fields_ = 3});
    RUVIA_CHECK((automatic_over_limit.index() != 0));
    if ((automatic_over_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(automatic_over_limit).field_section_error_ ==
                    ruvia::http3_field_section_error::field_list_too_large);
    }

    ruvia::http_response explicit_response;
    explicit_response.body("payload");
    explicit_response.header("Content-Length", "0007");
    const auto explicit_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get,
        explicit_response);
    const auto explicit_length = ruvia::encode_http3_response_head(explicit_response, explicit_plan,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 154, .max_fields_ = 3});
    RUVIA_CHECK((explicit_length.index() == 0));
    if ((explicit_length.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(explicit_length).headers_.decoded_field_section_size(), 154U);
        fields decoded;
        RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(explicit_length).headers_.field_section(), collect, &decoded).index() == 0));
        const auto length = std::find(decoded.names_.begin(), decoded.names_.end(), "content-length");
        RUVIA_CHECK(length != decoded.names_.end());
        if (length != decoded.names_.end()) {
            const auto index = static_cast<std::size_t>(length - decoded.names_.begin());
            RUVIA_CHECK_EQ(decoded.values_[index], "7");
        }
    }
}

RUVIA_TEST(http3_response_writer_head_and_status_content_length_projection) {
    ruvia::http_response head_response;
    head_response.file_body("unused.bin", 123, 0, 123, ruvia::http_response_file_identity::unchecked());
    const auto head_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, head_response);
    const auto head = ruvia::encode_http3_response_head(head_response, head_plan);
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() == 0)) {
        fields decoded;
        RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(head).headers_.field_section(), collect, &decoded).index() == 0));
        RUVIA_CHECK(std::find(decoded.values_.begin(), decoded.values_.end(), "123") != decoded.values_.end());
        RUVIA_CHECK_EQ(std::get<0>(head).headers_.decoded_field_section_size(), 156U);
        RUVIA_CHECK(std::get<0>(head).body_plan_.body_suppressed());
    }

    for (const auto status : {ruvia::http_status::no_content, ruvia::http_status::reset_content}) {
        ruvia::http_response response;
        response.status(status);
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        const auto result_value = ruvia::encode_http3_response_head(response, plan);
        RUVIA_CHECK((result_value.index() == 0));
        if ((result_value.index() == 0)) {
            fields decoded;
            RUVIA_CHECK((ruvia::decode_http3_field_section(std::get<0>(result_value).headers_.field_section(), collect, &decoded).index() == 0));
            const auto expected = status == ruvia::http_status::reset_content ? "0" : "";
            if (expected[0] != '\0') {
                RUVIA_CHECK(std::find(decoded.values_.begin(), decoded.values_.end(), expected) != decoded.values_.end());
            } else {
                RUVIA_CHECK(std::find(decoded.names_.begin(), decoded.names_.end(), "content-length") == decoded.names_.end());
            }
        }
    }
    ruvia::http_response not_modified;
    not_modified.status(ruvia::http_status::not_modified);
    not_modified.header("Content-Length", "12");
    const auto not_modified_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, not_modified);
    const auto not_modified_head = ruvia::encode_http3_response_head(not_modified, not_modified_plan);
    RUVIA_CHECK((not_modified_head.index() == 0));
}

RUVIA_TEST(http3_response_writer_accepts_compact_static_qpack_under_encoded_limit) {
    ruvia::http_response response;
    response.status(ruvia::http_status::not_modified);
    response.header("Content-Type", "application/json");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto encoded = ruvia::encode_http3_response_head(response, plan,
        {.max_encoded_bytes_ = 64, .max_decoded_bytes_ = 1024, .max_fields_ = 3});
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() == 0)) {
        RUVIA_CHECK(std::get<0>(encoded).headers_.field_section().size() <= 64);
    }
}

RUVIA_TEST(http3_response_writer_rejects_invalid_projected_headers_and_limits) {
    for (const auto& [name, value] : {std::pair{"Connection", "close"}}) {
        ruvia::http_response response;
        response.header(name, value);
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        RUVIA_CHECK((ruvia::encode_http3_response_head(response, plan).index() != 0));
    }
    const std::array injected{ruvia::http3_field_section_field_view{"x-test", "bad\r\ninjected: yes"}};
    RUVIA_CHECK((ruvia::encode_http3_response_head(
                     ruvia::http_status::ok, ruvia::http_known_method::get, injected)
                     .index() != 0));
    ruvia::http_response response;
    response.body("x");
    response.header("Content-Length", "2");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK((ruvia::encode_http3_response_head(response, plan).index() != 0));
    response.remove_header("Content-Length");
    const auto valid_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto limited = ruvia::encode_http3_response_head(response, valid_plan,
        {.max_encoded_bytes_ = 64, .max_decoded_bytes_ = 64, .max_fields_ = 1});
    RUVIA_CHECK((limited.index() != 0));
}

RUVIA_TEST(http3_response_trailers_encode_allowed_fields_in_order_and_lowercase) {
    const std::array fields_value{
        ruvia::http3_field_section_field_view{"ETag", "\"v1\""},
        ruvia::http3_field_section_field_view{"Accept-Ranges", "bytes"},
        ruvia::http3_field_section_field_view{"X-Custom", "one"},
        ruvia::http3_field_section_field_view{"x-custom", "two"}};
    const auto encoded = ruvia::encode_http3_response_trailers(fields_value);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() != 0)) {
        return;
    }
    fields decoded;
    const auto count = ruvia::decode_http3_field_section(std::get<0>(encoded).field_section(), collect, &decoded);
    RUVIA_CHECK((count.index() == 0));
    RUVIA_CHECK_EQ(decoded.names_.size(), fields_value.size());
    RUVIA_CHECK_EQ(std::get<0>(encoded).decoded_field_section_size(), 176U);
    if (decoded.names_.size() == fields_value.size()) {
        RUVIA_CHECK_EQ(decoded.names_[0], "etag");
        RUVIA_CHECK_EQ(decoded.values_[0], "\"v1\"");
        RUVIA_CHECK_EQ(decoded.names_[1], "accept-ranges");
        RUVIA_CHECK_EQ(decoded.names_[2], "x-custom");
        RUVIA_CHECK_EQ(decoded.values_[2], "one");
        RUVIA_CHECK_EQ(decoded.values_[3], "two");
    }
}

RUVIA_TEST(http3_response_trailers_reject_forbidden_and_malformed_fields) {
    for (const auto field : {ruvia::http3_field_section_field_view{"Content-Length", "1"},
             ruvia::http3_field_section_field_view{"Date", "today"},
             ruvia::http3_field_section_field_view{"Location", "/"},
             ruvia::http3_field_section_field_view{":status", "200"}}) {
        const std::array fields_value{field};
        RUVIA_CHECK((ruvia::encode_http3_response_trailers(fields_value).index() != 0));
    }
    const std::array crlf{ruvia::http3_field_section_field_view{"x-test", "bad\r\ninjected"}};
    const auto invalid = ruvia::encode_http3_response_trailers(crlf);
    RUVIA_CHECK((invalid.index() != 0));
    if ((invalid.index() != 0)) {
        RUVIA_CHECK(std::get<1>(invalid).kind_ == ruvia::http3_response_head_error::invalid_field);
    }
    const std::array content_length{ruvia::http3_field_section_field_view{"content-length", "1"}};
    const auto forbidden = ruvia::encode_http3_response_trailers(content_length);
    RUVIA_CHECK((forbidden.index() != 0));
    if ((forbidden.index() != 0)) {
        RUVIA_CHECK(std::get<1>(forbidden).kind_ == ruvia::http3_response_head_error::forbidden_field);
    }
}

RUVIA_TEST(http3_response_trailers_enforce_field_section_limits_and_empty_section) {
    const auto empty = ruvia::encode_http3_response_trailers({});
    RUVIA_CHECK((empty.index() == 0));
    if ((empty.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(empty).decoded_field_section_size(), 0U);
        fields decoded;
        const auto count = ruvia::decode_http3_field_section(std::get<0>(empty).field_section(), collect, &decoded);
        RUVIA_CHECK((count.index() == 0));
        if ((count.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(count), 0U);
        }
    }
    const std::array fields_value{ruvia::http3_field_section_field_view{"x", "y"}};
    const auto too_many = ruvia::encode_http3_response_trailers(fields_value,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 1024, .max_fields_ = 0});
    RUVIA_CHECK((too_many.index() != 0));
    if ((too_many.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_many).field_section_error_ == ruvia::http3_field_section_error::too_many_fields);
    }
    const auto exact_decoded = ruvia::encode_http3_response_trailers(fields_value,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 34, .max_fields_ = 1});
    RUVIA_CHECK((exact_decoded.index() == 0));
    if ((exact_decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(exact_decoded).decoded_field_section_size(), 34U);
    }
    const auto too_large_decoded = ruvia::encode_http3_response_trailers(fields_value,
        {.max_encoded_bytes_ = 1024, .max_decoded_bytes_ = 33, .max_fields_ = 1});
    RUVIA_CHECK((too_large_decoded.index() != 0));
    if ((too_large_decoded.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_large_decoded).field_section_error_ ==
                    ruvia::http3_field_section_error::field_list_too_large);
    }
    const auto too_large_encoded = ruvia::encode_http3_response_trailers(fields_value,
        {.max_encoded_bytes_ = 1, .max_decoded_bytes_ = 1024, .max_fields_ = 1});
    RUVIA_CHECK((too_large_encoded.index() != 0));
    if ((too_large_encoded.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_large_encoded).field_section_error_ ==
                    ruvia::http3_field_section_error::field_section_too_large);
    }
}

RUVIA_TEST(http3_response_trailers_payload_releases_pmr_allocations_on_destruction) {
    counting_resource resource;
    counting_resource destination_resource;
    {
        const std::array fields_value{ruvia::http3_field_section_field_view{"X-Test", "value"}};
        auto encoded = ruvia::encode_http3_response_trailers(fields_value, {}, &resource);
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() != 0)) {
            return;
        }
        RUVIA_CHECK_EQ(std::get<0>(encoded).decoded_field_section_size(), 43U);
        auto moved = std::move(std::get<0>(encoded));
        RUVIA_CHECK_EQ(moved.decoded_field_section_size(), 43U);
        RUVIA_CHECK_EQ(std::get<0>(encoded).decoded_field_section_size(), 0U);
        RUVIA_CHECK(std::get<0>(encoded).field_section().empty());
        auto destination = ruvia::encode_http3_response_trailers({}, {}, &destination_resource);
        RUVIA_CHECK((destination.index() == 0));
        if ((destination.index() == 0)) {
            std::get<0>(destination) = std::move(moved);
            RUVIA_CHECK_EQ(std::get<0>(destination).decoded_field_section_size(), 43U);
            RUVIA_CHECK_EQ(moved.decoded_field_section_size(), 0U);
            RUVIA_CHECK(moved.field_section().empty());
        }
        RUVIA_CHECK(resource.allocations_ > resource.deallocations_);
    }
    RUVIA_CHECK(resource.allocations_ > 0);
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
    RUVIA_CHECK(destination_resource.allocations_ > 0);
    RUVIA_CHECK_EQ(destination_resource.allocations_, destination_resource.deallocations_);
}

RUVIA_TEST(http3_response_head_enforces_section_limits_including_status) {
    const auto too_few_fields = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, {}, {.max_encoded_bytes_ = 64, .max_decoded_bytes_ = 1024, .max_fields_ = 0});
    RUVIA_CHECK((too_few_fields.index() != 0));
    if ((too_few_fields.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_few_fields).field_section_error_ ==
                    ruvia::http3_field_section_error::too_many_fields);
    }

    const auto status_only_limit = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, {}, {.max_encoded_bytes_ = 64, .max_decoded_bytes_ = 1024, .max_fields_ = 1});
    RUVIA_CHECK((status_only_limit.index() == 0));

    const std::array one_field{ruvia::http3_field_section_field_view{"x", "y"}};
    const auto one_over_limit = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, one_field, {.max_encoded_bytes_ = 64, .max_decoded_bytes_ = 1024, .max_fields_ = 1});
    RUVIA_CHECK((one_over_limit.index() != 0));
    if ((one_over_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(one_over_limit).field_section_error_ ==
                    ruvia::http3_field_section_error::too_many_fields);
    }

    const auto too_small = ruvia::encode_http3_response_head(ruvia::http_status::ok,
        ruvia::http_known_method::get, {}, {.max_encoded_bytes_ = 2, .max_decoded_bytes_ = 1024, .max_fields_ = 8});
    RUVIA_CHECK((too_small.index() != 0));
    if ((too_small.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_small).field_section_error_ ==
                    ruvia::http3_field_section_error::field_section_too_large);
    }
}
