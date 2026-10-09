#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_client_response.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback_ref.h"

#include "http3/http3_connection_driver.h"
#include "http3/http3_connection_state.h"
#include "http3/http3_server_connection.h"
#include "integration/worker_capabilities.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "server/http_server_options.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::detail {

struct http3_connection_driver_test_access final {
    using driver = http3_connection_driver;
    using stream = driver::stream_state;
    using connection = driver;
    using tunnel_result = driver::tunnel_established_result;

    static connection make_connection(std::pmr::memory_resource* resource,
        http3_connection_identity identity) {
        connection value(resource);
        value.identity_ = identity;
        value.streams_.reserve(8);
        return value;
    }

    static stream& add_request_stream(connection& value, std::uint64_t id) {
        value.streams_.emplace_back(value.streams_.get_allocator().resource());
        auto& result_value = value.streams_.back();
        result_value.id_ = id;
        result_value.request_stream_ = true;
        result_value.input_phase_ = stream::receive_phase::body;
        return result_value;
    }

    static tunnel_result accept_tunnel_established(connection& value,
        const http3_stream_control& control, std::uint64_t accepted_wire_bytes) noexcept {
        return value.accept_tunnel_established(control, accepted_wire_bytes);
    }

    static bool confirm_tunnel_established(connection& value,
        std::uint64_t stream_id, std::uint64_t accepted_wire_bytes) noexcept {
        return value.confirm_tunnel_established(stream_id, accepted_wire_bytes);
    }

    static void note_peer_fin(connection& value, std::uint64_t stream_id) noexcept {
        value.note_peer_fin(stream_id);
    }

    static void note_input_reset(connection& value, std::uint64_t stream_id) noexcept {
        value.note_input_reset(stream_id);
    }

    static void complete_input_terminal(connection& value, std::uint64_t stream_id) noexcept {
        value.complete_input_terminal(stream_id);
    }

    static void install_frame_tracker(stream& value, std::pmr::memory_resource* resource) {
        value.frame_tracker_ = make_pmr_object<http3_stream_frames>(resource,
            http3_stream_kind::request, resource);
    }

    static std::size_t pending_handshakes(const connection& value) noexcept {
        return value.pending_tunnel_handshakes_;
    }

    static bool closing(const connection& value) noexcept {
        return value.close_started_;
    }

    static http3_connection_identity identity(const connection& value) noexcept {
        return value.identity_;
    }

    static http3_datagram_receive_status plan_received_datagram(connection& value,
        const http3_datagram_view& datagram) noexcept {
        value.config_.local_settings_.h3_datagram_ = true;
        return value.plan_datagram_receive(datagram);
    }
};

struct http3_server_connection_reset_intent_test_access final {
    using owner = http3_server_connection;

    static bool enqueue_local(owner& owner_value, std::uint64_t stream_id) noexcept {
        return enqueue(owner_value, stream_id, http3_connection_error_code::request_cancelled,
            owner::reset_intent_origin_type::local_cancellation);
    }

    static bool enqueue_protocol(owner& owner_value, std::uint64_t stream_id,
        http3_connection_error_code error_code) noexcept {
        return enqueue(owner_value, stream_id, error_code, owner::reset_intent_origin_type::stream_protocol_error);
    }

private:
    static bool enqueue(owner& owner_value, std::uint64_t stream_id,
        http3_connection_error_code error_code, owner::reset_intent_origin_type origin) noexcept {
        bool created = false;
        auto* slot = owner_value.requests_.find_or_create(stream_id, created);
        if (slot == nullptr || !owner_value.enqueue_reset_intent(*slot, error_code, origin)) {
            return false;
        }
        owner_value.notify_activation();
        return true;
    }
};

}  // namespace ruvia::detail

namespace {

using connection_type = ruvia::detail::http3_server_connection;
using control_type = ruvia::detail::http3_stream_control;
using buffer = ruvia::detail::http3_stream_buffer;
using message_id_type = ruvia::detail::http3_stream_id;
using namespace std::chrono_literals;

struct test_activation_signal final {
    explicit test_activation_signal(const ruvia::worker_handle& worker_value)
        : signal_(worker_value) {}

    [[nodiscard]] connection_type::activation_ref_type activation_ref() noexcept {
        return {.context_ = this,
            .activate_ = [](void* context_value, std::uint64_t, std::uint64_t, std::uint64_t,
                             const connection_type::worker_activation_type&) noexcept {
                static_cast<test_activation_signal*>(context_value)->signal_.notify();
            },
            .slot_generation_ = 1};
    }

    operator connection_type::activation_ref_type() noexcept {
        return activation_ref();
    }

    void notify() noexcept {
        signal_.notify();
    }

    [[nodiscard]] ruvia::task<void> wait() {
        return signal_.wait();
    }

    ruvia::worker_signal signal_;
};

constexpr connection_type::work_lanes_type all_work_lanes{
    .data_ = true, .control_ = true, .local_ = true};

constexpr std::uint64_t epoch = 47;
constexpr std::uint64_t base_generation = 71;

struct handler_state final {
    ruvia::worker_signal* slow_started_{};
    ruvia::worker_signal* held_started_{};
    ruvia::worker_signal* held_release_{};
    bool held_handler_finished_{};
    bool slow_observed_stop_{};
    bool slow_started_observed_{};
    std::string body_seen_;
    std::string large_response_header_;
    ruvia::worker_signal* websocket_started_{};
    bool websocket_started_observed_{};
    bool websocket_handler_finished_{};
    std::size_t handler_calls_{};
    std::atomic<std::size_t>* runtime_handler_calls_{};
    int push_mode_{};
    bool push_completed_{};
    bool push_accepted_{};
    std::string pushed_cookie_;
    std::string pushed_header_;
    ruvia::http_priority pushed_priority_{};
};

inline ruvia::task<void> websocket_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    state_value.websocket_started_observed_ = true;
    state_value.websocket_started_->notify();
    co_await context_value.get_websocket().close();
    state_value.websocket_handler_finished_ = true;
}

inline ruvia::task<ruvia::http_response> request_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    ++state_value.handler_calls_;
    if (state_value.runtime_handler_calls_ != nullptr) {
        state_value.runtime_handler_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    const auto path = context_value.req().path();
    if (path == "/push") {
        std::string target = state_value.push_mode_ == 9 ? "/slow" : "/first";
        std::string authority = state_value.push_mode_ == 6 ? "other.test" : "example.test";
        std::array headers{ruvia::http_header_view("cookie", "session=pushed"), ruvia::http_header_view("x-pushed", "owned-header")};
        auto operation = context_value.push({.method_ = state_value.push_mode_ == 8 ? "HEAD" : "GET", .authority_ = authority, .path_ = target, .headers_ = headers});
        target.assign("changed");
        authority.assign("changed");
        if (state_value.push_mode_ != 5) {
            state_value.push_accepted_ = co_await std::move(operation);
        }
        state_value.push_completed_ = true;
        co_return context_value.text("parent");
    }
    if (path == "/first") {
        state_value.pushed_cookie_ = context_value.req().cookie("session").value_or("");
        state_value.pushed_header_ = context_value.req().header("x-pushed").value_or("");
    }

    if (path == "/advertise") {
        std::array<std::string, 40> storage;
        std::array<std::string_view, 40> origins;
        for (std::size_t index = 0; index != storage.size(); ++index) {
            storage[index] = "https://" + std::string(60, 'a') + "." + std::string(60, 'b') + std::to_string(index) + ".example.test";
            origins[index] = storage[index];
        }
        co_await context_value.advertise_origins(origins);
    }
    if (path == "/throw") {
        throw std::runtime_error("buffered HTTP/3 route failure");
    }
    if (path == "/file") {
        ruvia::http_response response({.resource_ = context_value.arena()});
        response.file_body("virtual-response.bin", 5, 0, 5, ruvia::http_response_file_identity::checked({}));
        co_return response;
    }
    if (path == "/held") {
        state_value.held_started_->notify();
        co_await state_value.held_release_->wait();
        state_value.held_handler_finished_ = true;
        co_return context_value.text("released");
    }
    if (path == "/slow" || path == "/slow-body") {
        if (path == "/slow-body") {
            const auto body = co_await context_value.req().text();
            state_value.body_seen_.assign(body);
        }
        state_value.slow_started_observed_ = true;
        state_value.slow_started_->notify();
        const auto result_value = co_await ruvia::sleep_for(
            context_value.worker(), 5s, context_value.get_stop_token());
        state_value.slow_observed_stop_ = result_value == ruvia::timer_sleep_result::stop_requested;
        state_value.pushed_priority_ = context_value.req().priority();
        co_return context_value.text("must-not-be-published");
    }
    if (context_value.req().method() == "POST") {
        const auto body = co_await context_value.req().text();
        state_value.body_seen_.assign(body);
        co_return context_value.text(std::string_view(body));
    }
    if (!state_value.large_response_header_.empty()) {
        context_value.header("x-large-response", state_value.large_response_header_);
    }
    co_return context_value.text(path);
}

struct routes final {
    handler_state handlers_;
    ruvia::detail::router router_;
    ruvia::detail::router_impl& implementation_{ruvia::detail::router_impl::from(router_)};

    routes() {
        add(ruvia::http_known_method::get, "/first");
        add(ruvia::http_known_method::get, "/push");
        add(ruvia::http_known_method::get, "/throw");
        add(ruvia::http_known_method::get, "/slow");
        add(ruvia::http_known_method::get, "/held");
        add(ruvia::http_known_method::get, "/file");
        add(ruvia::http_known_method::post, "/body");
        add(ruvia::http_known_method::post, "/slow-body");
        add(ruvia::http_known_method::get, "/advertise");
        implementation_.register_websocket_route(ruvia::http_known_method::get,
            routing_test::path("/socket"),
            ruvia::detail::route_stream_handler_type(&handlers_, &websocket_handler),
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
        implementation_.finalize();
    }

    void add(ruvia::http_known_method method, std::string_view path) {
        implementation_.register_route(method, routing_test::path(path),
            ruvia::detail::route_handler_type(&handlers_, &request_handler),
            ruvia::detail::request_body_mode::buffered,
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
    }
};

struct fixture final {
    routes routes_;
    ruvia::worker_memory worker_;
    ruvia::stop_source worker_stop_source_;
    ruvia::stop_token worker_stop_;
    ruvia::detail::context_services services_;
    ruvia::detail::http_server_options options_;
    ruvia::connection_scanner scanner_;
    asio::any_io_executor executor_;

    fixture(const ruvia::worker_handle& worker_handle_value,
        std::pmr::memory_resource& upstream)
        : worker_(upstream),
          worker_stop_(worker_stop_source_.token()),
          services_(worker_handle_value, worker_stop_),
          scanner_(worker_handle_value, {.scan_interval_ = 2ms}),
          executor_(asio::system_executor{}) {}
};

inline std::string frame(std::uint64_t type, std::string_view payload_value) {
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto size = ruvia::encode_http3_frame_header(header_value, type, payload_value.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 test frame encoding failed");
    }
    std::string wire(header_value.data(), std::get<0>(size));
    wire.append(payload_value);
    return wire;
}

inline std::string request_wire(ruvia::worker_memory& worker_value, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::http3_field_section_field_view> fields = {}) {
    const auto encoded = ruvia::encode_http3_client_request_head({.method_ = method,
                                                                     .scheme_ = "https",
                                                                     .authority_ = "example.test",
                                                                     .path_ = path,
                                                                     .fields_ = fields,
                                                                     .body_length_ = body.empty()
                                                                                         ? std::nullopt
                                                                                         : std::optional<std::uint64_t>(body.size())},
        {}, worker_value.resource());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 test request-head encoding failed");
    }
    std::string wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded).field_section_.data(), std::get<0>(encoded).field_section_.size()));
    if (!body.empty()) {
        wire += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), body);
    }
    return wire;
}

inline bool accepted(buffer::send_result result_value) noexcept {
    return result_value == buffer::send_result::sent;
}

inline bool accepted(buffer::control_result result_value) noexcept {
    return result_value == buffer::control_result::sent;
}

inline connection_type::event_result_type route_request(connection_type& connection, buffer& inbound,
    ruvia::worker_memory& worker_value, message_id_type id, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::http3_field_section_field_view> fields = {}) {
    const auto wire = request_wire(worker_value, method, path, body, fields);
    if (wire.size() > buffer::max_block_bytes) {
        throw std::runtime_error("HTTP/3 test request exceeded one buffer block");
    }
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.try_send(id, bytes_value)) ||
        !accepted(inbound.try_send_control(
            {control_type::kind::stream_fin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 test inbound buffer is full");
    }

    control_type fin;
    if (!inbound.try_receive_control(fin)) {
        throw std::runtime_error("HTTP/3 test FIN control is missing");
    }
    const auto deferred = connection.accept_control(fin);
    if (deferred.input_.status_ != connection_type::input_type::status_type::deferred_fin) {
        throw std::runtime_error("HTTP/3 test FIN was not deferred behind its data");
    }

    buffer::borrowed_block block;
    if (!inbound.try_receive(block) || block.id().stream_id_ != id.stream_id_) {
        throw std::runtime_error("HTTP/3 test data block is missing");
    }
    auto result_value = connection.accept_data(block);
    block.release();
    return result_value;
}

inline connection_type::event_result_type accept_wire_bytes(connection_type& connection, buffer& inbound,
    message_id_type id, std::span<const char> wire);

inline std::string websocket_request_wire(ruvia::worker_memory& worker_value, std::string_view version) {
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", version}};
    const auto encoded = ruvia::encode_http3_field_section(fields_value, worker_value.resource());
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 WebSocket field section encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded).data(), std::get<0>(encoded).size()));
}

inline connection_type::event_result_type accept_wire_bytes(connection_type& connection, buffer& inbound,
    message_id_type id, std::span<const char> wire) {
    connection_type::event_result_type result;
    for (std::size_t offset = 0; offset < wire.size();) {
        const auto size = std::min(buffer::max_block_bytes, wire.size() - offset);
        const auto chunk = wire.subspan(offset, size);
        if (!accepted(inbound.try_send(id, std::as_bytes(chunk)))) {
            throw std::runtime_error("HTTP/3 raw-wire fixture buffer is full");
        }
        buffer::borrowed_block block;
        if (!inbound.try_receive(block)) {
            throw std::runtime_error("HTTP/3 raw-wire fixture block is missing");
        }
        result = connection.accept_data(block);
        block.release();
        offset += size;
        if (result.status_ != connection_type::event_status_type::accepted) {
            return result;
        }
    }
    return result;
}

struct published_wire final {
    std::string bytes_;
    std::optional<std::uint64_t> final_wire_bytes_;
};

inline std::size_t wire_index(std::uint64_t stream_id) {
    if ((stream_id & 3U) != 0 || stream_id / 4 >= 6) {
        throw std::runtime_error("unexpected HTTP/3 test stream ID");
    }
    return static_cast<std::size_t>(stream_id / 4);
}

inline void drain_data_only(buffer& outbound, std::array<published_wire, 6>& wires) {
    buffer::borrowed_block block;
    while (outbound.try_receive(block)) {
        auto& wire = wires[wire_index(block.id().stream_id_)];
        const auto bytes_value = block.bytes();
        wire.bytes_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
        block.release();
    }
}

inline void drain_all(buffer& outbound, std::array<published_wire, 6>& wires) {
    bool again = false;
    do {
        control_type control;
        while (outbound.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::stream_fin) {
                auto& wire = wires[wire_index(control.id_.stream_id_)];
                wire.final_wire_bytes_ = control.value_;
            }
        }
        drain_data_only(outbound, wires);
        again = outbound.has_pending();
    } while (again);
}

struct decoded_response final {
    std::size_t informational_heads_{};
    std::size_t final_heads_{};
    std::size_t message_ends_{};
    std::uint16_t status_{};
    std::string body_;
};

inline void capture_response(void* raw, const ruvia::http3_client_response_event& event) {
    auto& response = *static_cast<decoded_response*>(raw);
    if (event.kind_ == ruvia::http3_client_response_event_kind::informational_head) {
        ++response.informational_heads_;
    } else if (event.kind_ == ruvia::http3_client_response_event_kind::final_head) {
        ++response.final_heads_;
        response.status_ = event.head_->status_;
    } else if (event.kind_ == ruvia::http3_client_response_event_kind::body) {
        response.body_.append(event.body_.data(), event.body_.size());
    } else if (event.kind_ == ruvia::http3_client_response_event_kind::message_end) {
        ++response.message_ends_;
    }
}

inline decoded_response decode_response(const published_wire& wire, ruvia::http_known_method method,
    std::uint64_t stream_id, std::pmr::memory_resource* resource) {
    if (!wire.final_wire_bytes_ || *wire.final_wire_bytes_ != wire.bytes_.size()) {
        throw std::runtime_error("HTTP/3 response FIN length does not match its wire bytes");
    }
    decoded_response response;
    ruvia::http3_client_response decoder(stream_id, method, resource);
    const auto result_value = decoder.feed(
        std::span<const char>(wire.bytes_.data(), wire.bytes_.size()), true, false,
        &capture_response, &response);
    if (result_value.status_ != ruvia::http3_client_response_status::message_end) {
        throw std::runtime_error("HTTP/3 response decoder rejected published bytes");
    }
    return response;
}

inline ruvia::task<void> stop_after(ruvia::event_loop_attachment& attachment,
    ruvia::task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

inline void run_worker_task(ruvia::event_loop_attachment& attachment, ruvia::task<void> operation) {
    auto root = attachment.loop().start(stop_after(attachment, std::move(operation)));
    attachment.run();
    root.get();
}

inline ruvia::task<void> wait_for_ready(connection_type& connection, std::size_t count,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        if (connection.ready_request_count() == count) {
            co_return;
        }
        const auto sleep = co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value);
        if (sleep != ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 buffered requests did not become ready in time");
}

inline ruvia::task<bool> wait_for_slow_start(fixture& fixture_value,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        if (fixture_value.routes_.handlers_.slow_started_observed_) {
            co_return true;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    co_return fixture_value.routes_.handlers_.slow_started_observed_;
}

inline void require_watchdog_success(ruvia::testing::test_context& ruvia_ctx, bool succeeded);

inline ruvia::task<bool> wait_for_task_count(connection_type& connection, std::size_t count,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value) {
    for (std::size_t attempt_value = 0; attempt_value < 2000; ++attempt_value) {
        if (connection.active_task_count() == count) {
            co_return true;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms, stop_token_value) !=
            ruvia::timer_sleep_result::elapsed) {
            break;
        }
    }
    co_return connection.active_task_count() == count;
}

inline void require_watchdog_success(ruvia::testing::test_context& ruvia_ctx, bool succeeded) {
    RUVIA_CHECK(succeeded);
    if (!succeeded) {
        std::terminate();
    }
}

inline void simulate_global_stop_takeover(connection_type& connection) {
    const auto close_intent = connection.peek_transport_intent();
    if (!close_intent ||
        close_intent->token_.kind_ != connection_type::transport_intent_kind_type::connection_close ||
        !connection.take_over_transport_retirement({.epoch_ = close_intent->token_.id_.epoch_,
            .connection_generation_ = close_intent->token_.id_.connection_generation_}) ||
        !connection.ack_transport_intent(close_intent->token_)) {
        throw std::runtime_error("test local lifecycle owner failed to take over HTTP/3 retirement");
    }
    while (const auto intent = connection.peek_transport_intent()) {
        if (!connection.ack_transport_intent(intent->token_)) {
            throw std::runtime_error("test local lifecycle owner failed to settle HTTP/3 intent");
        }
    }
}

inline ruvia::task<void> publish_group(connection_type& connection, buffer& outbound,
    std::array<published_wire, 6>& wires, std::span<const std::uint64_t> stream_ids,
    const ruvia::worker_handle& worker_value, const ruvia::stop_token& stop_token_value,
    bool exercise_backpressure, ruvia::testing::test_context& ruvia_ctx) {
    std::size_t finished = 0;
    bool saw_data_backpressure = false;
    bool saw_control_backpressure = false;

    if (exercise_backpressure) {
        co_await wait_for_ready(connection, stream_ids.size(), worker_value, stop_token_value);
        const auto first = connection.publish_one(all_work_lanes);
        RUVIA_CHECK(first.status_ == connection_type::publish_status_type::attempted);
        RUVIA_CHECK_EQ(first.stream_id_, stream_ids[0]);
        RUVIA_CHECK(first.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::bytes_published);
        RUVIA_CHECK(first.publication_.block_reason_ ==
                    connection_type::dispatch_type::publish_block_reason_type::none);

        // Leave the first DATA block queued. The next sibling must still receive
        // its turn and report the shared DATA lane as blocked.
        const auto second = connection.publish_one(all_work_lanes);
        RUVIA_CHECK(second.status_ == connection_type::publish_status_type::attempted);
        RUVIA_CHECK_EQ(second.stream_id_, stream_ids[1]);
        RUVIA_CHECK(second.publication_.status_ ==
                    connection_type::dispatch_type::publish_status_type::backpressured);
        RUVIA_CHECK(second.publication_.block_reason_ ==
                    connection_type::dispatch_type::publish_block_reason_type::data);
        saw_data_backpressure = true;
        drain_all(outbound, wires);
        RUVIA_CHECK_EQ(connection.reactivate_blocked({.data_ = true}), std::size_t{1});
    }

    std::size_t attempts = 0;
    while (finished < stream_ids.size() && ++attempts < 20000) {
        const auto attempt_value = connection.publish_one(all_work_lanes);
        if (attempt_value.status_ == connection_type::publish_status_type::no_ready_request) {
            drain_all(outbound, wires);
            const auto reactivated = connection.reactivate_blocked(
                {.data_ = true, .control_ = true});
            if (reactivated == 0) {
                throw std::runtime_error("HTTP/3 buffered publisher stalled without blocked work");
            }
            continue;
        }
        RUVIA_CHECK(attempt_value.status_ == connection_type::publish_status_type::attempted);
        if (attempt_value.status_ != connection_type::publish_status_type::attempted) {
            break;
        }
        const auto result_value = attempt_value.publication_;
        if (result_value.status_ == connection_type::dispatch_type::publish_status_type::fin_published) {
            ++finished;
        } else if (result_value.status_ == connection_type::dispatch_type::publish_status_type::backpressured) {
            if (result_value.block_reason_ == connection_type::dispatch_type::publish_block_reason_type::data) {
                saw_data_backpressure = true;
                drain_data_only(outbound, wires);
                (void)connection.reactivate_blocked({.data_ = true});
            } else if (result_value.block_reason_ == connection_type::dispatch_type::publish_block_reason_type::control) {
                saw_control_backpressure = true;
                drain_all(outbound, wires);
                (void)connection.reactivate_blocked(
                    {.data_ = true, .control_ = true});
            } else {
                RUVIA_CHECK(false);
            }
        } else if (result_value.status_ == connection_type::dispatch_type::publish_status_type::bytes_published) {
            drain_data_only(outbound, wires);
            (void)connection.reactivate_blocked({.data_ = true});
        } else {
            RUVIA_CHECK(result_value.status_ == connection_type::dispatch_type::publish_status_type::complete);
            if (result_value.status_ != connection_type::dispatch_type::publish_status_type::complete) {
                break;
            }
            ++finished;
        }
    }
    drain_all(outbound, wires);
    RUVIA_CHECK(attempts < 20000);
    RUVIA_CHECK_EQ(finished, stream_ids.size());
    if (exercise_backpressure) {
        RUVIA_CHECK(saw_data_backpressure);
        RUVIA_CHECK(saw_control_backpressure);
    }
}

}  // namespace
