#include <array>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_message_head.h"

#include "test_harness.h"

namespace {

std::variant<ruvia::http3_message_head, ruvia::http3_message_head_error> decode(
    std::initializer_list<ruvia::http3_field_section_field_view> fields_value,
    ruvia::http3_message_head_kind kind, std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    ruvia::http3_message_head_limits limits = {}) {
    const std::vector<ruvia::http3_field_section_field_view> field_vector(fields_value);
    const auto wire = ruvia::encode_http3_field_section(field_vector, std::pmr::get_default_resource());
    if ((wire.index() != 0)) {
        return ruvia::http3_message_head_error::qpack_decompression_failed;
    }
    return ruvia::decode_http3_message_head(std::get<0>(wire), kind, resource, limits);
}

class failing_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value >= 64) {
            throw std::bad_alloc();
        }
        auto* value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        --live_;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http3_message_head_decodes_get_post_and_response) {
    const auto get = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/a?q=1"},
                                {":authority", "example.test"}, {"accept", "text/plain"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((get.index() == 0));
    if ((get.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(get).method_, "GET");
        RUVIA_CHECK_EQ(std::get<0>(get).path_, "/a?q=1");
        RUVIA_CHECK_EQ(std::get<0>(get).headers_.size(), 1U);
        RUVIA_CHECK_EQ(std::get<0>(get).headers_[0].name_, "accept");
    }

    const auto post = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                 {":authority", "example.test"}, {"content-length", "3"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((post.index() == 0));
    if ((post.index() == 0)) {
        RUVIA_CHECK(std::get<0>(post).content_length_ == std::uint64_t{3});
    }

    const auto response = decode({{":status", "204"}, {"content-length", "0"}},
        ruvia::http3_message_head_kind::response);
    RUVIA_CHECK((response.index() == 0));
    if ((response.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(response).status_, 204);
    }

    const auto wide = decode({{":status", "200"},
                                 {"content-length", " \t18446744073709551615, 018446744073709551615\t "},
                                 {"content-length", "18446744073709551615"}, {"x-ows", " value\t"}},
        ruvia::http3_message_head_kind::response);
    RUVIA_CHECK((wide.index() == 0));
    if ((wide.index() == 0)) {
        RUVIA_CHECK(std::get<0>(wide).content_length_ == UINT64_MAX);
        RUVIA_CHECK_EQ(std::get<0>(wide).headers_.back().value_, " value\t");
    }
}

RUVIA_TEST(http3_message_head_rejects_pseudo_header_order_duplicates_and_unknown_fields) {
    const auto late_pseudo = decode({{":method", "GET"}, {":scheme", "https"}, {"x", "y"},
                                        {":path", "/"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((late_pseudo.index() != 0) && std::get<1>(late_pseudo) == ruvia::http3_message_head_error::message_error);
    const auto duplicate = decode({{":method", "GET"}, {":method", "POST"}, {":scheme", "https"},
                                      {":path", "/"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((duplicate.index() != 0) && std::get<1>(duplicate) == ruvia::http3_message_head_error::message_error);
    const auto unknown = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                    {":unknown", "x"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((unknown.index() != 0) && std::get<1>(unknown) == ruvia::http3_message_head_error::message_error);
    const auto uppercase = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                      {"X-Test", "value"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((uppercase.index() != 0) && std::get<1>(uppercase) == ruvia::http3_message_head_error::message_error);
    const auto invalid_path = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "relative"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((invalid_path.index() != 0) && std::get<1>(invalid_path) == ruvia::http3_message_head_error::message_error);
}

RUVIA_TEST(http3_message_head_field_names_accept_only_lowercase_token_bytes) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const std::size_t position : {0U, 4U, 31U, 63U}) {
        std::string name(64, 'x');
        for (unsigned byte = 0; byte < 256; ++byte) {
            name[position] = static_cast<char>(byte);
            const bool allowed = (byte >= 'a' && byte <= 'z') ||
                                 (byte >= '0' && byte <= '9') ||
                                 punctuation.find(static_cast<char>(byte)) != std::string_view::npos;
            const auto response = decode({{":status", "200"}, {name, "value"}},
                ruvia::http3_message_head_kind::response);
            RUVIA_CHECK_EQ((response.index() == 0), allowed);
            if (response.index() == 0) {
                RUVIA_CHECK_EQ(std::get<0>(response).headers_.front().name_, std::string_view(name));
            } else {
                RUVIA_CHECK(std::get<1>(response) == ruvia::http3_message_head_error::message_error);
            }
            const auto request = decode({{":method", "GET"}, {":scheme", "https"},
                                            {":authority", "example.test"}, {":path", "/"}, {name, "value"}},
                ruvia::http3_message_head_kind::request);
            RUVIA_CHECK_EQ((request.index() == 0), allowed);
            if ((request.index() != 0)) {
                RUVIA_CHECK(std::get<1>(request) == ruvia::http3_message_head_error::message_error);
            }
        }
    }
}

RUVIA_TEST(http3_message_head_pseudo_names_are_case_sensitive) {
    for (const auto name : {":Method", ":Protocol", ":Scheme", ":Authority", ":Path"}) {
        const auto request = decode({{name, "GET"}, {":scheme", "https"}, {":path", "/"}},
            ruvia::http3_message_head_kind::request);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    }
    const auto response = decode({{":Status", "200"}}, ruvia::http3_message_head_kind::response);
    RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::http3_message_head_error::message_error);
}

RUVIA_TEST(http3_message_head_rejects_host_field_content_length_and_status_errors) {
    const auto host_mismatch = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                          {":authority", "one.test"}, {"host", "two.test"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((host_mismatch.index() != 0));
    const auto connection_field = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                                             {"connection", "close"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((connection_field.index() != 0));
    const auto bad_te = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"te", "gzip"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((bad_te.index() != 0));
    const auto conflicting_length = decode({{":method", "POST"}, {":scheme", "https"}, {":path", "/"},
                                               {"content-length", "2"}, {"content-length", "3"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((conflicting_length.index() != 0));
    for (const std::string_view value : {"", "1,", ",1", "1,,1", "1, 2", "+1", "-1", "18446744073709551616"}) {
        const auto invalid = decode({{":status", "200"}, {"content-length", value}},
            ruvia::http3_message_head_kind::response);
        RUVIA_CHECK((invalid.index() != 0) && std::get<1>(invalid) == ruvia::http3_message_head_error::message_error);
    }
    const auto bad_status = decode({{":status", "099"}}, ruvia::http3_message_head_kind::response);
    RUVIA_CHECK((bad_status.index() != 0));
}

RUVIA_TEST(http3_message_head_checks_origin_authority_connect_and_te_direction) {
    using ruvia::http3_message_head_kind;
    const auto options = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "*"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((options.index() == 0));
    const auto custom_scheme = decode({{":method", "GET"}, {":scheme", "custom"},
                                          {":authority", "example.test"}, {":path", "/"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((custom_scheme.index() == 0));
    const auto bad_path = decode({{":method", "GET"}, {":scheme", "https"},
                                     {":authority", "example.test"}, {":path", "/bad\\path"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((bad_path.index() != 0) && std::get<1>(bad_path) == ruvia::http3_message_head_error::message_error);
    const auto bad_authority = decode({{":method", "GET"}, {":scheme", "https"},
                                          {":authority", "example.test:not-a-port"}, {":path", "/"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((bad_authority.index() != 0) && std::get<1>(bad_authority) == ruvia::http3_message_head_error::message_error);
    const auto tunnel = decode({{":method", "CONNECT"}, {":authority", "example.test:443"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((tunnel.index() == 0));
    if ((tunnel.index() == 0)) {
        RUVIA_CHECK(std::get<0>(tunnel).protocol_.empty());
    }
    const auto missing_port = decode({{":method", "CONNECT"}, {":authority", "example.test"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((missing_port.index() != 0) && std::get<1>(missing_port) == ruvia::http3_message_head_error::message_error);
    const auto response_te = decode({{":status", "200"}, {"te", "trailers"}},
        http3_message_head_kind::response);
    RUVIA_CHECK((response_te.index() != 0) && std::get<1>(response_te) == ruvia::http3_message_head_error::message_error);
}

RUVIA_TEST(http3_message_head_validates_extended_connect_pseudo_fields) {
    using ruvia::http3_message_head_kind;
    const auto websocket_value = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                            {":scheme", "https"}, {":authority", "example.test"},
                                            {":path", "/socket?room=one"}, {"x-end-to-end", "retained"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((websocket_value.index() == 0));
    if ((websocket_value.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(websocket_value).method_, "CONNECT");
        RUVIA_CHECK_EQ(std::get<0>(websocket_value).protocol_, "websocket");
        RUVIA_CHECK_EQ(std::get<0>(websocket_value).path_, "/socket?room=one");
        RUVIA_CHECK_EQ(std::get<0>(websocket_value).headers_.size(), 1U);
        RUVIA_CHECK_EQ(std::get<0>(websocket_value).headers_[0].name_, "x-end-to-end");
    }

    const auto extended_host_mismatch = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                                   {":scheme", "https"}, {":authority", "one.test"},
                                                   {":path", "/socket"}, {"host", "two.test"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((extended_host_mismatch.index() != 0));
    if ((extended_host_mismatch.index() != 0)) {
        RUVIA_CHECK(std::get<1>(extended_host_mismatch) == ruvia::http3_message_head_error::message_error);
    }

    const auto missing_protocol = decode({{":method", "CONNECT"}, {":scheme", "https"},
                                             {":authority", "example.test"}, {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((missing_protocol.index() != 0));
    const auto missing_scheme = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                           {":authority", "example.test"}, {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((missing_scheme.index() != 0));
    const auto missing_authority = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                              {":scheme", "https"}, {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((missing_authority.index() != 0));
    const auto missing_path = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                         {":scheme", "https"}, {":authority", "example.test"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((missing_path.index() != 0));

    const auto protocol_before_method = decode({{":protocol", "websocket"}, {":method", "CONNECT"},
                                                   {":scheme", "https"}, {":authority", "example.test"},
                                                   {":path", "/socket"}},
        http3_message_head_kind::request);
    // RFC 9114 requires pseudo-fields to precede ordinary fields, but does not
    // impose an order among the pseudo-fields themselves.
    RUVIA_CHECK((protocol_before_method.index() == 0));
    const auto protocol_after_regular = decode({{":method", "CONNECT"}, {"x", "y"},
                                                   {":protocol", "websocket"}, {":scheme", "https"},
                                                   {":authority", "example.test"}, {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((protocol_after_regular.index() != 0));
    const auto duplicate_protocol = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                               {":protocol", "websocket"}, {":scheme", "https"},
                                               {":authority", "example.test"}, {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((duplicate_protocol.index() != 0));
    const auto invalid_protocol = decode({{":method", "CONNECT"}, {":protocol", "web socket"},
                                             {":scheme", "https"}, {":authority", "example.test"},
                                             {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((invalid_protocol.index() != 0));
    const auto protocol_on_get = decode({{":method", "GET"}, {":protocol", "websocket"},
                                            {":scheme", "https"}, {":authority", "example.test"},
                                            {":path", "/socket"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((protocol_on_get.index() != 0));
    if ((protocol_on_get.index() != 0)) {
        RUVIA_CHECK(std::get<1>(protocol_on_get) == ruvia::http3_message_head_error::message_error);
    }
}

RUVIA_TEST(http3_message_head_validates_origin_and_rejects_every_duplicate_host) {
    using ruvia::http3_message_head_kind;
    const auto valid = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                  {":scheme", "https"}, {":authority", "example.test"},
                                  {":path", "/socket"}, {"origin", "https://example.test"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((valid.index() == 0));

    const auto opaque = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                   {":scheme", "https"}, {":authority", "example.test"},
                                   {":path", "/socket"}, {"origin", "null"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((opaque.index() == 0));

    const auto invalid_origin = decode({{":method", "CONNECT"}, {":protocol", "websocket"},
                                           {":scheme", "https"}, {":authority", "example.test"},
                                           {":path", "/socket"}, {"origin", "https://example.test/path"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((invalid_origin.index() != 0));

    const auto duplicate_host = decode({{":method", "GET"}, {":scheme", "https"},
                                           {":authority", "example.test"}, {":path", "/"},
                                           {"host", "example.test"}, {"host", "example.test"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((duplicate_host.index() != 0));
}

RUVIA_TEST(http3_message_head_rejects_invalid_cors_preflight_fields) {
    using ruvia::http3_message_head_kind;
    const auto valid = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"origin", "https://app.example"},
                                  {"access-control-request-method", "POST"},
                                  {"access-control-request-headers", "x-trace, content-type"}},
        http3_message_head_kind::request);
    RUVIA_CHECK((valid.index() == 0));

    for (const std::string_view invalid : {"x bad", "", "x-trace, bad name"}) {
        const auto request = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"},
                                        {"origin", "https://app.example"},
                                        {"access-control-request-method", "POST"},
                                        {"access-control-request-headers", invalid}},
            http3_message_head_kind::request);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    }
    for (const std::string_view invalid : {"POST GET", ""}) {
        const auto request = decode({{":method", "OPTIONS"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"},
                                        {"origin", "https://app.example"},
                                        {"access-control-request-method", invalid}},
            http3_message_head_kind::request);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    }
}

RUVIA_TEST(http3_message_head_rejects_malformed_representation_fields) {
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"content-type", "application/json; charset=utf-8"},
                                  {"content-encoding", "gzip, br"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((valid.index() == 0));

    for (const auto field : {ruvia::http3_field_section_field_view{"content-type", "text plain"},
             ruvia::http3_field_section_field_view{"content-encoding", "gzip;q=1"}}) {
        const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                        {":authority", "example.test"}, {":path", "/"}, field},
            ruvia::http3_message_head_kind::request);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    }
    const auto repeated = decode({{":method", "POST"}, {":scheme", "https"},
                                     {":authority", "example.test"}, {":path", "/"},
                                     {"content-type", "text/plain"},
                                     {"content-type", "application/json"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((repeated.index() != 0) && std::get<1>(repeated) == ruvia::http3_message_head_error::message_error);

    for (const auto field : {ruvia::http3_field_section_field_view{"content-type", "text plain"},
             ruvia::http3_field_section_field_view{"content-encoding", "gzip;q=1"}}) {
        const auto response = decode({{":status", "200"}, field},
            ruvia::http3_message_head_kind::response);
        RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::http3_message_head_error::message_error);
    }
}

RUVIA_TEST(http3_message_head_rejects_forbidden_declared_trailer_names) {
    const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/"},
                                    {"trailer", "Content-Length"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    const auto response = decode({{":status", "200"}, {"trailer", "Content-Length"}},
        ruvia::http3_message_head_kind::response);
    RUVIA_CHECK((response.index() != 0) && std::get<1>(response) == ruvia::http3_message_head_error::message_error);
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"trailer", "x-checksum"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((valid.index() == 0));
}

RUVIA_TEST(http3_message_head_rejects_malformed_expectation) {
    const auto request = decode({{":method", "POST"}, {":scheme", "https"},
                                    {":authority", "example.test"}, {":path", "/"},
                                    {"expect", "foo?bar"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request) == ruvia::http3_message_head_error::message_error);
    const auto valid = decode({{":method", "POST"}, {":scheme", "https"},
                                  {":authority", "example.test"}, {":path", "/"},
                                  {"expect", "100-continue"}},
        ruvia::http3_message_head_kind::request);
    RUVIA_CHECK((valid.index() == 0));
}

RUVIA_TEST(http3_message_head_applies_rfc_field_section_size_and_propagates_resource_exceptions) {
    const auto oversized = decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}},
        ruvia::http3_message_head_kind::request, std::pmr::get_default_resource(), {40, 16});
    RUVIA_CHECK((oversized.index() != 0) && std::get<1>(oversized) == ruvia::http3_message_head_error::field_section_too_large);
    const auto exact = decode({{":status", "200"}}, ruvia::http3_message_head_kind::response,
        std::pmr::get_default_resource(), {.max_field_section_size_ = 42});
    RUVIA_CHECK((exact.index() == 0));
    for (const std::size_t limit : {std::size_t{0}, std::size_t{31}, std::size_t{41}}) {
        const auto rejected = decode({{":status", "200"}}, ruvia::http3_message_head_kind::response,
            std::pmr::get_default_resource(), {.max_field_section_size_ = limit});
        RUVIA_CHECK((rejected.index() != 0) && std::get<1>(rejected) == ruvia::http3_message_head_error::field_section_too_large);
    }
    const auto oversized_wire = decode({{":method", "GET"}, {":scheme", "https"},
                                           {":authority", "example.test"}, {":path", "/"}},
        ruvia::http3_message_head_kind::request, std::pmr::get_default_resource(),
        {.max_encoded_bytes_ = 2});
    RUVIA_CHECK((oversized_wire.index() != 0) && std::get<1>(oversized_wire) ==
                                                     ruvia::http3_message_head_error::field_section_too_large);

    failing_resource resource;
    const std::string long_name(256, 'n');
    const std::string long_value(256, 'v');
    bool threw = false;
    try {
        (void)decode({{":method", "GET"}, {":scheme", "https"}, {":path", "/"},
                         {long_name, long_value}},
            ruvia::http3_message_head_kind::request, &resource);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(resource.live_, std::size_t{0});
}
