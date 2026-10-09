#include "ruvia/http/websocket_handshake.h"

#include <array>
#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http1/http1_server_request_parser.h"
#include "ruvia/http/http_request.h"

#include "http2/http2_websocket_handshake.h"
#include "test_harness.h"

namespace {

using ruvia::http_request;
using ruvia::detail::hpack_decoder;
using ruvia::detail::http1_server_request_parser;
using ruvia::detail::http2_encode_websocket_handshake_headers;
using ruvia::detail::http2_stream_state;
using ruvia::detail::make_websocket_server_negotiation;
using ruvia::detail::validate_http2_websocket_handshake;

struct collector final {
    std::vector<std::pair<std::string, std::string>> headers_;
};

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<collector*>(target)->headers_.emplace_back(name, value);
    return true;
}

bool has_header(const collector& fields_value, std::string_view name, std::string_view value) {
    for (const auto& field : fields_value.headers_) {
        if (field.first == name && field.second == value) {
            return true;
        }
    }
    return false;
}

bool has_header_name(const collector& fields_value, std::string_view name) {
    for (const auto& field : fields_value.headers_) {
        if (field.first == name) {
            return true;
        }
    }
    return false;
}

http_request parse_request(std::string_view raw_request) {
    http1_server_request_parser parser;
    auto parsed_value = parser.parse_message(raw_request);
    return std::move(parsed_value.request_);
}

http_request request_with_protocol() {
    return parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "\r\n");
}

http_request request_with_version() {
    return parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n");
}

http_request request_with_bad_version() {
    return parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 8\r\n"
        "\r\n");
}

http2_stream_state make_stream() {
    return http2_stream_state(1, std::pmr::new_delete_resource());
}

[[nodiscard]] bool accepts_websocket_handshake(
    const http2_stream_state& stream, const http_request& request) {
    const auto result_value = validate_http2_websocket_handshake(stream, request);
    return result_value.accepted() != nullptr;
}

[[nodiscard]] bool rejects_websocket_handshake(
    const http2_stream_state& stream, const http_request& request) {
    const auto result_value = validate_http2_websocket_handshake(stream, request);
    return result_value.failure() != nullptr;
}

}  // namespace

RUVIA_TEST(websocket_subprotocol_negotiation) {
    const auto request = request_with_protocol();
    constexpr std::array<std::string_view, 2> preferred_protocols{"superchat", "chat"};
    constexpr std::array<std::string_view, 1> chat_protocol{"chat"};
    constexpr std::array<std::string_view, 1> binary_protocol{"binary"};
    // Server preference wins: the first supported token the client also offered.
    const auto preferred =
        make_websocket_server_negotiation(request, {.supported_subprotocols_ = preferred_protocols});
    RUVIA_CHECK_EQ(preferred.subprotocol(), std::string_view("superchat"));
    const auto chat =
        make_websocket_server_negotiation(request, {.supported_subprotocols_ = chat_protocol});
    RUVIA_CHECK_EQ(chat.subprotocol(), std::string_view("chat"));
    // No overlap yields no subprotocol.
    const auto no_overlap =
        make_websocket_server_negotiation(request, {.supported_subprotocols_ = binary_protocol});
    RUVIA_CHECK(no_overlap.subprotocol().empty());

    // A request offering nothing yields no subprotocol.
    const auto none = parse_request("GET /ws HTTP/1.1\r\nHost: example.test\r\n\r\n");
    const auto no_offer =
        make_websocket_server_negotiation(none, {.supported_subprotocols_ = chat_protocol});
    RUVIA_CHECK(no_offer.subprotocol().empty());
}

RUVIA_TEST(websocket_server_negotiation_owns_selected_subprotocol) {
    const auto request = request_with_protocol();
    std::string supported = "chat";
    const std::array<std::string_view, 1> supported_views{supported};
    const auto negotiation =
        make_websocket_server_negotiation(request, {.supported_subprotocols_ = supported_views});

    supported.front() = 'X';

    RUVIA_CHECK_EQ(negotiation.subprotocol(), std::string_view("chat"));
}

RUVIA_TEST(websocket_h1_and_h2_share_compression_negotiation_and_serialization) {
    struct case_value final {
        std::string_view extension_header_;
        ruvia::websocket_compression expected_;
    };
    constexpr case_value cases[] = {
        {"", (ruvia::websocket_compression{})},
        {"Sec-WebSocket-Extensions: permessage-deflate\r\n",
            (ruvia::websocket_compression{.enabled_ = true})},
        {"Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15\r\n",
            (ruvia::websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15})},
    };

    for (const auto& test_case : cases) {
        std::string raw =
            "GET /ws HTTP/1.1\r\n"
            "Host: example.test\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        raw.append(test_case.extension_header_);
        raw.append("\r\n");
        const auto request = parse_request(raw);
        const auto h1 = ruvia::make_websocket_server_handshake(request, {});
        const auto h2 = make_websocket_server_negotiation(request, {});

        RUVIA_CHECK(h1.compression() == test_case.expected_);
        RUVIA_CHECK(h2.compression() == test_case.expected_);
        const auto extension = ruvia::detail::get_websocket_compression_extension(test_case.expected_);
        RUVIA_CHECK_EQ(h2.extensions(), extension.view());

        std::string response;
        h1.for_each_response_part([&response](std::string_view part) { response.append(part); });
        const auto header_value =
            std::string("Sec-WebSocket-Extensions: ") + std::string(extension.view()) + "\r\n";
        RUVIA_CHECK_EQ(response.find("Sec-WebSocket-Extensions:"),
            extension.empty() ? std::string::npos : response.find(header_value));
    }
}

RUVIA_TEST(websocket_request_validity_requires_all_conditions) {
    const auto request = request_with_version();

    auto valid = make_stream();
    valid.set_protocol("websocket");
    RUVIA_CHECK(valid.begin_extended_connect());
    RUVIA_CHECK(valid.finalize_remote_connect_head());
    RUVIA_CHECK(accepts_websocket_handshake(valid, request));

    for (const auto forbidden : {std::string_view("Sec-WebSocket-Key: key-is-h1-only\r\n"),
             std::string_view("Sec-WebSocket-Accept: accept-is-h1-only\r\n")}) {
        std::string raw =
            "GET /ws HTTP/1.1\r\nHost: example.test\r\nSec-WebSocket-Version: 13\r\n";
        raw.append(forbidden);
        raw.append("\r\n");
        RUVIA_CHECK(rejects_websocket_handshake(valid, parse_request(raw)));
    }

    // The complete HTTP/2 field section must be decoded before the helper can
    // validate websocket-specific request headers. A synthetically pending
    // CONNECT is not yet a complete opening handshake.
    auto unfinalized = make_stream();
    unfinalized.set_protocol("websocket");
    RUVIA_CHECK(unfinalized.begin_extended_connect());
    RUVIA_CHECK(rejects_websocket_handshake(unfinalized, request));

    // A completed/rejected CONNECT is no longer an opening handshake.
    auto open_tunnel = make_stream();
    open_tunnel.set_protocol("websocket");
    RUVIA_CHECK(open_tunnel.begin_extended_connect());
    RUVIA_CHECK(open_tunnel.finalize_remote_connect_head());
    RUVIA_CHECK(open_tunnel.accept_connect());
    RUVIA_CHECK(rejects_websocket_handshake(open_tunnel, request));

    // Without the extended-CONNECT websocket marker (:method CONNECT +
    // :protocol websocket, RFC 8441) it is not a websocket handshake at all.
    auto no_extended_connect = make_stream();
    RUVIA_CHECK(rejects_websocket_handshake(no_extended_connect, request));

    // A Content-Length must not be present on a websocket CONNECT.
    auto with_content_length = make_stream();
    with_content_length.set_protocol("websocket");
    RUVIA_CHECK(with_content_length.begin_extended_connect());
    RUVIA_CHECK(with_content_length.finalize_remote_connect_head());
    RUVIA_CHECK(with_content_length.declare_remote_content_length(5));
    RUVIA_CHECK(rejects_websocket_handshake(with_content_length, request));

    // A websocket opening handshake must leave the client-to-server send half
    // open for websocket frames. A CONNECT head that already carried END_STREAM
    // is a half-closed CONNECT decision, not an admissible websocket tunnel start.
    auto half_closed = make_stream();
    half_closed.set_protocol("websocket");
    RUVIA_CHECK(half_closed.record_remote_head_end_stream());
    RUVIA_CHECK(half_closed.begin_extended_connect());
    RUVIA_CHECK(half_closed.finalize_remote_connect_head());
    RUVIA_CHECK(half_closed.remote_receive().connect_pending_end_stream() != nullptr);
    RUVIA_CHECK(rejects_websocket_handshake(half_closed, request));

    // The Sec-websocket-Version must be exactly 13.
    const auto bad_version = request_with_bad_version();
    auto stream = make_stream();
    stream.set_protocol("websocket");
    RUVIA_CHECK(stream.begin_extended_connect());
    RUVIA_CHECK(stream.finalize_remote_connect_head());
    const auto unsupported_version = validate_http2_websocket_handshake(stream, bad_version);
    RUVIA_CHECK(unsupported_version.failure() != nullptr);
    if (const auto* failure = unsupported_version.failure()) {
        const auto error = failure->protocol_error();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::bad_request);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::http_response response;
        failure->apply_required_response_headers(response);
        RUVIA_CHECK_EQ(response.header("Sec-WebSocket-Version"), std::string_view("13"));
    }
}

RUVIA_TEST(websocket_subprotocol_offers_are_validated_for_extended_connect) {
    const auto malformed = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat, bad token\r\n"
        "\r\n");
    auto malformed_stream = make_stream();
    malformed_stream.set_protocol("websocket");
    RUVIA_CHECK(malformed_stream.begin_extended_connect());
    RUVIA_CHECK(malformed_stream.finalize_remote_connect_head());
    RUVIA_CHECK(rejects_websocket_handshake(malformed_stream, malformed));

    const auto duplicate = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Protocol: superchat, chat\r\n"
        "\r\n");
    auto duplicate_stream = make_stream();
    duplicate_stream.set_protocol("websocket");
    RUVIA_CHECK(duplicate_stream.begin_extended_connect());
    RUVIA_CHECK(duplicate_stream.finalize_remote_connect_head());
    RUVIA_CHECK(rejects_websocket_handshake(duplicate_stream, duplicate));
}

RUVIA_TEST(websocket_extension_offers_are_validated_for_extended_connect) {
    const auto malformed = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; =value\r\n"
        "\r\n");
    auto stream = make_stream();
    stream.set_protocol("websocket");
    RUVIA_CHECK(stream.begin_extended_connect());
    RUVIA_CHECK(stream.finalize_remote_connect_head());
    RUVIA_CHECK(rejects_websocket_handshake(stream, malformed));
}

RUVIA_TEST(http2_websocket_handshake_does_not_invent_server_product) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "\r\n");
    constexpr std::array<std::string_view, 1> chat_protocol{"chat"};
    const auto negotiation =
        make_websocket_server_negotiation(request, {.supported_subprotocols_ = chat_protocol});
    std::pmr::string block(std::pmr::get_default_resource());
    http2_encode_websocket_handshake_headers(block, negotiation);

    collector fields;
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    const auto decode_result = decoder.decode(block, &fields, &collect);
    RUVIA_CHECK(decode_result.decoded());
    RUVIA_CHECK(has_header(fields, ":status", "200"));
    RUVIA_CHECK(has_header(fields, "sec-websocket-protocol", "chat"));
    const auto extension = ruvia::detail::get_websocket_compression_extension((ruvia::websocket_compression{.enabled_ = true}));
    RUVIA_CHECK(has_header(fields, "sec-websocket-extensions", extension.view()));
    RUVIA_CHECK(has_header_name(fields, "date"));
    RUVIA_CHECK(!has_header_name(fields, "server"));
}

RUVIA_TEST(websocket_h2_handshake_encodes_owned_cookies_and_application_date) {
    std::string cookie = "sid=0123456789abcdef; HttpOnly";
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", cookie),
        ruvia::http_header_view("Set-Cookie", "theme=dark"),
        ruvia::http_header_view("Date", "Wed, 21 Oct 2015 07:28:00 GMT"),
        ruvia::http_header_view("Alt-Svc", "h3=\":443\"; ma=86400")};
    const auto negotiation = make_websocket_server_negotiation(request_with_version(), {.response_headers_ = fields_value});
    cookie.assign(cookie.size(), 'x');
    std::pmr::string block(std::pmr::get_default_resource());
    http2_encode_websocket_handshake_headers(block, negotiation);
    collector decoded;
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    const auto result_value = decoder.decode(block, &decoded, &collect);
    RUVIA_CHECK(result_value.decoded());
    RUVIA_CHECK(has_header(decoded, ":status", "200"));
    RUVIA_CHECK(has_header(decoded, "set-cookie", "sid=0123456789abcdef; HttpOnly"));
    RUVIA_CHECK(has_header(decoded, "set-cookie", "theme=dark"));
    RUVIA_CHECK(has_header(decoded, "date", "Wed, 21 Oct 2015 07:28:00 GMT"));
    RUVIA_CHECK(has_header(decoded, "alt-svc", "h3=\":443\"; ma=86400"));
    RUVIA_CHECK_EQ(decoded.headers_.size(), std::size_t{5});
    RUVIA_CHECK(!has_header_name(decoded, "connection"));
    RUVIA_CHECK(!has_header_name(decoded, "upgrade"));
}
