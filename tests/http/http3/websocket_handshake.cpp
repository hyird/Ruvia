#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_server_request.h"
#include "ruvia/http/http3_websocket_handshake.h"
#include "ruvia/http/http_response.h"

#include "test_harness.h"

namespace {

struct captured_fields final {
    std::vector<std::string> names_;
    std::vector<std::string> values_;
};

bool collect_field(void* opaque, ruvia::http3_field_section_field_view field) {
    auto& captured_value = *static_cast<captured_fields*>(opaque);
    captured_value.names_.emplace_back(field.name_);
    captured_value.values_.emplace_back(field.value_);
    return true;
}

bool make_request(std::span<const ruvia::http3_field_section_field_view> fields_value,
    std::pmr::memory_resource* resource,
    std::optional<ruvia::http3_server_request>& request) {
    auto section = ruvia::encode_http3_field_section(fields_value, resource);
    if ((section.index() != 0)) {
        return false;
    }
    auto head = ruvia::decode_http3_message_head(
        std::get<0>(section), ruvia::http3_message_head_kind::request, resource);
    if ((head.index() != 0)) {
        return false;
    }
    request.emplace(std::get<0>(head), resource, resource);
    return true;
}

std::size_t count_header(const captured_fields& fields_value, std::string_view name) {
    std::size_t count = 0;
    for (const auto& candidate : fields_value.names_) {
        count += candidate == name;
    }
    return count;
}

std::string_view find_header(const captured_fields& fields_value, std::string_view name) {
    for (std::size_t i = 0; i < fields_value.names_.size(); ++i) {
        if (fields_value.names_[i] == name) {
            return fields_value.values_[i];
        }
    }
    return {};
}

}  // namespace

RUVIA_TEST(http3_websocket_handshake_builds_canonical_extended_connect_response) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array request_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"origin", "https://example.test"},
        ruvia::http3_field_section_field_view{"cookie", "session=abc"},
        ruvia::http3_field_section_field_view{"authorization", "Bearer token"},
        ruvia::http3_field_section_field_view{"x-client", "preserved"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
        ruvia::http3_field_section_field_view{"sec-websocket-protocol", "chat, superchat"},
        ruvia::http3_field_section_field_view{"sec-websocket-extensions", "permessage-deflate"},
    };
    std::optional<ruvia::http3_server_request> request;
    RUVIA_CHECK(make_request(request_fields, &resource, request));
    if (!request) {
        return;
    }

    constexpr std::array<std::string_view, 1> supported{"superchat"};
    const std::array response_headers_value{
        ruvia::http_header_view{"content-type", "application/websocket"},
        ruvia::http_header_view{"set-cookie", "first=1; Path=/"},
        ruvia::http_header_view{"set-cookie", "second=2; Path=/"},
    };
    auto handshake = ruvia::make_http3_websocket_handshake(request->request(),
        request->extended_connect_protocol(), true, {
                                                        .supported_subprotocols_ = supported,
                                                        .response_headers_ = response_headers_value,
                                                        .resource_ = &resource,
                                                        .deflate_ = {.enabled_ = true},
                                                        .date_ = "Tue, 15 Nov 1994 08:12:31 GMT",
                                                    });
    RUVIA_CHECK((handshake.index() == 0));
    if ((handshake.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(handshake).subprotocol(), "superchat");
    RUVIA_CHECK(std::get<0>(handshake).compression() != (ruvia::websocket_compression{}));

    const auto frame = ruvia::decode_http3_frame(std::get<0>(handshake).headers_frame());
    RUVIA_CHECK((frame.index() == 0));
    if ((frame.index() != 0)) {
        return;
    }
    RUVIA_CHECK(std::get<0>(frame).type_ == static_cast<std::uint64_t>(ruvia::http3_frame_type::headers));
    RUVIA_CHECK_EQ(std::get<0>(frame).encoded_bytes_, std::get<0>(handshake).headers_frame().size());
    captured_fields captured;
    const auto decoded = ruvia::decode_http3_field_section(std::get<0>(frame).payload_, collect_field, &captured);
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK_EQ(find_header(captured, ":status"), "200");
    RUVIA_CHECK_EQ(find_header(captured, "date"), "Tue, 15 Nov 1994 08:12:31 GMT");
    RUVIA_CHECK_EQ(find_header(captured, "sec-websocket-protocol"), "superchat");
    RUVIA_CHECK(!find_header(captured, "sec-websocket-extensions").empty());
    RUVIA_CHECK_EQ(count_header(captured, "set-cookie"), 2U);
    RUVIA_CHECK_EQ(find_header(captured, "content-type"), "application/websocket");
    for (const auto prohibited : {"content-length", "connection", "upgrade",
             "sec-websocket-accept", "sec-websocket-key"}) {
        RUVIA_CHECK_EQ(count_header(captured, prohibited), 0U);
    }
}

RUVIA_TEST(http3_websocket_handshake_rejects_invalid_version_framing_and_half_closed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array valid_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
    };
    std::optional<ruvia::http3_server_request> valid;
    RUVIA_CHECK(make_request(valid_fields, &resource, valid));
    if (!valid) {
        return;
    }
    RUVIA_CHECK((ruvia::validate_http3_websocket_handshake(
                     valid->request(), valid->extended_connect_protocol(), true)
                     .index() == 0));
    const auto half_closed = ruvia::validate_http3_websocket_handshake(
        valid->request(), valid->extended_connect_protocol(), false);
    RUVIA_CHECK((half_closed.index() != 0));
    RUVIA_CHECK(std::get<1>(half_closed).kind() ==
                ruvia::http3_websocket_handshake_failure::kind_type::invalid_request);
    const auto invalid_protocol_error = std::get<1>(half_closed).protocol_error();
    RUVIA_CHECK_EQ(invalid_protocol_error.status(), ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(std::string_view(invalid_protocol_error.what()), "invalid WebSocket handshake");
    ruvia::http_response invalid_response;
    std::get<1>(half_closed).apply_required_response_headers(invalid_response);
    RUVIA_CHECK(!invalid_response.header("Sec-WebSocket-Version").has_value());
    const auto wrong_protocol = ruvia::validate_http3_websocket_handshake(
        valid->request(), "not-websocket", true);
    RUVIA_CHECK((wrong_protocol.index() != 0));
    if ((wrong_protocol.index() != 0)) {
        RUVIA_CHECK(std::get<1>(wrong_protocol).kind() ==
                    ruvia::http3_websocket_handshake_failure::kind_type::invalid_request);
    }

    const std::array wrong_version_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "12"},
    };
    std::optional<ruvia::http3_server_request> wrong_version;
    RUVIA_CHECK(make_request(wrong_version_fields, &resource, wrong_version));
    if (wrong_version) {
        const auto result_value = ruvia::validate_http3_websocket_handshake(
            wrong_version->request(), wrong_version->extended_connect_protocol(), true);
        RUVIA_CHECK((result_value.index() != 0));
        RUVIA_CHECK(std::get<1>(result_value).kind() ==
                    ruvia::http3_websocket_handshake_failure::kind_type::unsupported_version);
        const auto protocol_error = std::get<1>(result_value).protocol_error();
        RUVIA_CHECK_EQ(protocol_error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(std::string_view(protocol_error.what()), "unsupported WebSocket version");

        const auto handshake = ruvia::make_http3_websocket_handshake(
            wrong_version->request(), wrong_version->extended_connect_protocol(), true);
        RUVIA_CHECK((handshake.index() != 0));
        if ((handshake.index() != 0)) {
            RUVIA_CHECK(std::get<1>(handshake).kind() ==
                        ruvia::http3_websocket_handshake_failure::kind_type::unsupported_version);
        }

        ruvia::http_response error_response;
        error_response.status(protocol_error.status());
        error_response.header("Sec-WebSocket-Version", "8");
        error_response.header("X-Application-Error", "retained");
        std::get<1>(result_value).apply_required_response_headers(error_response);
        RUVIA_CHECK_EQ(error_response.header("Sec-WebSocket-Version").value_or(""), "13");
        RUVIA_CHECK_EQ(error_response.header("X-Application-Error").value_or(""), "retained");
    }

    const std::array framed_fields{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
        ruvia::http3_field_section_field_view{"content-length", "0"},
    };
    std::optional<ruvia::http3_server_request> framed;
    RUVIA_CHECK(make_request(framed_fields, &resource, framed));
    if (framed) {
        const auto result_value = ruvia::validate_http3_websocket_handshake(
            framed->request(), framed->extended_connect_protocol(), true);
        RUVIA_CHECK((result_value.index() != 0));
    }
}

RUVIA_TEST(http3_websocket_handshake_rejects_duplicate_version_and_h1_handshake_fields) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array duplicate_version{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
    };
    std::optional<ruvia::http3_server_request> duplicate;
    RUVIA_CHECK(make_request(duplicate_version, &resource, duplicate));
    if (duplicate) {
        RUVIA_CHECK((ruvia::validate_http3_websocket_handshake(
                         duplicate->request(), duplicate->extended_connect_protocol(), true)
                         .index() != 0));
    }

    for (const auto forbidden : {"sec-websocket-key", "sec-websocket-accept", "content-length"}) {
        const std::array base{
            ruvia::http3_field_section_field_view{":method", "CONNECT"},
            ruvia::http3_field_section_field_view{":protocol", "websocket"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/socket"},
            ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
            ruvia::http3_field_section_field_view{
                forbidden, forbidden == std::string_view("content-length") ? "0" : "x"},
        };
        std::optional<ruvia::http3_server_request> request;
        RUVIA_CHECK(make_request(base, &resource, request));
        if (request) {
            RUVIA_CHECK((ruvia::validate_http3_websocket_handshake(
                             request->request(), request->extended_connect_protocol(), true)
                             .index() != 0));
        }
    }

    for (const auto forbidden : {"connection", "upgrade"}) {
        const std::array fields_value{
            ruvia::http3_field_section_field_view{":method", "CONNECT"},
            ruvia::http3_field_section_field_view{":protocol", "websocket"},
            ruvia::http3_field_section_field_view{":scheme", "https"},
            ruvia::http3_field_section_field_view{":authority", "example.test"},
            ruvia::http3_field_section_field_view{":path", "/socket"},
            ruvia::http3_field_section_field_view{"sec-websocket-version", "13"},
            ruvia::http3_field_section_field_view{forbidden, "websocket"},
        };
        auto section = ruvia::encode_http3_field_section(fields_value, &resource);
        RUVIA_CHECK((section.index() == 0));
        if ((section.index() == 0)) {
            const auto head = ruvia::decode_http3_message_head(
                std::get<0>(section), ruvia::http3_message_head_kind::request, &resource);
            RUVIA_CHECK((head.index() != 0));
        }
    }
}
