#include <array>
#include <string>
#include <variant>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_response_server.h"

#include "test_harness.h"

RUVIA_TEST(http_connect_udp_default_template_roundtrip_and_target_rejection) {
    for (const auto host : {"example.test", "192.0.2.6", "2001:db8::42"}) {
        const auto path = ruvia::encode_http_connect_udp_path({.host_ = host, .port_ = 443});
        RUVIA_CHECK((path.index() == 0));
        const auto target = ruvia::parse_http_connect_udp_path(std::get<0>(path));
        RUVIA_CHECK((target.index() == 0) && std::get<0>(target).host_ == host && std::get<0>(target).port_ == 443);
    }
    RUVIA_CHECK((ruvia::encode_http_connect_udp_path({.host_ = "fe80::1%eth0", .port_ = 80}).index() != 0));
    RUVIA_CHECK((ruvia::encode_http_connect_udp_path({.host_ = "[::1]", .port_ = 80}).index() != 0));
    RUVIA_CHECK((ruvia::encode_http_connect_udp_path({.host_ = "example.test", .port_ = 0}).index() != 0));
    RUVIA_CHECK((ruvia::parse_http_connect_udp_path("/.well-known/masque/udp/2001:db8::42/443/").index() != 0));
    RUVIA_CHECK((ruvia::parse_http_connect_udp_path("/.well-known/masque/udp/host/65536/").index() != 0));
}
RUVIA_TEST(http_connect_udp_default_template_requires_target_segments) {
    constexpr std::string_view prefix = "/.well-known/masque/udp/";
    for (std::size_t size = 0; size <= prefix.size(); ++size) {
        const auto parsed_value = ruvia::parse_http_connect_udp_path(prefix.substr(0, size));
        RUVIA_CHECK(std::get_if<ruvia::http_connect_udp_error>(&parsed_value) != nullptr);
        if (const auto* error = std::get_if<ruvia::http_connect_udp_error>(&parsed_value)) {
            RUVIA_CHECK(*error == ruvia::http_connect_udp_error::invalid_path);
        }
    }
    for (const auto path : {"/.well-known/masque/udp//", "/.well-known/masque/udp/host/",
             "/.well-known/masque/udp/host/443", "/.well-known/masque/udp/host/443/extra/"}) {
        const auto parsed_value = ruvia::parse_http_connect_udp_path(path);
        RUVIA_CHECK(std::get_if<ruvia::http_connect_udp_error>(&parsed_value) != nullptr);
    }
}
RUVIA_TEST(http_connect_udp_upgrade_handshake_requires_capsule_negotiation) {
    const std::array fields{ruvia::http_header_view{"Host", "example.test"}, ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"}, ruvia::http_header_view{"Capsule-Protocol", "?1;version=1"}};
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request({.authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, fields).has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request({.version_ = ruvia::http_protocol_version::http10, .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request({.method_ = "CONNECT", .authority_ = "example.test", .path_ = "/proxy", .headers_ = fields}).has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 200, fields).has_value());
    RUVIA_CHECK(!ruvia::parse_http_capsule_protocol("?1, ?0").has_value());
    RUVIA_CHECK(!ruvia::parse_http_capsule_protocol("true").has_value());
    const auto disabled = ruvia::parse_http_capsule_protocol("?0");
    RUVIA_CHECK(disabled.has_value() && !*disabled);
    const std::array bad_resp{ruvia::http_header_view{"Upgrade", "websocket"}};
    const auto resp_err =
        ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101, bad_resp);
    RUVIA_CHECK(resp_err == ruvia::http_connect_udp_error::invalid_response);
    const auto abs_req = ruvia::make_parsed_http_request("GET",
        "https://example.test/.well-known/masque/udp/192.0.2.1/443/", fields, {},
        std::pmr::get_default_resource());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request(abs_req).has_value());
    const auto abs_query_req = ruvia::make_parsed_http_request("GET",
        "https://example.test/.well-known/masque/udp/192.0.2.1/443/?target=1", fields, {},
        std::pmr::get_default_resource());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request(abs_query_req).has_value());
}
RUVIA_TEST(http_udp_capsule_session_checks_context_size_and_half_close) {
    ruvia::http_datagram_session session;
    const std::string payload = "udp";
    const auto plan = session.prepare_udp_datagram(payload);
    RUVIA_CHECK(plan.index() == 0 && std::get<0>(plan).prefix_size_ == 3);
    RUVIA_CHECK(std::get<0>(plan).payload_.data() == payload.data());
    std::string wire(std::get<0>(plan).prefix_.data(), std::get<0>(plan).prefix_size_);
    wire.append(payload);
    const auto value = std::string_view(wire).substr(2);
    const auto received = session.receive_udp_datagram(value);
    RUVIA_CHECK(received.index() == 0 && std::get<0>(received) && std::get<0>(received)->payload_.size() == 3);
    wire[2] = 2;
    const auto dropped = session.receive_udp_datagram(value);
    RUVIA_CHECK(dropped.index() == 0 && !std::get<0>(dropped));
    const std::string too_large(65528, 'x');
    RUVIA_CHECK(session.prepare_udp_datagram(too_large).index() != 0);
    session.close_send();
    session.close_receive();
    RUVIA_CHECK(session.prepare_udp_datagram(payload).index() != 0);
    const auto closed = session.receive_udp_datagram(value);
    RUVIA_CHECK(closed.index() == 0 && !std::get<0>(closed));
}

RUVIA_TEST(http_connect_udp_response_preparation_owns_required_fields_and_http1_upgrade_framing) {
    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    response.header("X-Proxy", "test");
    auto prepared = ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http11);
    RUVIA_CHECK((prepared.index() == 0));
    if (prepared.index() == 0) {
        RUVIA_CHECK(std::get<0>(prepared).header("capsule-protocol") == "?1");
        RUVIA_CHECK(std::get<0>(prepared).header("x-proxy") == "test");
        RUVIA_CHECK(std::get<0>(prepared).status() == ruvia::http_status::switching_protocols);
        auto plan = ruvia::prepare_http1_connect_udp_response_head(std::get<0>(prepared));
        RUVIA_CHECK((plan.index() == 0));
        if ((plan.index() == 0)) {
            ruvia::http_response_head_buffer head{std::pmr::polymorphic_allocator<char>{}};
            ruvia::append_http1_response_head(std::get<0>(prepared), head, std::get<0>(plan));
            RUVIA_CHECK(head.view().starts_with("HTTP/1.1 101 "));
            RUVIA_CHECK(head.view().find("Connection: Upgrade\r\n") != std::string_view::npos);
            RUVIA_CHECK(head.view().find("Content-Length:") == std::string_view::npos);
            RUVIA_CHECK(head.view().find("Transfer-Encoding:") == std::string_view::npos);
        }
    }
    ruvia::http_response parameterized;
    parameterized.status(ruvia::http_status::ok);
    parameterized.header("Capsule-Protocol", "?1;version=1");
    auto prepared_param = ruvia::prepare_http_connect_udp_response(
        std::move(parameterized), ruvia::http_protocol_version::http2);
    RUVIA_CHECK(prepared_param.index() == 0);
    if (prepared_param.index() == 0) {
        RUVIA_CHECK_EQ(std::get<0>(prepared_param).header("capsule-protocol").value_or(""), "?1;version=1");
    }
    for (const auto field : {"Content-Length", "Transfer-Encoding", "Content-Type", "Content-Encoding", "Connection", "Upgrade", "Trailer"}) {
        ruvia::http_response invalid;
        invalid.header(field, field == std::string_view("Content-Length") ? "0" : field == std::string_view("Content-Type") ? "application/octet-stream"
                                                                                                                            : "test");
        RUVIA_CHECK((ruvia::prepare_http_connect_udp_response(std::move(invalid), ruvia::http_protocol_version::http11).index() != 0));
    }
    ruvia::http_response disabled;
    disabled.header("Capsule-Protocol", "?0");
    RUVIA_CHECK((ruvia::prepare_http_connect_udp_response(std::move(disabled), ruvia::http_protocol_version::http11).index() != 0));
    ruvia::http_response content;
    content.body("forbidden");
    RUVIA_CHECK((ruvia::prepare_http_connect_udp_response(std::move(content), ruvia::http_protocol_version::http11).index() != 0));
}

RUVIA_TEST(http_connect_udp_http1_request_writer_generates_upgrade_without_message_content) {
    ruvia::http1_client_request_writer writer;
    std::array<char, 4096> buffer;
    const std::array fields_value{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"X-Proxy", "test"}};
    const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = 8443});
    auto result_value = writer.prepare_connect_udp(origin, "/udp", fields_value, buffer);
    const auto* const prepared = std::get_if<ruvia::prepared_http1_client_request>(&result_value);
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        const auto head = prepared->head();
        RUVIA_CHECK(head.starts_with("GET /udp HTTP/1.1\r\n"));
        RUVIA_CHECK(head.find("Host: [::1]:8443\r\n") != std::string_view::npos);
        RUVIA_CHECK(head.find("Upgrade: connect-udp\r\n") != std::string_view::npos);
        RUVIA_CHECK(head.find("Content-Length:") == std::string_view::npos);
        RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string_view::npos);
    }
    auto missing = writer.prepare_connect_udp(origin, "/udp", {}, buffer);
    RUVIA_CHECK(std::holds_alternative<ruvia::http1_client_request_prepare_error>(missing));
    const std::array invalid{ruvia::http_header_view{"Capsule-Protocol", "?0"}};
    auto disabled = writer.prepare_connect_udp(origin, "/udp", invalid, buffer);
    RUVIA_CHECK(std::holds_alternative<ruvia::http1_client_request_prepare_error>(disabled));
    auto bad_target = writer.prepare_connect_udp(origin, "relative", fields_value, buffer);
    RUVIA_CHECK(std::holds_alternative<ruvia::http1_client_request_prepare_error>(bad_target) &&
                std::get<ruvia::http1_client_request_prepare_error>(bad_target) ==
                    ruvia::http1_client_request_prepare_error::invalid_target);
    const std::array with_host{fields_value[0], ruvia::http_header_view{"Host", "[::1]:8443"}};
    auto managed_host = writer.prepare_connect_udp(origin, "/udp", with_host, buffer);
    RUVIA_CHECK(std::holds_alternative<ruvia::http1_client_request_prepare_error>(managed_host) &&
                std::get<ruvia::http1_client_request_prepare_error>(managed_host) ==
                    ruvia::http1_client_request_prepare_error::host_header_managed_by_writer);
}

RUVIA_TEST(http_connect_udp_http2_uses_extended_connect_without_upgrade_fields) {
    const std::array fields{ruvia::http_header_view{"capsule-protocol", "?1"}};
    const ruvia::http_connect_udp_request_view request{
        .version_ = ruvia::http_protocol_version::http2,
        .method_ = "CONNECT",
        .protocol_ = "connect-udp",
        .scheme_ = "https",
        .authority_ = "example.test",
        .path_ = "/proxy",
        .headers_ = fields};
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request(request).has_value());
    auto invalid = request;
    invalid.method_ = "GET";
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request(invalid).has_value());
    invalid = request;
    invalid.protocol_ = "websocket";
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request(invalid).has_value());
    invalid = request;
    invalid.scheme_ = "";
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request(invalid).has_value());
    const std::array upgrade_fields{fields[0], ruvia::http_header_view{"connection", "upgrade"},
        ruvia::http_header_view{"upgrade", "connect-udp"}};
    invalid = request;
    invalid.headers_ = upgrade_fields;
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request(invalid).has_value());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, fields)
            .has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 101, fields)
            .has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, upgrade_fields)
            .has_value());

    ruvia::http_response response;
    response.status(ruvia::http_status::created);
    auto prepared = ruvia::prepare_http_connect_udp_response(
        std::move(response), ruvia::http_protocol_version::http2);
    RUVIA_CHECK(prepared.index() == 0);
    if (prepared.index() == 0) {
        RUVIA_CHECK(std::get<0>(prepared).status() == ruvia::http_status::created);
        RUVIA_CHECK(std::get<0>(prepared).header("capsule-protocol") == "?1");
        RUVIA_CHECK(!std::get<0>(prepared).header("connection").has_value());
        RUVIA_CHECK(!std::get<0>(prepared).header("upgrade").has_value());
    }
}

RUVIA_TEST(http_connect_udp_recipient_treats_capsule_protocol_as_optional) {
    const std::array upgrade{ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Upgrade", "connect-udp"}};
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request({.authority_ = "example.test", .path_ = "/proxy", .headers_ = upgrade})
            .has_value());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http11, 101,
        std::span<const ruvia::http_header_view>(upgrade).subspan(1))
            .has_value());
    for (const auto value : {"1", "?0", "\"yes\""}) {
        const std::array ignored{ruvia::http_header_view{"Capsule-Protocol", value}};
        const ruvia::http_connect_udp_request_view request{.version_ = ruvia::http_protocol_version::http2,
            .method_ = "CONNECT",
            .authority_ = "example.test",
            .path_ = "/proxy",
            .headers_ = ignored};
        RUVIA_CHECK(!ruvia::validate_http_connect_udp_request(request).has_value());
        RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, ignored).has_value());
    }
    const std::array duplicate{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, 200, duplicate).has_value());
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, std::span<const ruvia::http_header_view>{})
            .has_value());
}

RUVIA_TEST(http_connect_udp_rejects_statuses_that_cannot_start_capsule_protocol) {
    const std::array capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    for (const std::uint16_t status : {std::uint16_t{204}, std::uint16_t{205}, std::uint16_t{206}}) {
        RUVIA_CHECK(ruvia::validate_http_connect_udp_response(ruvia::http_protocol_version::http2, status, capsule) ==
                    ruvia::http_connect_udp_error::invalid_response);
        ruvia::http_response response;
        response.status(ruvia::http_status_code::from_value(status));
        RUVIA_CHECK(ruvia::prepare_http_connect_udp_response(std::move(response), ruvia::http_protocol_version::http2).index() != 0);
    }
}

RUVIA_TEST(http_connect_udp_sender_requires_exactly_one_capsule_protocol_true) {
    const auto check = [](std::span<const ruvia::http_header_view> fields) {
        return ruvia::validate_http_connect_udp_request_to_send({.version_ = ruvia::http_protocol_version::http2,
            .method_ = "CONNECT",
            .authority_ = "example.test",
            .path_ = "/proxy",
            .headers_ = fields});
    };
    RUVIA_CHECK(check({}).has_value());
    const std::array false_value{ruvia::http_header_view{"Capsule-Protocol", "?0"}};
    RUVIA_CHECK(check(false_value).has_value());
    const std::array true_value{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    RUVIA_CHECK(!check(true_value).has_value());
}

RUVIA_TEST(http_connect_udp_sender_and_recipient_field_validation) {
    // Sender rejects empty Connection list members and end-to-end field names (RFC 9110 5.6.1.1, 7.6.1).
    const std::array sender_bad_connection{
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", ", Upgrade, Cache-Control, Accept"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/.well-known/masque/udp/192.0.2.1/443/",
                                                                     .headers_ = sender_bad_connection,
                                                                 })
            .has_value());

    // Sender validates Host syntax and authority match.
    const std::array bad_host_h1{
        ruvia::http_header_view{"Host", "invalid@host:99999"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = bad_host_h1,
                                                                 })
            .has_value());

    const std::array mismatched_host_h1{
        ruvia::http_header_view{"Host", "other.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", "connect-udp"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http11,
                                                                     .method_ = "GET",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = mismatched_host_h1,
                                                                 })
            .has_value());

    // HTTP/2 rejects connection-specific headers (Keep-Alive, Proxy-Connection, TE) and non-http(s) schemes.
    const std::array h2_keep_alive{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Keep-Alive", "timeout=5"},
    };
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http2,
                                                                     .method_ = "CONNECT",
                                                                     .protocol_ = "connect-udp",
                                                                     .scheme_ = "https",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = h2_keep_alive,
                                                                 })
            .has_value());
    const std::array valid_capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request_to_send({
                                                                     .version_ = ruvia::http_protocol_version::http2,
                                                                     .method_ = "CONNECT",
                                                                     .protocol_ = "connect-udp",
                                                                     .scheme_ = "not a valid scheme!",
                                                                     .authority_ = "example.test",
                                                                     .path_ = "/proxy",
                                                                     .headers_ = valid_capsule,
                                                                 })
            .has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, h2_keep_alive)
            .has_value());
    ruvia::http_response h2_resp;
    h2_resp.status(ruvia::http_status::ok);
    h2_resp.header("Keep-Alive", "timeout=5");
    RUVIA_CHECK(ruvia::prepare_http_connect_udp_response(
                    std::move(h2_resp), ruvia::http_protocol_version::http2)
                    .index() != 0);

    // Recipient parses Upgrade = #protocol (ignoring empty elements and allowing additional tokens on requests).
    const std::array list_fields{
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp, fallback"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    const auto parsed_req = ruvia::make_parsed_http_request(
        "GET", "/.well-known/masque/udp/192.0.2.1/443/", list_fields, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(ruvia::is_http_connect_udp_upgrade_request(parsed_req));
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request(parsed_req).has_value());

    const std::array empty_elem_resp{
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", ", connect-udp,"},
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
    };
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http11, 101, empty_elem_resp)
            .has_value());

    // Recipient allows Content-Length: 0 on requests, rejects non-zero Content-Length on requests,
    // and rejects Content-Length on 2xx responses (RFC 9297 3.2).
    const std::array cl0_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "0"},
    };
    RUVIA_CHECK(!ruvia::validate_http_connect_udp_request({
                                                              .version_ = ruvia::http_protocol_version::http2,
                                                              .method_ = "CONNECT",
                                                              .protocol_ = "connect-udp",
                                                              .scheme_ = "https",
                                                              .authority_ = "example.test",
                                                              .path_ = "/proxy",
                                                              .headers_ = cl0_req,
                                                          })
            .has_value());
    const std::array cl5_req{
        ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Content-Length", "5"},
    };
    RUVIA_CHECK(ruvia::validate_http_connect_udp_request({
                                                             .version_ = ruvia::http_protocol_version::http2,
                                                             .method_ = "CONNECT",
                                                             .protocol_ = "connect-udp",
                                                             .scheme_ = "https",
                                                             .authority_ = "example.test",
                                                             .path_ = "/proxy",
                                                             .headers_ = cl5_req,
                                                         })
            .has_value());
    RUVIA_CHECK(ruvia::validate_http_connect_udp_response(
        ruvia::http_protocol_version::http2, 200, cl0_req)
            .has_value());
}
