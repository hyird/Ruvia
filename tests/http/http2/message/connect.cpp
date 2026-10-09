#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "http2/http2_connection.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_hpack.h"
#include "http2/http2_tunnel_state.h"
#include "http2/http2_websocket_handshake.h"
#include "http2/http2_window_update.h"
#include "test_harness.h"

namespace {

using ruvia::http2_request_head_submit_result;
using ruvia::detail::hpack_decoder;
using ruvia::detail::hpack_encoder;
using ruvia::detail::http2_connect_form;
using ruvia::detail::http2_connect_pending;
using ruvia::detail::http2_connect_rejected;
using ruvia::detail::http2_connection;
using ruvia::detail::http2_data_submit_status;
using ruvia::detail::http2_end_stream;
using ruvia::detail::http2_error_code;
using ruvia::detail::http2_event;
using ruvia::detail::http2_event_kind;
using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_is_pending_websocket_connect;
using ruvia::detail::http2_not_connect;
using ruvia::detail::http2_request_head_submit_error;
using ruvia::detail::http2_role;
using ruvia::detail::http2_stream_state;
using ruvia::detail::http2_submit_status;
using ruvia::detail::http2_tunnel_open;
using ruvia::detail::http2_tunnel_state;

std::uint32_t submitted_request_stream_id(const http2_request_head_submit_result& result_value) {
    if (const auto* submitted = result_value.submitted()) {
        return submitted->stream_id();
    }
    throw std::runtime_error("HTTP/2 CONNECT head was not submitted");
}

http2_request_head_submit_error request_head_submit_error(const http2_request_head_submit_result& result_value) {
    if (const auto* failure = result_value.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 CONNECT head did not fail");
}

struct header_observation final {
    std::string method_;
    std::string protocol_;
    std::string scheme_;
    std::string authority_;
    std::string path_;
    std::string status_;
    std::size_t content_length_count_{0};
    std::size_t path_count_{0};
};

bool observe_header(void* target, std::string_view name, std::string_view value) {
    auto& observation_value = *static_cast<header_observation*>(target);
    auto assign = [value](std::string& field) { field.assign(value.data(), value.size()); };
    if (name == ":method") {
        assign(observation_value.method_);
    } else if (name == ":protocol") {
        assign(observation_value.protocol_);
    } else if (name == ":scheme") {
        assign(observation_value.scheme_);
    } else if (name == ":authority") {
        assign(observation_value.authority_);
    } else if (name == ":path") {
        ++observation_value.path_count_;
        assign(observation_value.path_);
    } else if (name == ":status") {
        assign(observation_value.status_);
    } else if (name == "content-length") {
        ++observation_value.content_length_count_;
    }
    return true;
}

std::pmr::string frame(std::pmr::memory_resource* resource, http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload = {}) {
    std::pmr::string bytes(resource);
    char header[9];
    ruvia::detail::http2_encode_frame_header(
        header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id);
    bytes.append(header, sizeof(header));
    bytes.append(payload.data(), payload.size());
    return bytes;
}

void handshake(http2_connection& connection) {
    connection.begin_connection();
    connection.consume_output(connection.pending_output().size());
    if (connection.role() == http2_role::server) {
        const auto preface = connection.feed(ruvia::detail::http2_client_preface);
        if (preface != ruvia::detail::http2_feed_result::accepted) {
            throw std::runtime_error("server rejected valid client preface");
        }
    }
    const auto settings = frame(std::pmr::get_default_resource(), http2_frame_type::settings, 0, 0);
    const auto result_value = connection.feed(std::string_view(settings.data(), settings.size()));
    if (result_value != ruvia::detail::http2_feed_result::accepted || !connection.received_peer_settings()) {
        throw std::runtime_error("connection rejected valid initial SETTINGS");
    }
    connection.consume_output(connection.pending_output().size());
}

void begin_client(http2_connection& client) {
    client.begin_connection();
    client.consume_output(client.pending_output().size());
}

void enable_extended_connect(http2_connection& client) {
    char settings[15];
    auto* out = ruvia::detail::http2_write_frame_header(settings, 6, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::enable_connect_protocol, 1);
    (void)out;
    (void)client.feed(std::string_view(settings, sizeof(settings)));
    client.consume_output(client.pending_output().size());
}

void drain_events(http2_connection& connection) {
    while (connection.next_event().has_value()) {
    }
}

void feed_standard_connect(http2_connection& server, std::pmr::memory_resource* resource,
    std::uint32_t stream_id = 1, std::uint8_t extra_flags = 0) {
    std::pmr::string block(resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":authority", "example.test:443");
    const auto request = frame(resource, http2_frame_type::headers,
        static_cast<std::uint8_t>(ruvia::detail::http2_flag_end_headers | extra_flags), stream_id,
        std::string_view(block.data(), block.size()));
    (void)server.feed(std::string_view(request.data(), request.size()));
}

void open_standard_tunnel(http2_connection& server, std::pmr::memory_resource* resource) {
    feed_standard_connect(server, resource);
    drain_events(server);
    ruvia::http_response response({.resource_ = resource});
    response.status(ruvia::http_status::ok);
    (void)server.submit_connect_response_head(1, response);
    server.consume_output(server.pending_output().size());
}

header_observation decode_single_header_frame(std::pmr::memory_resource* resource,
    std::string_view bytes_value, ruvia::testing::test_context& ruvia_ctx) {
    header_observation observation;
    RUVIA_CHECK(bytes_value.size() >= 9);
    if (bytes_value.size() < 9) {
        return observation;
    }
    const auto header_value = ruvia::detail::http2_parse_frame_header(bytes_value.substr(0, 9));
    RUVIA_CHECK_EQ(header_value.type_, static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK(bytes_value.size() >= 9 + header_value.length_);
    if (bytes_value.size() < 9 + header_value.length_) {
        return observation;
    }
    hpack_decoder decoder({.resource_ = resource});
    const auto decode_result =
        decoder.decode(bytes_value.substr(9, header_value.length_), &observation, &observe_header);
    RUVIA_CHECK(decode_result.decoded());
    return observation;
}

}  // namespace

RUVIA_TEST(http2_tunnel_state_alternatives_own_valid_transitions) {
    http2_tunnel_state state;
    RUVIA_CHECK(state.not_connect() != nullptr);
    RUVIA_CHECK(state.pending() == nullptr);
    RUVIA_CHECK(state.open() == nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.accept());
    RUVIA_CHECK(!state.reject());
    RUVIA_CHECK(!state.begin(static_cast<http2_connect_form>(0xFF)));
    RUVIA_CHECK(state.not_connect() != nullptr);

    RUVIA_CHECK(state.begin(http2_connect_form::standard));
    RUVIA_CHECK(state.not_connect() == nullptr);
    RUVIA_CHECK(state.pending() != nullptr);
    RUVIA_CHECK(state.pending()->form() == http2_connect_form::standard);
    RUVIA_CHECK(state.open() == nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.begin(http2_connect_form::extended));
    RUVIA_CHECK(state.accept());
    RUVIA_CHECK(state.pending() == nullptr);
    RUVIA_CHECK(state.open() != nullptr);
    RUVIA_CHECK(state.rejected() == nullptr);
    RUVIA_CHECK(!state.accept());
    RUVIA_CHECK(!state.reject());
    RUVIA_CHECK(!state.begin(http2_connect_form::extended));

    http2_tunnel_state rejected;
    RUVIA_CHECK(rejected.begin(http2_connect_form::extended));
    RUVIA_CHECK(rejected.pending() != nullptr);
    RUVIA_CHECK(rejected.pending()->form() == http2_connect_form::extended);
    RUVIA_CHECK(rejected.reject());
    RUVIA_CHECK(rejected.pending() == nullptr);
    RUVIA_CHECK(rejected.open() == nullptr);
    RUVIA_CHECK(rejected.rejected() != nullptr);
    RUVIA_CHECK(!rejected.accept());
}

RUVIA_TEST(http2_connect_client_standard_head_owns_shape_and_gates_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    begin_client(client);

    const ruvia::http_header_view host[] = {{"host", "example.test:443"}};
    const ruvia::http_header_view length[] = {{"content-length", "0"}};
    const ruvia::http_header_view transfer[] = {{"te", "trailers"}};
    RUVIA_CHECK(request_head_submit_error(client.submit_connect_request_head("example.test")) ==
                http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_connect_request_head("example.test:0")) ==
                http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_connect_request_head("example.test:443", host)) ==
                http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_connect_request_head(
                    "example.test:443", length)) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_connect_request_head(
                    "example.test:443", transfer)) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    const auto submitted = client.submit_connect_request_head("example.test:443");
    RUVIA_CHECK(submitted.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(submitted);
    RUVIA_CHECK_EQ(stream_id, std::uint32_t{1});
    const auto out = client.pending_output();
    const auto frame_header = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK((frame_header.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    const auto observed_value = decode_single_header_frame(&resource, out, ruvia_ctx);
    RUVIA_CHECK_EQ(observed_value.method_, std::string("CONNECT"));
    RUVIA_CHECK_EQ(observed_value.authority_, std::string("example.test:443"));
    RUVIA_CHECK(observed_value.scheme_.empty());
    RUVIA_CHECK(observed_value.path_.empty());
    RUVIA_CHECK(observed_value.protocol_.empty());
    RUVIA_CHECK_EQ(observed_value.content_length_count_, std::size_t{0});

    const auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == http2_connect_form::standard);
    RUVIA_CHECK(stream->local_send().connect_pending() != nullptr);
    RUVIA_CHECK(stream->local_send().tunnel_open() == nullptr);
    RUVIA_CHECK(stream->local_content().forbidden() != nullptr);
    RUVIA_CHECK(client.submit_data(stream_id, "early", http2_end_stream::keep_open) ==
                http2_data_submit_status::invalid_state);
}

RUVIA_TEST(http2_connect_client_extended_head_requires_setting_and_protocol_contract) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    begin_client(client);
    RUVIA_CHECK(
        request_head_submit_error(client.submit_extended_connect_request_head("connect-udp", "https",
            "example.test", "/masque")) == http2_request_head_submit_error::peer_capability_unavailable);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    enable_extended_connect(client);
    const ruvia::http_header_view raw_length[] = {{"content-length", "0"}};
    const ruvia::http_header_view expect_continue[] = {{"expect", "100-continue"}};
    RUVIA_CHECK(
        request_head_submit_error(client.submit_extended_connect_request_head("bad protocol", "https",
            "example.test", "/masque")) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(
        request_head_submit_error(client.submit_extended_connect_request_head("connect-udp", "https",
            "example.test", "relative")) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_extended_connect_request_head("example-tunnel",
                    "https", "example.test", "*")) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_extended_connect_request_head("example-tunnel",
                    "https", "example.test", "")) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(
        request_head_submit_error(client.submit_extended_connect_request_head("example-tunnel", "https",
            "user@example.test", "/tunnel")) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(
        request_head_submit_error(client.submit_extended_connect_request_head("connect-udp", "https",
            "example.test", "/masque", raw_length)) == http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(request_head_submit_error(client.submit_extended_connect_request_head(
                    "connect-udp", "https", "example.test", "/masque", expect_continue)) ==
                http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    const auto generic = client.submit_extended_connect_request_head(
        "example-tunnel", "custom+transport", "user:secret@example.test", "");
    RUVIA_CHECK(generic.submitted() != nullptr);
    const auto generic_stream = submitted_request_stream_id(generic);
    RUVIA_CHECK_EQ(generic_stream, std::uint32_t{1});
    auto out = client.pending_output();
    const auto frame_header = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK((frame_header.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    const auto observed_value = decode_single_header_frame(&resource, out, ruvia_ctx);
    RUVIA_CHECK_EQ(observed_value.method_, std::string("CONNECT"));
    RUVIA_CHECK_EQ(observed_value.protocol_, std::string("example-tunnel"));
    RUVIA_CHECK_EQ(observed_value.scheme_, std::string("custom+transport"));
    RUVIA_CHECK_EQ(observed_value.authority_, std::string("user:secret@example.test"));
    RUVIA_CHECK_EQ(observed_value.path_count_, std::size_t{1});
    RUVIA_CHECK(observed_value.path_.empty());
    const auto* generic_pending = client.stream(generic_stream)->tunnel().pending();
    RUVIA_CHECK(generic_pending != nullptr);
    RUVIA_CHECK(generic_pending->form() == http2_connect_form::extended);
    RUVIA_CHECK_EQ(
        client.stream(generic_stream)->request_protocol(), std::string_view("example-tunnel"));
    RUVIA_CHECK_EQ(
        client.stream(generic_stream)->request_scheme(), std::string_view("custom+transport"));
    client.consume_output(out.size());

    RUVIA_CHECK(request_head_submit_error(client.submit_extended_connect_request_head("websocket", "https",
                    "example.test", "/ws")) == http2_request_head_submit_error::invalid_message);
    const ruvia::http_header_view websocket_headers[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-protocol", "chat, superchat"},
        {"sec-websocket-extensions", "permessage-deflate; client_max_window_bits"},
    };
    RUVIA_CHECK(request_head_submit_error(client.submit_extended_connect_request_head(
                    "websocket", "gemini", "example.test", "/ws", websocket_headers)) ==
                http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.stream(3) == nullptr);
    const auto websocket_value = client.submit_extended_connect_request_head(
        "WebSocket", "HTTPS", "example.test", "/ws", websocket_headers);
    RUVIA_CHECK(websocket_value.submitted() != nullptr);
    const auto websocket_stream = submitted_request_stream_id(websocket_value);
    RUVIA_CHECK_EQ(websocket_stream, std::uint32_t{3});
    out = client.pending_output();
    const auto websocket_observed = decode_single_header_frame(&resource, out, ruvia_ctx);
    RUVIA_CHECK_EQ(websocket_observed.protocol_, std::string("websocket"));
    RUVIA_CHECK_EQ(
        client.stream(websocket_stream)->request_protocol(), std::string_view("websocket"));
    RUVIA_CHECK(http2_is_pending_websocket_connect(*client.stream(websocket_stream)));
}

// RFC 8441 keeps the RFC 6455 syntax of Sec-websocket-Protocol and
// Sec-websocket-Extensions on Extended CONNECT. The dedicated websocket
// sender must reject malformed offers before allocating a stream or emitting
// an HPACK block that a conformant server can only reject.
RUVIA_TEST(http2_connect_client_websocket_offer_rejection_is_transactional) {
    const auto rejects = [&](std::span<const ruvia::http_header_view> headers) {
        std::pmr::monotonic_buffer_resource resource;
        http2_connection client(&resource, http2_role::client);
        begin_client(client);
        enable_extended_connect(client);

        const auto result_value = client.submit_extended_connect_request_head(
            "websocket", "https", "example.test", "/ws", headers);
        RUVIA_CHECK(result_value.failure() != nullptr);
        if (result_value.failure() != nullptr) {
            RUVIA_CHECK(result_value.failure()->error() == http2_request_head_submit_error::invalid_message);
        }
        RUVIA_CHECK(client.pending_output().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const ruvia::http_header_view empty_subprotocol[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-protocol", ""},
    };
    rejects(empty_subprotocol);

    const ruvia::http_header_view malformed_subprotocol[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-protocol", "chat, bad token"},
    };
    rejects(malformed_subprotocol);

    const ruvia::http_header_view duplicate_subprotocol[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-protocol", "chat"},
        {"sec-websocket-protocol", "superchat, chat"},
    };
    rejects(duplicate_subprotocol);

    const ruvia::http_header_view empty_extensions[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-extensions", ""},
    };
    rejects(empty_extensions);

    const ruvia::http_header_view malformed_extensions[] = {
        {"sec-websocket-version", "13"},
        {"sec-websocket-extensions", "permessage-deflate;"},
    };
    rejects(malformed_extensions);
}

RUVIA_TEST(http2_connect_server_accepts_standard_tunnel_and_preserves_half_close) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    feed_standard_connect(server, &resource);

    auto event = server.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!server.next_event().has_value());
    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == http2_connect_form::standard);

    ruvia::http_response invalid_body({.resource_ = &resource});
    invalid_body.status(ruvia::http_status::ok);
    invalid_body.body("not tunnel metadata");
    RUVIA_CHECK(
        server.submit_connect_response_head(1, invalid_body) == http2_submit_status::invalid_message);
    ruvia::http_response invalid_length({.resource_ = &resource});
    invalid_length.status(ruvia::http_status::ok);
    invalid_length.header("Content-Length", "0");
    RUVIA_CHECK(
        server.submit_connect_response_head(1, invalid_length) == http2_submit_status::invalid_message);
    ruvia::http_response invalid_connection({.resource_ = &resource});
    invalid_connection.status(ruvia::http_status::ok);
    invalid_connection.header("Connection", "close");
    RUVIA_CHECK(server.submit_connect_response_head(1, invalid_connection) ==
                http2_submit_status::invalid_message);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(stream->tunnel().pending() != nullptr);

    ruvia::http_response accepted({.resource_ = &resource});
    accepted.status(ruvia::http_status::ok);
    accepted.header("X-Tunnel", "ready");
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == http2_submit_status::accepted);
    const auto response_bytes = server.pending_output();
    const auto response_frame = ruvia::detail::http2_parse_frame_header(response_bytes.substr(0, 9));
    RUVIA_CHECK((response_frame.flags_ & ruvia::detail::http2_flag_end_stream) == 0);
    const auto observed_value = decode_single_header_frame(&resource, response_bytes, ruvia_ctx);
    RUVIA_CHECK_EQ(observed_value.status_, std::string("200"));
    RUVIA_CHECK_EQ(observed_value.content_length_count_, std::size_t{0});
    server.consume_output(response_bytes.size());
    RUVIA_CHECK(stream->tunnel().open() != nullptr);
    RUVIA_CHECK(stream->local_send().tunnel_open() != nullptr);

    const auto peer_data = frame(&resource, http2_frame_type::data, 0, 1, "peer");
    (void)server.feed(std::string_view(peer_data.data(), peer_data.size()));
    auto peer_data_event = server.next_event().value();
    RUVIA_CHECK(peer_data_event.kind() == http2_event_kind::tunnel_data);
    RUVIA_CHECK_EQ(peer_data_event.tunnel_data()->bytes(), std::string_view("peer"));
    drain_events(server);
    server.consume_output(server.pending_output().size());

    const auto peer_fin =
        frame(&resource, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, 1, "fin");
    (void)server.feed(std::string_view(peer_fin.data(), peer_fin.size()));
    auto peer_fin_event = server.next_event().value();
    RUVIA_CHECK(peer_fin_event.kind() == http2_event_kind::tunnel_data);
    RUVIA_CHECK_EQ(peer_fin_event.tunnel_data()->bytes(), std::string_view("fin"));
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::tunnel_end);
    RUVIA_CHECK(!server.next_event().has_value());
    server.consume_output(server.pending_output().size());
    RUVIA_CHECK(stream->remote_receive().end_stream() != nullptr);

    // Peer FIN closes only its send half; the server can still finish its own half.
    RUVIA_CHECK(server.submit_data(1, "reply", http2_end_stream::keep_open) ==
                http2_data_submit_status::accepted);
    server.consume_output(server.pending_output().size());
    RUVIA_CHECK(
        server.submit_data(1, {}, http2_end_stream::end_stream) == http2_data_submit_status::accepted);
    server.consume_output(server.pending_output().size());

    const auto after_fin = frame(&resource, http2_frame_type::data, 0, 1, "late");
    (void)server.feed(std::string_view(after_fin.data(), after_fin.size()));
    // Both halves are now closed. Tolerant minimal processing discards the late
    // peer frame and banks its connection credit below the batching threshold;
    // it never emits a second stream frame after closure.
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(!server.connection_error().has_value());
}

RUVIA_TEST(http2_connect_client_success_ignores_length_and_uses_tunnel_events) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    const auto request = client.submit_connect_request_head("example.test:443");
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "200");
    hpack_encoder::encode_header(response, "content-length", "not-a-number");
    const auto head =
        frame(&resource, http2_frame_type::headers, ruvia::detail::http2_flag_end_headers, stream_id,
            std::string_view(response.data(), response.size()));
    (void)client.feed(std::string_view(head.data(), head.size()));
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());
    auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->tunnel().open() != nullptr);
    RUVIA_CHECK(stream->remote_content().allowed_without_length() != nullptr);
    RUVIA_CHECK(stream->local_send().tunnel_open() != nullptr);

    const auto data = frame(
        &resource, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, stream_id, "opaque");
    (void)client.feed(std::string_view(data.data(), data.size()));
    auto event = client.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::tunnel_data);
    RUVIA_CHECK_EQ(event.tunnel_data()->bytes(), std::string_view("opaque"));
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::tunnel_end);
    RUVIA_CHECK(!client.next_event().has_value());
    client.consume_output(client.pending_output().size());

    RUVIA_CHECK(client.submit_data(stream_id, "last", http2_end_stream::end_stream) ==
                http2_data_submit_status::accepted);
}

RUVIA_TEST(http2_connect_client_rejection_closes_request_half_and_decodes_response_body) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection client(&resource, http2_role::client);
    handshake(client);
    const auto request = client.submit_connect_request_head("example.test:443");
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto stream_id = submitted_request_stream_id(request);
    client.consume_output(client.pending_output().size());

    std::pmr::string response(&resource);
    hpack_encoder::encode_header(response, ":status", "407");
    hpack_encoder::encode_header(response, "content-length", "3");
    const auto head =
        frame(&resource, http2_frame_type::headers, ruvia::detail::http2_flag_end_headers, stream_id,
            std::string_view(response.data(), response.size()));
    (void)client.feed(std::string_view(head.data(), head.size()));
    const auto request_fin = client.pending_output();
    const auto fin = ruvia::detail::http2_parse_frame_header(request_fin.substr(0, 9));
    RUVIA_CHECK_EQ(fin.type_, static_cast<std::uint8_t>(http2_frame_type::data));
    RUVIA_CHECK_EQ(fin.length_, std::uint32_t{0});
    RUVIA_CHECK((fin.flags_ & ruvia::detail::http2_flag_end_stream) != 0);
    client.consume_output(request_fin.size());
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!client.next_event().has_value());

    auto* stream = client.stream(stream_id);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->tunnel().rejected() != nullptr);
    RUVIA_CHECK(stream->local_send().end_stream_committed() != nullptr);
    RUVIA_CHECK(stream->local_send().connect_pending() == nullptr);
    RUVIA_CHECK(stream->local_send().tunnel_open() == nullptr);
    RUVIA_CHECK(client.submit_data(stream_id, "tunnel?", http2_end_stream::keep_open) ==
                http2_data_submit_status::invalid_state);

    const auto body = frame(
        &resource, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, stream_id, "bad");
    (void)client.feed(std::string_view(body.data(), body.size()));
    auto event = client.next_event().value();
    RUVIA_CHECK(event.kind() == http2_event_kind::message_body_chunk);
    RUVIA_CHECK_EQ(event.message_body_chunk()->bytes(), std::string_view("bad"));
    RUVIA_CHECK(client.next_event().value().kind() == http2_event_kind::message_end);
}

RUVIA_TEST(http2_connect_server_rejection_accepts_empty_terminal_data) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    feed_standard_connect(server, &resource);
    drain_events(server);

    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remote_receive().connect_pending() != nullptr);

    ruvia::http_response rejected({.resource_ = &resource});
    rejected.status(ruvia::http_status::forbidden);
    const auto submitted = server.submit_response_head(1, rejected,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, rejected));
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK(stream->remote_receive().connect_rejected_awaiting_end_stream() != nullptr);
    server.consume_output(server.pending_output().size());

    const auto empty_keep_open = frame(&resource, http2_frame_type::data, 0, 1);
    RUVIA_CHECK(server.feed(std::string_view(empty_keep_open.data(), empty_keep_open.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(stream->remote_receive().connect_rejected_awaiting_end_stream() != nullptr);

    // A client that receives the non-2xx response closes its still-open CONNECT
    // request half with an empty DATA(END_STREAM). This is normal completion, not a
    // second request body and not a STREAM_CLOSED error.
    const auto terminal =
        frame(&resource, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, 1);
    RUVIA_CHECK(server.feed(std::string_view(terminal.data(), terminal.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(!server.next_event().has_value());
    RUVIA_CHECK(stream->remote_receive().end_stream() != nullptr);
    RUVIA_CHECK(!server.connection_error().has_value());
}

RUVIA_TEST(http2_connect_pending_accepts_empty_request_half_close) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    feed_standard_connect(server, &resource);
    drain_events(server);

    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remote_receive().connect_pending() != nullptr);
    const auto terminal =
        frame(&resource, http2_frame_type::data, ruvia::detail::http2_flag_end_stream, 1);
    RUVIA_CHECK(server.feed(std::string_view(terminal.data(), terminal.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(!server.next_event().has_value());
    RUVIA_CHECK(stream->remote_receive().connect_pending_end_stream() != nullptr);

    ruvia::http_response accepted({.resource_ = &resource});
    accepted.status(ruvia::http_status::ok);
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == http2_submit_status::accepted);
    RUVIA_CHECK(stream->remote_receive().end_stream() != nullptr);
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::tunnel_end);
    RUVIA_CHECK(!server.next_event().has_value());
}

RUVIA_TEST(http2_connect_open_tunnel_batches_owner_released_window_credit) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    open_standard_tunnel(server, &resource);
    auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->remote_receive().tunnel_open() != nullptr);

    const auto data = frame(&resource, http2_frame_type::data, 0, 1, "peer");
    RUVIA_CHECK(server.feed(std::string_view(data.data(), data.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::tunnel_data);
    RUVIA_CHECK(!server.next_event().has_value());
    RUVIA_CHECK(server.pending_output().empty());

    server.release_all_received_data(1);
    // Owner acknowledgement transfers the four octets into both live receive
    // scopes, but neither emits a frame before the batching threshold.
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(!server.connection_error().has_value());
}

RUVIA_TEST(http2_connect_server_rejects_data_before_acceptance) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    feed_standard_connect(server, &resource);
    drain_events(server);

    const auto data = frame(&resource, http2_frame_type::data, 0, 1, "early");
    (void)server.feed(std::string_view(data.data(), data.size()));
    const auto out = server.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
    RUVIA_CHECK(server.stream(1) == nullptr);
    RUVIA_CHECK(!server.connection_error().has_value());
}

RUVIA_TEST(http2_connect_pending_stream_cannot_hide_invalid_data_padding) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);
    feed_standard_connect(server, &resource);
    drain_events(server);

    const char malformed_payload[] = {5, 'x'};
    const auto malformed = frame(&resource, http2_frame_type::data, ruvia::detail::http2_flag_padded,
        1, std::string_view(malformed_payload, sizeof(malformed_payload)));
    const auto result_value = server.feed(std::string_view(malformed.data(), malformed.size()));
    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error().has_value());
    const auto out = server.pending_output();
    const auto goaway = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type_, static_cast<std::uint8_t>(http2_frame_type::goaway));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 13)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connect_open_tunnel_rejects_headers_and_unknown_stream_frames) {
    std::pmr::monotonic_buffer_resource resource;
    {
        http2_connection server(&resource);
        handshake(server);
        open_standard_tunnel(server, &resource);
        std::pmr::string block(&resource);
        hpack_encoder::encode_header(block, "x-trailer", "forbidden");
        const auto headers = frame(&resource, http2_frame_type::headers,
            static_cast<std::uint8_t>(
                ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream),
            1, std::string_view(block.data(), block.size()));
        (void)server.feed(std::string_view(headers.data(), headers.size()));
        const auto out = server.pending_output();
        const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
        RUVIA_CHECK(!server.connection_error().has_value());
    }
    {
        http2_connection server(&resource);
        handshake(server);
        open_standard_tunnel(server, &resource);
        const auto unknown = frame(&resource, static_cast<http2_frame_type>(0x20), 0, 1);
        (void)server.feed(std::string_view(unknown.data(), unknown.size()));
        const auto out = server.pending_output();
        const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
        RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(http2_error_code::protocol_error));
        RUVIA_CHECK(!server.connection_error().has_value());
    }
}

RUVIA_TEST(http2_connect_server_rejects_extended_before_advertising_capability) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "connect-udp");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":authority", "example.test");
    hpack_encoder::encode_header(block, ":path", "/masque");
    const auto request = frame(&resource, http2_frame_type::headers,
        ruvia::detail::http2_flag_end_headers, 1, std::string_view(block.data(), block.size()));
    const auto result_value = server.feed(std::string_view(request.data(), request.size()));

    RUVIA_CHECK(result_value == ruvia::detail::http2_feed_result::connection_not_started);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(server.stream(1) == nullptr);
    RUVIA_CHECK(!server.connection_error().has_value());
}

RUVIA_TEST(http2_connect_server_retains_generic_extended_protocol) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "example-tunnel");
    hpack_encoder::encode_header(block, ":scheme", "custom+transport");
    hpack_encoder::encode_header(block, ":authority", "user:secret@example.test");
    hpack_encoder::encode_header(block, ":path", "");
    const auto request = frame(&resource, http2_frame_type::headers,
        ruvia::detail::http2_flag_end_headers, 1, std::string_view(block.data(), block.size()));
    (void)server.feed(std::string_view(request.data(), request.size()));
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::message_head);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto* stream = server.stream(1);
    RUVIA_CHECK(stream != nullptr);
    const auto* pending = stream->tunnel().pending();
    RUVIA_CHECK(pending != nullptr);
    RUVIA_CHECK(pending->form() == http2_connect_form::extended);
    RUVIA_CHECK(!http2_is_pending_websocket_connect(*stream));
    RUVIA_CHECK_EQ(stream->request_protocol(), std::string_view("example-tunnel"));
    RUVIA_CHECK_EQ(stream->request_scheme(), std::string_view("custom+transport"));
    RUVIA_CHECK_EQ(stream->scheme_default_port(), std::uint16_t{0});
    RUVIA_CHECK_EQ(stream->request_authority(), std::string_view("user:secret@example.test"));
    RUVIA_CHECK(stream->has_path());
    RUVIA_CHECK(stream->request_path().empty());
}

RUVIA_TEST(http2_connect_server_rejects_non_http_websocket_scheme) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "websocket");
    hpack_encoder::encode_header(block, ":scheme", "gemini");
    hpack_encoder::encode_header(block, ":authority", "example.test");
    hpack_encoder::encode_header(block, ":path", "/ws");
    hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    const auto request = frame(&resource, http2_frame_type::headers,
        ruvia::detail::http2_flag_end_headers, 1, std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto out = server.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_connect_server_rejects_asterisk_path) {
    std::pmr::monotonic_buffer_resource resource;
    http2_connection server(&resource);
    handshake(server);

    std::pmr::string block(&resource);
    hpack_encoder::encode_header(block, ":method", "CONNECT");
    hpack_encoder::encode_header(block, ":protocol", "example-tunnel");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":authority", "example.test");
    hpack_encoder::encode_header(block, ":path", "*");
    const auto request = frame(&resource, http2_frame_type::headers,
        ruvia::detail::http2_flag_end_headers, 1, std::string_view(block.data(), block.size()));

    RUVIA_CHECK(server.feed(std::string_view(request.data(), request.size())) ==
                ruvia::detail::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(server.next_event().value().kind() == http2_event_kind::stream_closed);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto out = server.pending_output();
    const auto reset = ruvia::detail::http2_parse_frame_header(out.substr(0, 9));
    RUVIA_CHECK_EQ(reset.type_, static_cast<std::uint8_t>(http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(
        ruvia::detail::http2_read32(reinterpret_cast<const unsigned char*>(out.data() + 9)),
        static_cast<std::uint32_t>(http2_error_code::protocol_error));
}
