#include <variant>

#include "http3_buffered_dispatch_fixture.h"

namespace {

std::string client_text_frame(std::string_view payload_value) {
    if (payload_value.size() > 125) {
        throw std::runtime_error("HTTP/3 WebSocket fixture payload is too large");
    }
    constexpr std::array<char, 4> mask{char{0x11}, char{0x22}, char{0x33}, char{0x44}};
    std::string wire;
    wire.push_back(static_cast<char>(0x81));
    wire.push_back(static_cast<char>(0x80U | payload_value.size()));
    wire.append(mask.data(), mask.size());
    for (std::size_t index = 0; index < payload_value.size(); ++index) {
        wire.push_back(static_cast<char>(payload_value[index] ^ mask[index % mask.size()]));
    }
    return wire;
}

void feed_tunnel_data(fixture& fixture_value, dispatch_type& dispatch, std::uint64_t stream_id,
    std::string_view payload_value) {
    const auto input = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data),
        client_text_frame(payload_value));
    const auto result_value = fixture_value.session_.feed(stream_id, input);
    if (result_value.scope_ != ruvia::http3_connection_error_scope::none) {
        throw std::runtime_error("HTTP/3 WebSocket DATA was rejected");
    }
    dispatch.notify_tunnel_input();
}

void feed_raw_tunnel_data(engine_type& session_value, std::uint64_t stream_id, std::string_view payload_value) {
    const auto input = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), payload_value);
    const auto result_value = session_value.feed(stream_id, input);
    if (result_value.scope_ != ruvia::http3_connection_error_scope::none) {
        throw std::runtime_error("HTTP/3 WebSocket tunnel payload was rejected");
    }
}

void feed_raw_tunnel_data(fixture& fixture_value, std::uint64_t stream_id, std::string_view payload_value) {
    feed_raw_tunnel_data(fixture_value.session_, stream_id, payload_value);
}

void feed_tunnel_fin(fixture& fixture_value, dispatch_type& dispatch, std::uint64_t stream_id) {
    const auto result_value = fixture_value.session_.feed(stream_id, {}, true);
    if (result_value.scope_ != ruvia::http3_connection_error_scope::none) {
        throw std::runtime_error("HTTP/3 WebSocket FIN was rejected");
    }
    dispatch.notify_tunnel_input();
}

struct websocket_status_capture final {
    std::string status_;
};

bool capture_websocket_status(void* raw, ruvia::http3_field_section_field_view field) {
    if (field.name_ == ":status") {
        static_cast<websocket_status_capture*>(raw)->status_.assign(field.value_);
    }
    return true;
}

void check_websocket_published_wire(const published_wire& wire, fixture& fixture_value,
    ruvia::testing::test_context& ruvia_ctx) {
    RUVIA_CHECK(wire.final_wire_bytes_.has_value());
    RUVIA_CHECK_EQ(wire.final_wire_bytes_.value_or(0), wire.bytes_.size());
    const auto headers = ruvia::decode_http3_frame(
        std::span<const char>(wire.bytes_.data(), wire.bytes_.size()));
    RUVIA_CHECK((headers.index() == 0));
    if ((headers.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(headers).type_, static_cast<std::uint64_t>(ruvia::http3_frame_type::headers));
    websocket_status_capture status;
    const auto decoded = ruvia::decode_http3_field_section(std::get<0>(headers).payload_,
        &capture_websocket_status, &status, {}, fixture_value.worker_.resource());
    RUVIA_CHECK((decoded.index() == 0));
    RUVIA_CHECK(status.status_ == "200");

    std::string websocket_bytes;
    std::size_t offset = std::get<0>(headers).encoded_bytes_;
    while (offset < wire.bytes_.size()) {
        const auto data = ruvia::decode_http3_frame(
            std::span<const char>(wire.bytes_.data() + offset, wire.bytes_.size() - offset));
        RUVIA_CHECK((data.index() == 0));
        if ((data.index() != 0)) {
            return;
        }
        RUVIA_CHECK_EQ(std::get<0>(data).type_, static_cast<std::uint64_t>(ruvia::http3_frame_type::data));
        websocket_bytes.append(std::get<0>(data).payload_.data(), std::get<0>(data).payload_.size());
        offset += std::get<0>(data).encoded_bytes_;
    }
    RUVIA_CHECK_EQ(offset, wire.bytes_.size());
    std::size_t websocket_offset = 0;
    for (const auto expected : {std::string_view("first"), std::string_view("second")}) {
        RUVIA_CHECK(websocket_bytes.size() - websocket_offset >= 2);
        if (websocket_bytes.size() - websocket_offset < 2) {
            return;
        }
        const auto* frame_bytes = websocket_bytes.data() + websocket_offset;
        RUVIA_CHECK(static_cast<unsigned char>(frame_bytes[0]) == 0x81U);
        RUVIA_CHECK(static_cast<unsigned char>(frame_bytes[1]) == expected.size());
        RUVIA_CHECK(websocket_bytes.size() - websocket_offset >= expected.size() + 2);
        if (websocket_bytes.size() - websocket_offset < expected.size() + 2) {
            return;
        }
        RUVIA_CHECK(std::string_view(frame_bytes + 2, expected.size()) == expected);
        websocket_offset += expected.size() + 2;
    }
}

ruvia::task<bool> drive_websocket_handshake(fixture& fixture_value, dispatch_type& dispatch,
    const ruvia::worker_handle& worker_value, message_id_type id, published_wire& wire,
    ruvia::testing::test_context& ruvia_ctx) {
    ruvia::http3_client_response peer(id.stream_id_, ruvia::http_known_method::connect,
        fixture_value.worker_.resource());
    decoded_response response;
    std::optional<control_type> establishment;
    std::size_t decoded_bytes{};
    const auto deadline_value = std::chrono::steady_clock::now() + 3s;
    do {
        (void)dispatch.publish_step();
        control_type control;
        while (fixture_value.outbound_.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::tunnel_established) {
                establishment = control;
                RUVIA_CHECK(control.id_.epoch_ == id.epoch_);
                RUVIA_CHECK(control.id_.connection_generation_ == id.connection_generation_);
                RUVIA_CHECK(control.id_.stream_id_ == id.stream_id_);
            }
        }
        (void)drain_data_only(fixture_value.outbound_, id, wire);
        (void)peer.feed(std::span<const char>(wire.bytes_).subspan(decoded_bytes),
            false, false, &on_response, &response);
        decoded_bytes = wire.bytes_.size();
        co_await ruvia::sleep_for(worker_value, 1ms);
    } while ((!establishment || response.final_heads_ == 0) &&
             std::chrono::steady_clock::now() < deadline_value);
    RUVIA_CHECK_EQ(response.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(response.status_, std::uint16_t{200});
    RUVIA_CHECK(establishment &&
                establishment->value_ == static_cast<std::uint64_t>(wire.bytes_.size()));
    co_return establishment&& response.final_heads_ == 1 && response.status_ == 200;
}

ruvia::task<void> exercise_websocket_data_fin_backpressure(fixture& fixture_value,
    const ruvia::worker_handle& worker_handle_value, std::uint64_t stream_id,
    ruvia::testing::test_context& ruvia_ctx) {
    auto& handler_state_value = fixture_value.routes_.handlers_;
    handler_state_value.websocket_messages_ = {};
    handler_state_value.websocket_echoes_ = 0;
    handler_state_value.websocket_saw_fin_ = false;
    handler_state_value.websocket_retained_data_stable_ = true;
    ruvia::worker_signal handler_started(worker_handle_value);
    ruvia::worker_signal message_received(worker_handle_value);
    ruvia::worker_signal message_echoed(worker_handle_value);
    handler_state_value.websocket_started_ = &handler_started;
    handler_state_value.websocket_message_received_ = &message_received;
    handler_state_value.websocket_message_echoed_ = &message_echoed;

    tunnel_callbacks_state callbacks(worker_handle_value, fixture_value.scanner_);
    feed_websocket_request(fixture_value, stream_id);
    auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_, callbacks.callbacks());
    ruvia::worker_signal finished(worker_handle_value);
    dispatch_type::run_status_type run_status{dispatch_type::run_status_type::failed};
    bool joined = false;
    ruvia::task_scope tasks(worker_handle_value, {.resource_ = fixture_value.worker_.resource()});
    tasks.spawn(run_owner(dispatch, run_status, joined, finished));

    published_wire wire;
    const message_id_type id{epoch, generation, stream_id};
    if (!(co_await drive_websocket_handshake(fixture_value, dispatch, worker_handle_value, id, wire, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handler_started.wait();
    RUVIA_CHECK(callbacks.scanner_attached_);

    const message_id_type filler_id{epoch, generation, stream_id + 1000};
    constexpr std::array<std::byte, 1> filler_bytes{std::byte{0x7f}};
    RUVIA_CHECK(send_accepted(fixture_value.outbound_.try_send(filler_id, filler_bytes)));
    feed_tunnel_data(fixture_value, dispatch, stream_id, "first");
    co_await message_received.wait();
    co_await callbacks.output_ready_.wait();
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    const auto blocked = dispatch.publish_step();
    RUVIA_CHECK(blocked.status_ == dispatch_type::publish_status_type::backpressured);
    RUVIA_CHECK(blocked.block_reason_ == block_reason_type::data);
    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), wire.bytes_.size());

    buffer::borrowed_block filler;
    RUVIA_CHECK(fixture_value.outbound_.try_receive(filler));
    RUVIA_CHECK(filler.id().stream_id_ == filler_id.stream_id_);
    RUVIA_CHECK(filler.bytes().size() == filler_bytes.size());
    filler.release();
    const auto first_echo = dispatch.publish_step();
    RUVIA_CHECK(first_echo.status_ == dispatch_type::publish_status_type::bytes_published);
    drain_buffer(fixture_value.outbound_, id, wire);
    co_await message_echoed.wait();

    feed_tunnel_data(fixture_value, dispatch, stream_id, "second");
    co_await message_received.wait();
    co_await callbacks.output_ready_.wait();
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::data);
    const auto second_echo = dispatch.publish_step();
    RUVIA_CHECK(second_echo.status_ == dispatch_type::publish_status_type::bytes_published);
    drain_buffer(fixture_value.outbound_, id, wire);
    co_await message_echoed.wait();

    feed_tunnel_fin(fixture_value, dispatch, stream_id);
    co_await callbacks.output_ready_.wait();
    RUVIA_CHECK(dispatch.publication_demand() == publication_demand_type::control);
    const auto fin = dispatch.publish_step();
    RUVIA_CHECK(fin.status_ == dispatch_type::publish_status_type::fin_published);
    drain_buffer(fixture_value.outbound_, id, wire);
    co_await finished.wait();
    co_await tasks.join();
    if (callbacks.scanner_attached_) {
        fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
    }

    RUVIA_CHECK(joined);
    RUVIA_CHECK(run_status == dispatch_type::run_status_type::tunnel_complete);
    RUVIA_CHECK(dispatch.complete());
    RUVIA_CHECK_EQ(handler_state_value.websocket_echoes_, std::size_t{2});
    RUVIA_CHECK(handler_state_value.websocket_messages_[0] == "first");
    RUVIA_CHECK(handler_state_value.websocket_messages_[1] == "second");
    RUVIA_CHECK(handler_state_value.websocket_saw_fin_);
    RUVIA_CHECK(handler_state_value.websocket_retained_data_stable_);
    RUVIA_CHECK(!callbacks.aborted_);
    RUVIA_CHECK(fixture_value.session_.request(stream_id) == nullptr);
    RUVIA_CHECK_EQ(fixture_value.session_.active_stream_count(), std::size_t{0});
    check_websocket_published_wire(wire, fixture_value, ruvia_ctx);
    handler_state_value.websocket_started_ = nullptr;
    handler_state_value.websocket_message_received_ = nullptr;
    handler_state_value.websocket_message_echoed_ = nullptr;
}

ruvia::task<void> exercise_websocket_tunnel_reuses_operation_storage(fixture& fixture_value,
    const ruvia::worker_handle& worker_handle_value, ruvia::testing::test_context& ruvia_ctx,
    std::size_t& warmed_live_allocations) {
    co_await exercise_websocket_data_fin_backpressure(fixture_value, worker_handle_value, 0, ruvia_ctx);
    warmed_live_allocations = fixture_value.upstream_.live_allocations();
    co_await exercise_websocket_data_fin_backpressure(fixture_value, worker_handle_value, 4, ruvia_ctx);
    RUVIA_CHECK_EQ(fixture_value.upstream_.live_allocations(), warmed_live_allocations);
}

enum class websocket_termination : std::uint8_t { reset,
    cancel,
    deadline };

ruvia::task<void> exercise_websocket_termination(fixture& fixture_value,
    const ruvia::worker_handle& worker_handle_value, websocket_termination termination,
    ruvia::testing::test_context& ruvia_ctx) {
    if (termination == websocket_termination::deadline) {
        fixture_value.options_.deadline_ = ruvia::deadline_config{.handler_ = 25ms};
    }
    ruvia::worker_signal handler_started(worker_handle_value);
    ruvia::worker_signal message_received(worker_handle_value);
    ruvia::worker_signal message_echoed(worker_handle_value);
    auto& handler_state_value = fixture_value.routes_.handlers_;
    handler_state_value.websocket_started_ = &handler_started;
    handler_state_value.websocket_message_received_ = &message_received;
    handler_state_value.websocket_message_echoed_ = &message_echoed;
    tunnel_callbacks_state callbacks(worker_handle_value, fixture_value.scanner_);
    feed_websocket_request(fixture_value, 0);
    auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_, callbacks.callbacks());
    ruvia::worker_signal finished(worker_handle_value);
    dispatch_type::run_status_type run_status{dispatch_type::run_status_type::failed};
    bool joined = false;
    ruvia::task_scope tasks(worker_handle_value, {.resource_ = fixture_value.worker_.resource()});
    tasks.spawn(run_owner(dispatch, run_status, joined, finished));

    published_wire handshake;
    if (!(co_await drive_websocket_handshake(fixture_value, dispatch, worker_handle_value,
            {epoch, generation, 0}, handshake, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handler_started.wait();

    if (termination == websocket_termination::reset) {
        const auto reset = fixture_value.session_.feed(0, {}, false, true);
        RUVIA_CHECK(reset.status_ == ruvia::http3_connection_status::need_more_data);
        RUVIA_CHECK(reset.scope_ == ruvia::http3_connection_error_scope::none);
        dispatch.notify_tunnel_input();
    } else if (termination == websocket_termination::cancel) {
        dispatch.cancel();
        (void)fixture_value.session_.cancel_request(0);
    } else {
        RUVIA_CHECK(co_await ruvia::sleep_for(worker_handle_value, 100ms, fixture_value.worker_stop_) ==
                    ruvia::timer_sleep_result::elapsed);
        (void)fixture_value.session_.cancel_request(0);
    }
    co_await finished.wait();
    co_await tasks.join();
    if (callbacks.scanner_attached_) {
        fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
    }

    RUVIA_CHECK(joined);
    RUVIA_CHECK(run_status == dispatch_type::run_status_type::cancelled);
    RUVIA_CHECK(!dispatch.handler_active());
    RUVIA_CHECK(callbacks.aborted_ != (termination == websocket_termination::cancel));
    RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
    RUVIA_CHECK_EQ(fixture_value.session_.active_stream_count(), std::size_t{0});
    RUVIA_CHECK(dispatch.cancellation_reason() ==
                (termination == websocket_termination::deadline
                        ? dispatch_type::cancellation_reason_type::deadline
                        : dispatch_type::cancellation_reason_type::explicit_value));
    handler_state_value.websocket_started_ = nullptr;
    handler_state_value.websocket_message_received_ = nullptr;
    handler_state_value.websocket_message_echoed_ = nullptr;
}

enum class peer_fin_wait_outcome : std::uint8_t { timeout,
    peer_fin,
    cancel };

ruvia::task<void> exercise_peer_transport_fin_wait(fixture& fixture_value,
    const ruvia::worker_handle& worker_handle_value, peer_fin_wait_outcome outcome, bool throw_handler,
    ruvia::testing::test_context& ruvia_ctx) {
    fixture_value.scanner_.start();
    auto& state_value = fixture_value.routes_.handlers_;
    state_value.websocket_throw_on_start_ = throw_handler;
    ruvia::worker_signal handler_started(worker_handle_value);
    state_value.websocket_started_ = &handler_started;
    tunnel_callbacks_state callbacks(worker_handle_value, fixture_value.scanner_);
    constexpr std::uint64_t stream_id = 0;
    feed_websocket_request(fixture_value, stream_id);
    auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_, callbacks.callbacks());
    ruvia::worker_signal finished(worker_handle_value);
    dispatch_type::run_status_type run_status{dispatch_type::run_status_type::failed};
    bool joined = false;
    ruvia::task_scope tasks(worker_handle_value, {.resource_ = fixture_value.worker_.resource()});
    tasks.spawn(run_owner(dispatch, run_status, joined, finished));

    published_wire handshake;
    if (!(co_await drive_websocket_handshake(fixture_value, dispatch, worker_handle_value,
            {epoch, generation, stream_id}, handshake, ruvia_ctx))) {
        dispatch.cancel();
        co_await tasks.join();
        co_return;
    }
    co_await handler_started.wait();

    // No peer websocket Close means no local QUIC FIN deadline yet, even while
    // the scanner continues to inspect the attached request entry.
    RUVIA_CHECK(co_await ruvia::sleep_for(worker_handle_value, 120ms, fixture_value.worker_stop_) ==
                ruvia::timer_sleep_result::elapsed);
    RUVIA_CHECK(!callbacks.aborted_);
    constexpr std::array<char, 6> peer_close{
        static_cast<char>(0x88), static_cast<char>(0x80), char{0x11}, char{0x22}, char{0x33}, char{0x44}};
    feed_raw_tunnel_data(fixture_value, stream_id, std::string_view(peer_close.data(), peer_close.size()));
    dispatch.notify_tunnel_input();

    published_wire close_frame_wire;
    bool received_fin = false;
    const auto publication_deadline = std::chrono::steady_clock::now() + 3s;
    while (!received_fin && std::chrono::steady_clock::now() < publication_deadline) {
        (void)dispatch.publish_step();
        control_type control;
        while (fixture_value.outbound_.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::stream_fin) {
                RUVIA_CHECK_EQ(control.value_, static_cast<std::uint64_t>(
                                                   handshake.bytes_.size() + close_frame_wire.bytes_.size()));
                received_fin = true;
            }
        }
        (void)drain_data_only(fixture_value.outbound_, {epoch, generation, stream_id}, close_frame_wire);
        if (!received_fin) {
            co_await ruvia::sleep_for(worker_handle_value, 1ms);
        }
    }
    RUVIA_CHECK(received_fin);
    if (!received_fin) {
        dispatch.cancel();
        (void)fixture_value.session_.cancel_request(stream_id);
        co_await tasks.join();
        fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
        co_return;
    }

    if (outcome == peer_fin_wait_outcome::peer_fin) {
        feed_tunnel_fin(fixture_value, dispatch, stream_id);
    } else if (outcome == peer_fin_wait_outcome::cancel) {
        fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
        dispatch.cancel();
        (void)fixture_value.session_.cancel_request(stream_id);
    } else {
        // Activity is deliberately refreshed repeatedly. It must not move the
        // absolute FIN deadline or turn it into an inactivity timeout.
        for (unsigned i = 0; i < 8 && !joined; ++i) {
            fixture_value.scanner_entry_.touch();
            RUVIA_CHECK(co_await ruvia::sleep_for(worker_handle_value, 5ms, fixture_value.worker_stop_) ==
                        ruvia::timer_sleep_result::elapsed);
        }
    }
    co_await finished.wait();
    co_await tasks.join();
    RUVIA_CHECK(joined);
    if (outcome == peer_fin_wait_outcome::timeout) {
        RUVIA_CHECK(callbacks.aborted_);
        RUVIA_CHECK(run_status == dispatch_type::run_status_type::cancelled);
        if (throw_handler) {
            bool saw_internal_error_close = false;
            std::size_t offset = 0;
            while (offset < close_frame_wire.bytes_.size()) {
                const auto data = ruvia::decode_http3_frame(std::span<const char>(
                    close_frame_wire.bytes_.data() + offset, close_frame_wire.bytes_.size() - offset));
                RUVIA_CHECK((data.index() == 0));
                if ((data.index() != 0)) {
                    break;
                }
                if (std::get<0>(data).type_ == static_cast<std::uint64_t>(ruvia::http3_frame_type::data) &&
                    std::get<0>(data).payload_.size() >= 4 &&
                    static_cast<unsigned char>(std::get<0>(data).payload_[0]) == 0x88U &&
                    static_cast<unsigned char>(std::get<0>(data).payload_[2]) == 0x03U &&
                    static_cast<unsigned char>(std::get<0>(data).payload_[3]) == 0xf3U) {
                    saw_internal_error_close = true;
                }
                offset += std::get<0>(data).encoded_bytes_;
            }
            RUVIA_CHECK(saw_internal_error_close);
        }
    } else {
        RUVIA_CHECK(!callbacks.aborted_);
        RUVIA_CHECK(run_status == dispatch_type::run_status_type::tunnel_complete ||
                    run_status == dispatch_type::run_status_type::cancelled);
    }
    if (outcome != peer_fin_wait_outcome::cancel) {
        fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
    }
    fixture_value.scanner_.stop();
    state_value.websocket_throw_on_start_ = false;
    state_value.websocket_started_ = nullptr;
}

ruvia::task<void> exercise_connect_tunnel(fixture& fixture_value, const ruvia::worker_handle& worker_value,
    ruvia::testing::test_context& ruvia_ctx, bool udp = false) {
    for (unsigned round = 0; round != 3; ++round) {
        const std::uint64_t id = round * 4;
        std::pmr::vector<ruvia::http3_field_section_field_view> fields_value(fixture_value.worker_.resource());
        fields_value.push_back({":method", "CONNECT"});
        fields_value.push_back({":authority", "backend.test:443"});
        if (udp || round != 0) {
            fields_value.push_back({":protocol", udp ? "connect-udp" : "test-tunnel"});
            fields_value.push_back({":scheme", "https"});
            fields_value.push_back({":path", udp ? "/udp/target.test" : "/tunnel/target.test"});
            if (udp) {
                fields_value.push_back({"capsule-protocol", "?1"});
            }
        }
        auto encoded = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
        if ((encoded.index() != 0)) {
            throw std::runtime_error("CONNECT fixture field encoding failed");
        }
        const auto request = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers), std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
        RUVIA_CHECK(fixture_value.session_.feed(id, request).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(fixture_value.session_.stream_state(id) == engine_type::stream_state_type::ready);
        tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
        auto dispatch = fixture_value.make_dispatch(id, fixture_value.services_, callbacks.callbacks());
        fixture_value.routes_.handlers_.tunnel_read_after_finish_ = round == 2;
        fixture_value.routes_.handlers_.tunnel_received_.clear();
        ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
        ruvia::worker_signal finished(worker_value);
        bool joined = false;
        auto status = dispatch_type::run_status_type::failed;
        tasks.spawn(run_owner(dispatch, status, joined, finished));
        published_wire wire;
        const message_id_type message_id{epoch, generation, id};
        bool sent = false;
        const std::string payload_value(62007, 'c');
        std::string tunnel_payload = payload_value;
        if (udp) {
            std::array<char, 16> header;
            const auto capsule_header_size = ruvia::encode_http_capsule_header(header, 0, payload_value.size() + 1);
            tunnel_payload.assign(header.data(), std::get<0>(capsule_header_size));
            tunnel_payload.push_back('\0');
            tunnel_payload.append(payload_value);
            tunnel_payload.append("\0\1\0", 3);
        }
        const auto deadline_value = std::chrono::steady_clock::now() + 3s;
        while (!joined && std::chrono::steady_clock::now() < deadline_value) {
            (void)dispatch.publish_step();
            drain_buffer(fixture_value.outbound_, message_id, wire);
            if (!sent && !wire.bytes_.empty() && (round != 2 || wire.final_wire_bytes_)) {
                const auto input = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), tunnel_payload);
                const auto fed = fixture_value.session_.feed(id, input, true);
                RUVIA_CHECK(fed.scope_ == ruvia::http3_connection_error_scope::none);
                dispatch.notify_tunnel_input();
                sent = true;
            }
            co_await ruvia::sleep_for(worker_value, 1ms);
        }
        if (!joined) {
            dispatch.cancel();
        }
        co_await tasks.join();
        if (callbacks.scanner_attached_) {
            fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
        }
        RUVIA_CHECK(joined && sent);
        RUVIA_CHECK(status == dispatch_type::run_status_type::tunnel_complete);
        RUVIA_CHECK(!callbacks.aborted_);
        RUVIA_CHECK(fixture_value.routes_.handlers_.tunnel_received_ == payload_value);
        RUVIA_CHECK(fixture_value.routes_.handlers_.tunnel_stable_);
        RUVIA_CHECK(wire.final_wire_bytes_ && *wire.final_wire_bytes_ == wire.bytes_.size());
        struct result_type {
            std::string body_;
            std::uint16_t status_{};
            bool ended_{};
            bool framing_field_{};
            bool capsule_{};
        } result;
        ruvia::http3_client_response response(id, ruvia::http_known_method::connect, fixture_value.worker_.resource());
        const auto callback_value = [](void* raw, const ruvia::http3_client_response_event& event) {
            auto& observed_value = *static_cast<result_type*>(raw);
            if (event.kind_ == ruvia::http3_client_response_event_kind::final_head) {
                observed_value.status_ = event.head_->status_;
                for (const auto& field : event.head_->headers_) {
                    observed_value.capsule_ = observed_value.capsule_ || (field.name_ == "capsule-protocol" && field.value_ == "?1");
                    observed_value.framing_field_ = observed_value.framing_field_ || field.name_ == "content-length" || field.name_ == "transfer-encoding";
                }
            }
            if (event.kind_ == ruvia::http3_client_response_event_kind::tunnel_data) {
                observed_value.body_.append(event.body_.data(), event.body_.size());
            }
            if (event.kind_ == ruvia::http3_client_response_event_kind::message_end) {
                observed_value.ended_ = true;
            }
        };
        const auto decoded = response.feed(std::span<const char>(wire.bytes_.data(), wire.bytes_.size()), true, false, callback_value, &result);
        RUVIA_CHECK(decoded.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(result.status_ == 200 && result.ended_ && !result.framing_field_);
        RUVIA_CHECK(!udp || result.capsule_);
        RUVIA_CHECK(result.body_ == (round == 2 ? std::string{} : tunnel_payload));
        RUVIA_CHECK(fixture_value.session_.active_stream_count() == 0);
    }
}

}  // namespace

RUVIA_TEST(http3_buffered_dispatch_bounds_tunnel_input_across_worker_and_releases_every_reservation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture_value(worker_handle_value, upstream, 1, 1, 1, 10, 16);
        feed_websocket_request(fixture_value, 0);
        feed_websocket_request(fixture_value, 4);
        feed_raw_tunnel_data(fixture_value, 0, "first");
        feed_raw_tunnel_data(fixture_value, 4, "other");
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{10});

        engine_type sibling_session(fixture_value.routes_.implementation_.route_table(), fixture_value.worker_,
            fixture_value.body_budget_, {.max_tunnel_buffered_bytes_ = 16});
        feed_websocket_request(sibling_session, fixture_value.worker_, 0);
        feed_raw_tunnel_data(sibling_session, 0, "x");
        RUVIA_CHECK(sibling_session.tunnel_input_overflowed(0));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{10});
        RUVIA_CHECK(sibling_session.cancel_request(0));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{10});

        std::array<char, 8> retained{};
        const auto other = fixture_value.session_.read_tunnel_data(4, retained);
        RUVIA_CHECK_EQ(other.bytes_, std::size_t{5});
        RUVIA_CHECK(std::string_view(retained.data(), other.bytes_) == "other");
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{5});
        RUVIA_CHECK(fixture_value.session_.cancel_request(0));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});
        RUVIA_CHECK(std::string_view(retained.data(), other.bytes_) == "other");
        RUVIA_CHECK(fixture_value.session_.cancel_request(4));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});

        feed_websocket_request(fixture_value, 12);
        feed_raw_tunnel_data(fixture_value, 12, "seventeen-bytes!!");
        RUVIA_CHECK(fixture_value.session_.tunnel_input_overflowed(12));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});
        RUVIA_CHECK(fixture_value.session_.cancel_request(12));
    }
    {
        // Exceed every implementation's worker pool size class so the upstream
        // rejection cannot be satisfied from a cached block on MSVC.
        constexpr std::size_t tunnel_limit = 512 * 1024;
        fixture fixture_value(worker_handle_value, upstream, 1, 1, 1, tunnel_limit, tunnel_limit);
        const std::string allocation_failure_payload(256 * 1024, 'a');
        feed_websocket_request(fixture_value, 16);
        fixture_value.allocations_.reject_ = true;
        feed_raw_tunnel_data(fixture_value, 16, allocation_failure_payload);
        fixture_value.allocations_.reject_ = false;
        RUVIA_CHECK(fixture_value.session_.tunnel_input_overflowed(16));
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});
        RUVIA_CHECK(fixture_value.session_.cancel_request(16));

        feed_websocket_request(fixture_value, 20);
        feed_raw_tunnel_data(fixture_value, 20, "reset");
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{5});
        const auto reset = fixture_value.session_.feed(20, {}, false, true);
        RUVIA_CHECK(reset.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK_EQ(fixture_value.body_budget_.used(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    (void)worker_handle_value;
}

RUVIA_TEST(http3_buffered_dispatch_websocket_tunnel_publishes_data_fin_and_bounds_pmr_storage) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    std::size_t warmed_live_allocations{};
    {
        fixture fixture(worker_handle_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment, exercise_websocket_tunnel_reuses_operation_storage(
                                        fixture, worker_handle_value, ruvia_ctx, warmed_live_allocations));
    }
    RUVIA_CHECK(warmed_live_allocations > 0);
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

void run_websocket_termination_case(websocket_termination termination,
    ruvia::testing::test_context& ruvia_ctx) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture(worker_handle_value, upstream);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment,
            exercise_websocket_termination(fixture, worker_handle_value, termination, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_websocket_tunnel_reset_cancel_and_deadline_wake_readers) {
    run_websocket_termination_case(websocket_termination::reset, ruvia_ctx);
    run_websocket_termination_case(websocket_termination::cancel, ruvia_ctx);
    run_websocket_termination_case(websocket_termination::deadline, ruvia_ctx);
}

void run_peer_transport_fin_wait_case(peer_fin_wait_outcome outcome, bool throw_handler,
    ruvia::testing::test_context& ruvia_ctx) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource upstream;
    {
        fixture fixture_value(worker_handle_value, upstream, 1, 1, 1, 64 * 1024 * 1024,
            64 * 1024, 80ms, 1ms);
        fixture_value.executor_ = attachment.loop().executor();
        run_worker_task(attachment,
            exercise_peer_transport_fin_wait(
                fixture_value, worker_handle_value, outcome, throw_handler, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(http3_buffered_dispatch_peer_transport_fin_deadline_is_publication_bound_and_cleans_up) {
    run_peer_transport_fin_wait_case(peer_fin_wait_outcome::timeout, false, ruvia_ctx);
    run_peer_transport_fin_wait_case(peer_fin_wait_outcome::timeout, true, ruvia_ctx);
    run_peer_transport_fin_wait_case(peer_fin_wait_outcome::peer_fin, false, ruvia_ctx);
    run_peer_transport_fin_wait_case(peer_fin_wait_outcome::cancel, false, ruvia_ctx);
}

RUVIA_TEST(http3_buffered_dispatch_connect_streams_own_bytes_and_read_after_published_send_fin) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto& worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    {
        fixture fixture(worker_value, resource);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment, exercise_connect_tunnel(fixture, worker_value, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_connect_success_admits_native_datagrams_before_peer_response_visibility) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto& worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    {
        fixture fixture_value(worker_value, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 5s, 1ms, 1200);
        fixture_value.executor_ = attachment.loop().executor();
        feed_peer_settings(fixture_value, std::nullopt, true);
        auto exercise = [&]() -> ruvia::task<void> {
            constexpr std::uint64_t stream_id = 0;
            const std::array fields_value{
                ruvia::http3_field_section_field_view{":method", "CONNECT"},
                ruvia::http3_field_section_field_view{":protocol", "connect-udp"},
                ruvia::http3_field_section_field_view{":scheme", "https"},
                ruvia::http3_field_section_field_view{":authority", "backend.test:443"},
                ruvia::http3_field_section_field_view{":path", "/udp/target.test"},
                ruvia::http3_field_section_field_view{"capsule-protocol", "?1"}};
            const auto encoded = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT publication fixture could not encode request");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture_value.session_.feed(stream_id, request).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(fixture_value.session_.stream_state(stream_id) == engine_type::stream_state_type::ready);
            const auto negotiated = fixture_value.session_.datagram_config(stream_id);
            RUVIA_CHECK(negotiated.local_h3_datagram_ && negotiated.peer_h3_datagram_ && negotiated.quic_datagram_);
            const message_id_type message_id{epoch, generation, stream_id};
            const control_type occupying_control{control_type::kind::stream_fin,
                {epoch, generation, 4}, 0};
            RUVIA_CHECK(control_accepted(fixture_value.outbound_.try_send_control(occupying_control)));
            tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
            auto dispatch = fixture_value.make_dispatch(stream_id, fixture_value.services_, callbacks.callbacks());
            ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
            ruvia::worker_signal finished(worker_value);
            bool joined = false;
            auto status = dispatch_type::run_status_type::failed;
            tasks.spawn(run_owner(dispatch, status, joined, finished));
            published_wire wire;
            decoded_response observed;
            ruvia::http3_client_response peer(stream_id, ruvia::http_known_method::connect, fixture_value.worker_.resource());
            std::array<char, 16> native{};
            const auto prefix = ruvia::encode_http3_datagram_prefix(native, stream_id);
            RUVIA_CHECK((prefix.index() == 0));
            native[std::get<0>(prefix)] = '\0';  // CONNECT-UDP context ID zero.
            native[std::get<0>(prefix) + 1] = 'c';
            const auto packet = std::span<const char>(native.data(), std::get<0>(prefix) + 2);
            std::optional<control_type> marker;
            const auto plan = [&]() {
                return ruvia::testing::plan_connect_datagram_for_peer(
                    {epoch, generation}, marker, wire.bytes_.size(), packet);
            };
            co_await callbacks.output_ready_.wait();
            (void)dispatch.publish_step();
            (void)drain_data_only(fixture_value.outbound_, message_id, wire);
            (void)peer.feed(wire.bytes_, false, false, &on_response, &observed);
            // Control admission is held: a peer must not already see success.
            // If it does, its legal first native datagram must already be admissible.
            if (observed.final_heads_ != 0) {
                RUVIA_CHECK_EQ(observed.status_, std::uint16_t{200});
                RUVIA_CHECK_EQ(static_cast<unsigned>(plan()),
                    static_cast<unsigned>(ruvia::http3_datagram_receive_status::deliver));
            }
            RUVIA_CHECK_EQ(observed.final_heads_, std::size_t{0});
            control_type control;
            RUVIA_CHECK(fixture_value.outbound_.try_receive_control(control));
            std::size_t decoded_bytes = wire.bytes_.size();
            for (unsigned turn = 0; turn != 8 && observed.final_heads_ == 0; ++turn) {
                (void)dispatch.publish_step();
                while (fixture_value.outbound_.try_receive_control(control)) {
                    if (control.kind_ == control_type::kind::tunnel_established) {
                        marker = control;
                    }
                }
                (void)drain_data_only(fixture_value.outbound_, message_id, wire);
                (void)peer.feed(std::span<const char>(wire.bytes_).subspan(decoded_bytes),
                    false, false, &on_response, &observed);
                decoded_bytes = wire.bytes_.size();
                co_await ruvia::sleep_for(worker_value, 1ms);
            }
            RUVIA_CHECK_EQ(observed.final_heads_, std::size_t{1});
            RUVIA_CHECK_EQ(observed.status_, std::uint16_t{200});
            RUVIA_CHECK_EQ(static_cast<unsigned>(plan()),
                static_cast<unsigned>(ruvia::http3_datagram_receive_status::deliver));
            RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.udp_tunnel_starts_, std::size_t{1});
            dispatch.cancel();
            (void)fixture_value.session_.cancel_request(stream_id);
            co_await tasks.join();
            if (callbacks.scanner_attached_) {
                fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
            }
            RUVIA_CHECK(joined && status == dispatch_type::run_status_type::cancelled);
            RUVIA_CHECK(!dispatch.handler_active());
        };
        run_worker_task(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_connect_cancellation_before_head_visibility_retires_admitted_tunnel_barrier) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto& worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    {
        fixture fixture_value(worker_value, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 5s, 1ms, 1200);
        fixture_value.executor_ = attachment.loop().executor();
        feed_peer_settings(fixture_value, std::nullopt, true);
        auto exercise = [&]() -> ruvia::task<void> {
            const std::array fields_value{
                ruvia::http3_field_section_field_view{":method", "CONNECT"},
                ruvia::http3_field_section_field_view{":protocol", "connect-udp"},
                ruvia::http3_field_section_field_view{":scheme", "https"},
                ruvia::http3_field_section_field_view{":authority", "backend.test:443"},
                ruvia::http3_field_section_field_view{":path", "/udp/target.test"},
                ruvia::http3_field_section_field_view{"capsule-protocol", "?1"}};
            const auto encoded = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT cancellation fixture could not encode request");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture_value.session_.feed(0, request).scope_ == ruvia::http3_connection_error_scope::none);
            const auto negotiated = fixture_value.session_.datagram_config(0);
            RUVIA_CHECK(negotiated.local_h3_datagram_ && negotiated.peer_h3_datagram_ && negotiated.quic_datagram_);
            const std::array<std::byte, 1> filler{std::byte{0x7f}};
            RUVIA_CHECK(send_accepted(fixture_value.outbound_.try_send(
                {epoch, generation, 4}, filler)));
            tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
            auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_, callbacks.callbacks());
            ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
            ruvia::worker_signal finished(worker_value);
            bool joined = false;
            auto status = dispatch_type::run_status_type::failed;
            tasks.spawn(run_owner(dispatch, status, joined, finished));
            std::optional<control_type> marker;
            for (unsigned turn = 0; turn != 8 && !marker; ++turn) {
                (void)dispatch.publish_step();
                control_type control;
                while (fixture_value.outbound_.try_receive_control(control)) {
                    if (control.kind_ == control_type::kind::tunnel_established) {
                        marker = control;
                    }
                }
                co_await ruvia::sleep_for(worker_value, 1ms);
            }
            RUVIA_CHECK(marker && marker->value_ != 0);
            buffer::borrowed_block held_credit;
            RUVIA_CHECK(fixture_value.outbound_.try_receive(held_credit));
            RUVIA_CHECK_EQ(held_credit.id().stream_id_, std::uint64_t{4});
            (void)dispatch.publish_step();  // HEAD still cannot borrow the held credit.
            co_await ruvia::sleep_for(worker_value, 1ms);
            decoded_response observed;
            ruvia::http3_client_response peer(0, ruvia::http_known_method::connect, fixture_value.worker_.resource());
            (void)peer.feed({}, false, false, &on_response, &observed);
            RUVIA_CHECK_EQ(observed.final_heads_, std::size_t{0});
            RUVIA_CHECK_EQ(fixture_value.routes_.handlers_.udp_tunnel_starts_, std::size_t{0});
            constexpr std::array<char, 3> native{'\0', '\0', 'c'};
            RUVIA_CHECK_EQ(static_cast<unsigned>(ruvia::testing::plan_connect_datagram_for_peer(
                               {epoch, generation}, marker, 0, native)),
                static_cast<unsigned>(ruvia::http3_datagram_receive_status::stream_error));
            dispatch.cancel();
            (void)fixture_value.session_.cancel_request(0);
            co_await tasks.join();
            if (callbacks.scanner_attached_) {
                fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
            }
            RUVIA_CHECK(joined && status == dispatch_type::run_status_type::cancelled);
            RUVIA_CHECK(!dispatch.handler_active());
            RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
            RUVIA_CHECK_EQ(held_credit.bytes().size(), std::size_t{1});
            RUVIA_CHECK(held_credit.bytes()[0] == std::byte{0x7f});
            held_credit.release();
            RUVIA_CHECK(fixture_value.outbound_.quiescent());
        };
        run_worker_task(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_buffered_dispatch_connect_drain_timeout_retires_only_its_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto& worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    {
        fixture fixture_value(worker_value, resource, 1, 1, 1, 64 * 1024 * 1024, 64 * 1024, 20ms, 1ms);
        fixture_value.executor_ = attachment.loop().executor();
        fixture_value.routes_.handlers_.tunnel_return_early_ = true;
        auto exercise = [&]() -> ruvia::task<void> {
            fixture_value.scanner_.start();
            const std::array fields_value{
                ruvia::http3_field_section_field_view{":method", "CONNECT"},
                ruvia::http3_field_section_field_view{":authority", "backend.test:443"}};
            const auto encoded = ruvia::encode_http3_field_section(fields_value, fixture_value.worker_.resource());
            if ((encoded.index() != 0)) {
                throw std::runtime_error("CONNECT timeout field encoding failed");
            }
            const auto request = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
                std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
            RUVIA_CHECK(fixture_value.session_.feed(0, request).scope_ == ruvia::http3_connection_error_scope::none);
            tunnel_callbacks_state callbacks(worker_value, fixture_value.scanner_);
            auto dispatch = fixture_value.make_dispatch(0, fixture_value.services_, callbacks.callbacks());
            ruvia::task_scope tasks(worker_value, {.resource_ = fixture_value.worker_.resource()});
            ruvia::worker_signal finished(worker_value);
            bool joined = false;
            auto status = dispatch_type::run_status_type::failed;
            tasks.spawn(run_owner(dispatch, status, joined, finished));
            published_wire wire;
            const auto deadline_value = std::chrono::steady_clock::now() + 2s;
            while (!joined && std::chrono::steady_clock::now() < deadline_value) {
                (void)dispatch.publish_step();
                drain_buffer(fixture_value.outbound_, {epoch, generation, 0}, wire);
                co_await ruvia::sleep_for(worker_value, 1ms);
            }
            if (!joined) {
                dispatch.cancel();
            }
            co_await tasks.join();
            if (callbacks.scanner_attached_) {
                fixture_value.scanner_.unregister_entry(fixture_value.scanner_entry_);
            }
            fixture_value.scanner_.stop();
            RUVIA_CHECK(joined && status == dispatch_type::run_status_type::cancelled && callbacks.aborted_);
            RUVIA_CHECK(wire.final_wire_bytes_.has_value());
            RUVIA_CHECK(fixture_value.session_.request(0) == nullptr);
            feed_request(fixture_value, 4, "GET", "/large");
            RUVIA_CHECK(fixture_value.session_.stream_state(4) == engine_type::stream_state_type::ready);
            RUVIA_CHECK(fixture_value.session_.release(4));
        };
        run_worker_task(attachment, exercise());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_buffered_dispatch_udp_tunnel_negotiates_capsules_and_preserves_datagram_boundaries_and_half_close) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    const auto& worker_value = attachment.loop().handle();
    ruvia::test::counting_memory_resource resource;
    {
        fixture fixture(worker_value, resource);
        fixture.executor_ = attachment.loop().executor();
        run_worker_task(attachment, exercise_connect_tunnel(fixture, worker_value, ruvia_ctx, true));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
