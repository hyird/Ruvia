#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include <asio/system_executor.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_client_response.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http_byte_range.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "http3/http3_buffered_request_dispatch.h"
#include "http3/http3_connection_state.h"
#include "http3/http3_server_stream_input.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace ruvia::testing {
http3_datagram_receive_status plan_connect_datagram_for_peer(
    detail::http3_connection_identity identity,
    std::optional<detail::http3_stream_control> marker,
    std::uint64_t accepted_wire_bytes, std::span<const char> bytes_value);
}

namespace {

using dispatch_type = ruvia::detail::http3_buffered_request_dispatch;
using engine_type = ruvia::detail::http3_sans_io_session_engine;
using input_type = ruvia::detail::http3_server_stream_input;
using buffer = ruvia::detail::http3_stream_buffer;
using message_id_type = ruvia::detail::http3_stream_id;
using control_type = ruvia::detail::http3_stream_control;
using block_reason_type = dispatch_type::publish_block_reason_type;
using publication_demand_type = dispatch_type::publication_demand_type;

constexpr std::uint64_t epoch = 37;
constexpr std::uint64_t generation = 53;
using namespace std::chrono_literals;

struct handler_state final {
    std::string response_body_{"buffered-h3-ok"};
    std::string response_trailer_{"yes"};
    std::filesystem::path file_path_;
    bool stream_throw_after_write_{};
    bool send_interim_{};
    bool interim_after_final_rejected_{};
    bool stream_retained_stable_{true};
    std::size_t upload_bytes_{};
    std::size_t upload_chunks_{};
    bool upload_trailer_observed_{};
    std::size_t handler_calls_{};
    std::size_t replay_safe_middleware_calls_{};
    ruvia::http3_early_data_info early_data_info_{};
    ruvia::worker_signal* started_{};
    bool request_read_correctly_{};
    bool handler_resumed_{};
    bool error_handler_called_{};
    bool error_handler_response_ready_{};
    std::string error_code_;
    bool allocate_error_headers_{};
    bool throw_from_error_handler_{};
    bool response_no_transform_{};
    bool error_no_transform_{};
    bool first_error_no_transform_{};
    std::string error_body_{"handled-error"};
    std::size_t error_handler_calls_{};
    std::size_t suspend_error_call_{};
    ruvia::worker_signal* error_started_{};
    std::string large_response_header_;
    ruvia::worker_signal* websocket_started_{};
    ruvia::worker_signal* websocket_message_received_{};
    ruvia::worker_signal* websocket_message_echoed_{};
    std::array<std::string, 2> websocket_messages_{};
    std::size_t websocket_echoes_{};
    bool websocket_saw_fin_{};
    bool websocket_throw_on_start_{};
    std::size_t udp_tunnel_starts_{};
    bool websocket_retained_data_stable_{true};
    std::string tunnel_received_;
    bool tunnel_read_after_finish_{};
    bool tunnel_return_early_{};
    bool tunnel_stable_{true};
};

inline ruvia::task<ruvia::http_response> buffered_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    ++state_value.handler_calls_;
    state_value.early_data_info_ = context_value.early_data_info();
    if (state_value.send_interim_) {
        std::string value(512, 'h');
        const std::array headers{ruvia::http_header_view("Link", value)};
        auto operation = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
        value.assign("changed");
        co_await std::move(operation);
    }
    const auto path = context_value.req().path();
    if (path == "/throw") {
        throw std::runtime_error("buffered handler failure");
    }
    if (path == "/suspend") {
        state_value.started_->notify();
        const auto sleep = co_await ruvia::sleep_for(
            context_value.worker(), 10s, context_value.get_stop_token());
        state_value.handler_resumed_ = true;
        if (sleep == ruvia::timer_sleep_result::stop_requested) {
            throw std::runtime_error("buffered handler stopped");
        }
        co_return context_value.text("unexpectedly resumed");
    }
    if (path == "/multipart-file") {
        ruvia::http_response response({.resource_ = context_value.arena()});
        auto ranges = ruvia::resolve_http_byte_range_set(
            "bytes=0-19999,30000-", state_value.response_body_.size());
        auto plan = ruvia::make_http_multipart_byte_range_plan(
            ranges, state_value.response_body_.size(), "text/plain", "h3_test_boundary", {},
            context_value.arena());
        response.status(ruvia::http_status::partial_content);
        response.header("Content-Type", plan.content_type());
        response.multipart_file_body(state_value.file_path_, state_value.response_body_.size(),
            ruvia::http_response_file_identity::unchecked(), std::move(plan));
        co_return response;
    }
    if (path == "/file") {
        ruvia::http_response response({.resource_ = context_value.arena()});
        if (state_value.file_path_.empty()) {
            response.file_body("virtual-response.bin", 5, 0, 5, ruvia::http_response_file_identity::checked({}));
        } else {
            response.file_body(state_value.file_path_, state_value.response_body_.size(), 0, state_value.response_body_.size(), ruvia::http_response_file_identity::unchecked());
        }
        co_return response;
    }
    if (path == "/empty-file") {
        ruvia::http_response response({.resource_ = context_value.arena()});
        response.file_body("virtual-empty-response.bin", 0, 0, 0, ruvia::http_response_file_identity::checked({}));
        co_return response;
    }
    if (path == "/items") {
        const auto body = co_await context_value.req().text();
        state_value.request_read_correctly_ = context_value.req().method() == "POST" &&
                                              context_value.req().path() == "/items" &&
                                              body == "payload" &&
                                              context_value.req().header("x-input") == "present" &&
                                              context_value.req().cookie("session") == "one" &&
                                              context_value.req().cookie("other") == "two";
    }
    if (path == "/upload") {
        auto& reader_value = context_value.req().get_body_reader();
        while (auto bytes = co_await reader_value.read()) {
            state_value.upload_bytes_ += bytes->size();
            ++state_value.upload_chunks_;
            if (state_value.started_ != nullptr) {
                state_value.started_->notify();
            }
            if (std::ranges::any_of(*bytes, [](std::byte byte) { return byte != std::byte{'u'}; })) {
                throw std::runtime_error("upload chunk corrupted");
            }
        }
        state_value.upload_trailer_observed_ = context_value.req().trailer("x-checksum") == "final" && context_value.req().header("x-checksum") == "initial";
    }
    context_value.header("x-dispatch", "buffered");
    if (state_value.response_no_transform_) {
        context_value.header("cache-control", "no-transform");
    }
    if (!state_value.large_response_header_.empty()) {
        context_value.header("x-large-response", state_value.large_response_header_);
    }
    co_return context_value.text(std::string_view(state_value.response_body_));
}

inline ruvia::task<void> buffered_websocket_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    const std::string expected_retained(512, 'r');
    const std::pmr::string retained(expected_retained, context_value.pool());
    state_value.websocket_started_->notify();
    if (state_value.websocket_throw_on_start_) {
        throw std::runtime_error("websocket handler failure");
    }
    auto& websocket_value = context_value.get_websocket();
    for (std::size_t index = 0; index < state_value.websocket_messages_.size(); ++index) {
        auto message = co_await websocket_value.read();
        if (!message) {
            break;
        }
        std::pmr::string temporary(message->payload(), context_value.pool());
        state_value.websocket_messages_[index].assign(temporary);
        state_value.websocket_message_received_->notify();
        co_await websocket_value.text(std::move(temporary));
        ++state_value.websocket_echoes_;
        state_value.websocket_retained_data_stable_ = state_value.websocket_retained_data_stable_ &&
                                                      std::string_view(retained.data(), retained.size()) ==
                                                          expected_retained;
        state_value.websocket_message_echoed_->notify();
    }
    state_value.websocket_saw_fin_ = !(co_await websocket_value.read()).has_value();
    state_value.websocket_retained_data_stable_ = state_value.websocket_retained_data_stable_ &&
                                                  std::string_view(retained.data(), retained.size()) ==
                                                      expected_retained;
}

inline ruvia::task<void> buffered_tunnel_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    if (state_value.tunnel_return_early_) {
        co_return;
    }
    auto& tunnel = context_value.tunnel();
    std::optional<std::pmr::string> retained;
    if (state_value.tunnel_read_after_finish_) {
        co_await tunnel.finish();
    }
    while (auto chunk = co_await tunnel.read()) {
        state_value.tunnel_received_.append(*chunk);
        if (!retained) {
            retained.emplace(*chunk, context_value.pool());
        }
        if (!state_value.tunnel_read_after_finish_) {
            auto write = tunnel.write(std::string_view(*chunk));
            chunk->assign("mutated");
            co_await std::move(write);
        }
        state_value.tunnel_stable_ = state_value.tunnel_stable_ && retained->find_first_not_of('c') == std::string_view::npos;
    }
}

inline ruvia::task<void> buffered_udp_tunnel_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    ruvia::http_udp_tunnel udp(context_value.tunnel().capsules());
    ++state_value.udp_tunnel_starts_;
    if (state_value.tunnel_read_after_finish_) {
        co_await udp.finish();
    }
    std::optional<ruvia::http_udp_datagram> retained;
    while (auto packet = co_await udp.read()) {
        const auto bytes_value = packet->payload();
        state_value.tunnel_received_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
        if (!state_value.tunnel_read_after_finish_) {
            co_await udp.send(bytes_value);
        }
        if (!retained) {
            retained = std::move(packet);
        }
        state_value.tunnel_stable_ = state_value.tunnel_stable_ && std::ranges::all_of(retained->payload(), [](std::byte byte) { return byte == std::byte{'c'}; });
    }
    co_await udp.finish();
}

inline ruvia::task<void> response_stream_handler(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<handler_state*>(raw);
    ++state_value.handler_calls_;
    const std::pmr::string retained(2048, 'r', context_value.pool());
    if (state_value.send_interim_) {
        const std::array headers{ruvia::http_header_view("Link", std::string_view("</style.css>; rel=preload"))};
        {
            auto cold = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
    }
    context_value.header("x-dispatch", "stream");
    for (unsigned i = 0; i < 4; ++i) {
        {
            auto cold = context_value.stream().write("discarded");
        }
        co_await context_value.stream().write(std::string_view(state_value.response_body_));
        if (state_value.send_interim_) {
            try {
                auto invalid = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::continue_value));
            } catch (const std::logic_error&) {
                state_value.interim_after_final_rejected_ = true;
            }
        }
        state_value.stream_retained_stable_ = state_value.stream_retained_stable_ && retained == std::pmr::string(2048, 'r', context_value.pool());
        if (state_value.stream_throw_after_write_) {
            throw std::runtime_error("stream failed after head");
        }
    }
    const std::array trailers{ruvia::http_header_view("x-complete", state_value.response_trailer_)};
    co_await context_value.stream().end(trailers);
}

struct replay_safe_route_middleware final : ruvia::middleware {
    static constexpr bool ruvia_replay_safe = true;
    handler_state* state_{};

    explicit replay_safe_route_middleware(handler_state* state_value)
        : state_(state_value) {}
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        ++state_->replay_safe_middleware_calls_;
        co_await next_value();
    }
};

struct routes final {
    ruvia::detail::router router_;
    ruvia::detail::router_impl& implementation_{ruvia::detail::router_impl::from(router_)};
    handler_state handlers_;
    ruvia::http_error_handler_type error_handler_;

    explicit routes(std::chrono::milliseconds peer_transport_fin_timeout = 5s)
        : error_handler_([this](ruvia::context& context_value, ruvia::http_error_info error)
                             -> ruvia::task<ruvia::http_response> {
              handlers_.error_handler_called_ = true;
              ++handlers_.error_handler_calls_;
              handlers_.error_code_.assign(error.code());
              if (handlers_.throw_from_error_handler_) {
                  throw std::runtime_error("error handler failed");
              }
              context_value.status(error.status());
              context_value.header("x-error-handler", "used");
              if (handlers_.error_no_transform_ ||
                  (handlers_.first_error_no_transform_ && handlers_.error_handler_calls_ == 1)) {
                  context_value.header("cache-control", "no-transform");
              }
              if (handlers_.suspend_error_call_ == handlers_.error_handler_calls_) {
                  handlers_.error_started_->notify();
                  (void)co_await ruvia::sleep_for(context_value.worker(), 10s, context_value.get_stop_token());
              }
              if (handlers_.allocate_error_headers_) {
                  for (unsigned i = 0; i < 32; ++i) {
                      context_value.header("x-owned-error-" + std::to_string(i), std::string_view(handlers_.response_body_));
                  }
              }
              auto response = context_value.text(std::string_view(handlers_.error_body_));
              handlers_.error_handler_response_ready_ = true;
              co_return response;
          }) {
        implementation_.set_error_handler(ruvia::detail::callback_access::ref(error_handler_));
        add(ruvia::http_known_method::post, "/items");
        add(ruvia::http_known_method::get, "/throw");
        add(ruvia::http_known_method::get, "/suspend");
        add(ruvia::http_known_method::get, "/file");
        add(ruvia::http_known_method::get, "/multipart-file");
        add(ruvia::http_known_method::get, "/empty-file");
        add(ruvia::http_known_method::get, "/large");
        const auto replay_safe_middleware =
            ruvia::detail::make_middleware_descriptor<replay_safe_route_middleware>(&handlers_);
        implementation_.register_route(ruvia::http_known_method::get, routing_test::path("/early-safe"),
            ruvia::detail::route_handler_type(&handlers_, &buffered_handler),
            ruvia::detail::request_body_mode::buffered, {},
            std::span<const ruvia::detail::controller_middleware_descriptor>(
                &replay_safe_middleware, 1));
        implementation_.register_route(ruvia::http_known_method::post, routing_test::path("/upload"),
            ruvia::detail::route_handler_type(&handlers_, &buffered_handler), ruvia::detail::request_body_mode::stream, {}, {});
        implementation_.register_response_stream_route(ruvia::http_known_method::get, routing_test::path("/stream"),
            ruvia::detail::route_stream_handler_type(&handlers_, &response_stream_handler), {}, {});
        ruvia::websocket_route_config websocket_config;
        websocket_config.lifecycle_.peer_transport_fin_timeout_ = peer_transport_fin_timeout;
        implementation_.register_websocket_route(ruvia::http_known_method::get,
            routing_test::path("/socket"),
            ruvia::detail::route_stream_handler_type(&handlers_, &buffered_websocket_handler),
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::move(websocket_config));
        implementation_.register_tunnel_route({}, std::pmr::string("backend.test:443"),
            ruvia::detail::route_stream_handler_type(&handlers_, &buffered_tunnel_handler), {}, {}, {.peer_transport_fin_timeout_ = peer_transport_fin_timeout});
        implementation_.register_tunnel_route("test-tunnel", std::pmr::string("/tunnel/:destination"),
            ruvia::detail::route_stream_handler_type(&handlers_, &buffered_tunnel_handler), {}, {}, {.peer_transport_fin_timeout_ = peer_transport_fin_timeout});
        implementation_.register_tunnel_route("connect-udp", std::pmr::string("/udp/:destination"),
            ruvia::detail::route_stream_handler_type(&handlers_, &buffered_udp_tunnel_handler), {}, {}, {.peer_transport_fin_timeout_ = peer_transport_fin_timeout});
        implementation_.finalize();
    }

    void add(ruvia::http_known_method method, std::string_view path) {
        implementation_.register_route(method, routing_test::path(path),
            ruvia::detail::route_handler_type(&handlers_, &buffered_handler),
            ruvia::detail::request_body_mode::buffered,
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
    }
};

class allocation_switch final : public std::pmr::memory_resource {
public:
    explicit allocation_switch(std::pmr::memory_resource& upstream)
        : upstream_(upstream) {}
    bool reject_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return upstream_.allocate(bytes_value, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        upstream_.deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::pmr::memory_resource& upstream_;
};

struct fixture final {
    routes routes_;
    ruvia::test::counting_memory_resource& upstream_;
    allocation_switch allocations_;
    ruvia::worker_memory worker_;
    ruvia::stop_source worker_stop_source_;
    ruvia::stop_token worker_stop_;
    ruvia::detail::context_services services_;
    ruvia::detail::http_server_options options_;
    ruvia::detail::http3_server_body_budget body_budget_;
    engine_type session_;
    buffer inbound_;
    input_type input_;
    buffer outbound_;
    ruvia::connection_scanner::entry_type scanner_entry_;
    ruvia::connection_scanner scanner_;
    asio::any_io_executor executor_{asio::system_executor{}};

    fixture(const ruvia::worker_handle& worker_handle_value,
        ruvia::test::counting_memory_resource& upstream,
        std::uint32_t outbound_blocks = 1, std::uint32_t outbound_data_slots = 1,
        std::uint32_t outbound_control_slots = 1,
        std::size_t worker_body_budget = 64 * 1024 * 1024,
        std::size_t tunnel_bytes_per_stream = 64 * 1024,
        std::chrono::milliseconds peer_transport_fin_timeout = 5s,
        std::chrono::milliseconds scanner_interval = 1ms, std::size_t max_native_payload_bytes = 0)
        : routes_(peer_transport_fin_timeout),
          upstream_(upstream),
          allocations_(upstream),
          worker_(allocations_),
          worker_stop_(worker_stop_source_.token()),
          services_(worker_handle_value, worker_stop_),
          body_budget_(worker_body_budget),
          session_(routes_.implementation_.route_table(), worker_, body_budget_,
              {.max_tunnel_buffered_bytes_ = tunnel_bytes_per_stream,
                  .connection_ = {.enable_connect_protocol_ = true, .enable_datagrams_ = max_native_payload_bytes != 0},
                  .max_quic_datagram_payload_bytes_ = max_native_payload_bytes}),
          inbound_(8, 8, 4, worker_.resource()),
          input_(session_, worker_, epoch, generation, 32),
          outbound_(outbound_blocks, outbound_data_slots, outbound_control_slots, worker_.resource()),
          scanner_(worker_handle_value, {.scan_interval_ = scanner_interval}) {}

    [[nodiscard]] dispatch_type make_dispatch(std::uint64_t stream_id,
        ruvia::detail::context_services dispatch_services,
        ruvia::detail::http3_tunnel_callbacks callbacks = {}, bool received_early_data = false) {
        return dispatch_type(session_, routes_.implementation_.route_table(), worker_,
            std::move(dispatch_services), options_, outbound_,
            {epoch, generation, stream_id, {}, received_early_data},
            scanner_entry_, executor_, callbacks);
    }
};

struct tunnel_callbacks_state final {
    tunnel_callbacks_state(const ruvia::worker_handle& worker_value,
        ruvia::connection_scanner& scanner)
        : scanner_(scanner),
          output_ready_(worker_value) {}

    [[nodiscard]] ruvia::detail::http3_tunnel_callbacks callbacks() noexcept {
        return {.context_ = this,
            .attach_scanner_ = [](void* raw, std::uint64_t,
                                   ruvia::connection_scanner::entry_type& entry_value) noexcept {
                auto& state_value = *static_cast<tunnel_callbacks_state*>(raw);
                if (!state_value.scanner_attached_) {
                    state_value.scanner_.register_entry(entry_value);
                    state_value.scanner_attached_ = true;
                }
                return true; },
            .output_ready_ = [](void* raw, std::uint64_t) noexcept { static_cast<tunnel_callbacks_state*>(raw)->output_ready_.notify(); },
            .abort_ = [](void* raw, std::uint64_t) noexcept {
                auto& state_value = *static_cast<tunnel_callbacks_state*>(raw);
                state_value.aborted_ = true;
                state_value.output_ready_.notify(); }};
    }

    ruvia::connection_scanner& scanner_;
    ruvia::worker_signal output_ready_;
    bool scanner_attached_{};
    bool aborted_{};
};

inline std::string frame(std::uint64_t type, std::string_view payload_value) {
    std::array<char, ruvia::http3_frame_header_max_bytes> header_value{};
    const auto size = ruvia::encode_http3_frame_header(header_value, type, payload_value.size());
    if ((size.index() != 0)) {
        throw std::runtime_error("HTTP/3 fixture frame encoding failed");
    }
    std::string result_value(header_value.data(), std::get<0>(size));
    result_value.append(payload_value);
    return result_value;
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
        throw std::runtime_error("HTTP/3 fixture request-head encoding failed");
    }
    std::string result_value = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(encoded).field_section_.data(), std::get<0>(encoded).field_section_.size()));
    if (!body.empty()) {
        result_value += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::data), body);
    }
    return result_value;
}

inline bool send_accepted(buffer::send_result result_value) noexcept {
    return result_value == buffer::send_result::sent;
}

inline bool control_accepted(buffer::control_result result_value) noexcept {
    return result_value == buffer::control_result::sent;
}

inline void feed_websocket_request(engine_type& session_value, ruvia::worker_memory& worker_value,
    std::uint64_t stream_id, std::string_view version = "13",
    std::string_view accept_encoding = {}) {
    const std::array fields_value{
        ruvia::http3_field_section_field_view{":method", "CONNECT"},
        ruvia::http3_field_section_field_view{":protocol", "websocket"},
        ruvia::http3_field_section_field_view{":scheme", "https"},
        ruvia::http3_field_section_field_view{":authority", "example.test"},
        ruvia::http3_field_section_field_view{":path", "/socket"},
        ruvia::http3_field_section_field_view{"sec-websocket-version", version},
        ruvia::http3_field_section_field_view{"accept-encoding", accept_encoding}};
    std::pmr::vector<ruvia::http3_field_section_field_view> fields_to_encode(worker_value.resource());
    for (const auto& field : fields_value) {
        if ((field.name_ == "sec-websocket-version" && version.empty()) ||
            (field.name_ == "accept-encoding" && accept_encoding.empty())) {
            continue;
        }
        fields_to_encode.push_back(field);
    }
    const auto field_section = ruvia::encode_http3_field_section(fields_to_encode, worker_value.resource());
    if ((field_section.index() != 0)) {
        throw std::runtime_error("HTTP/3 WebSocket fixture field section encoding failed");
    }
    const std::string wire = frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::headers),
        std::string_view(std::get<0>(field_section).data(), std::get<0>(field_section).size()));
    const auto result_value = session_value.feed(stream_id, wire);
    if (result_value.scope_ != ruvia::http3_connection_error_scope::none ||
        session_value.stream_state(stream_id) != engine_type::stream_state_type::ready) {
        throw std::runtime_error("HTTP/3 WebSocket fixture request was rejected");
    }
}

inline void feed_websocket_request(fixture& fixture_value, std::uint64_t stream_id) {
    feed_websocket_request(fixture_value.session_, fixture_value.worker_, stream_id);
}

inline void feed_request(fixture& fixture_value, std::uint64_t stream_id, std::string_view method,
    std::string_view path, std::string_view body = {},
    std::span<const ruvia::http3_field_section_field_view> fields = {},
    bool received_early_data = false) {
    const message_id_type id{epoch, generation, stream_id, {}, received_early_data};
    const auto wire = request_wire(fixture_value.worker_, method, path, body, fields);
    if (wire.size() > buffer::max_block_bytes) {
        throw std::runtime_error("HTTP/3 fixture request exceeds one buffer block");
    }
    const auto bytes_value = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!send_accepted(fixture_value.inbound_.try_send(id, bytes_value)) ||
        !control_accepted(fixture_value.inbound_.try_send_control(
            {control_type::kind::stream_fin, id, wire.size()}))) {
        throw std::runtime_error("HTTP/3 fixture inbound buffer is full");
    }

    control_type fin;
    if (!fixture_value.inbound_.try_receive_control(fin) ||
        fixture_value.input_.accept_control(fin).status_ != input_type::status_type::deferred_fin) {
        throw std::runtime_error("HTTP/3 fixture FIN was not deferred");
    }
    buffer::borrowed_block block;
    if (!fixture_value.inbound_.try_receive(block) || block.id().stream_id_ != stream_id) {
        throw std::runtime_error("HTTP/3 fixture DATA block is unavailable");
    }
    const auto fed = fixture_value.input_.accept_data(block);
    block.release();
    if (fed.status_ != input_type::status_type::finished ||
        fixture_value.session_.stream_state(stream_id) != engine_type::stream_state_type::ready) {
        throw std::runtime_error("HTTP/3 fixture request did not become ready");
    }
}

struct published_wire final {
    std::string bytes_;
    std::optional<std::uint64_t> final_wire_bytes_;
    std::size_t data_blocks_{};
    bool identity_matched_{true};
};

inline std::size_t drain_data_only(buffer& buffer, const message_id_type& expected, published_wire& output) {
    std::size_t received_value = 0;
    buffer::borrowed_block block;
    while (buffer.try_receive(block)) {
        if (block.id().epoch_ != expected.epoch_ ||
            block.id().connection_generation_ != expected.connection_generation_ ||
            block.id().stream_id_ != expected.stream_id_) {
            output.identity_matched_ = false;
        }
        const auto bytes_value = block.bytes();
        output.bytes_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
        ++output.data_blocks_;
        ++received_value;
        block.release();
    }
    return received_value;
}

inline void drain_buffer(buffer& buffer, const message_id_type& expected, published_wire& output) {
    bool again = false;
    do {
        control_type control;
        while (buffer.try_receive_control(control)) {
            if (control.kind_ == control_type::kind::stream_fin &&
                control.id_.epoch_ == expected.epoch_ &&
                control.id_.connection_generation_ == expected.connection_generation_ &&
                control.id_.stream_id_ == expected.stream_id_) {
                output.final_wire_bytes_ = control.value_;
            }
        }
        (void)drain_data_only(buffer, expected, output);
        again = buffer.has_pending();
    } while (again);
}

struct decoded_response final {
    std::size_t final_heads_{};
    std::size_t interim_heads_{};
    std::string early_link_;
    std::size_t body_events_{};
    std::size_t message_ends_{};
    std::uint16_t status_{};
    std::size_t decoded_field_section_size_{};
    std::size_t large_response_header_bytes_{};
    std::optional<std::uint64_t> content_length_;
    std::string dispatch_header_;
    std::string error_header_;
    std::string websocket_version_header_;
    std::string connection_header_;
    std::string upgrade_header_;
    std::string websocket_accept_header_;
    std::string content_type_;
    std::string content_encoding_;
    std::string body_;
    std::string complete_trailer_;
};

inline void on_response(void* raw, const ruvia::http3_client_response_event& event) {
    auto& response = *static_cast<decoded_response*>(raw);
    switch (event.kind_) {
        case ruvia::http3_client_response_event_kind::final_head:
            ++response.final_heads_;
            response.status_ = event.head_->status_;
            response.content_length_ = event.head_->content_length_;
            response.decoded_field_section_size_ = 7 + std::to_string(event.head_->status_).size() + 32;
            for (const auto& header : event.head_->headers_) {
                response.decoded_field_section_size_ += header.name_.size() + header.value_.size() + 32;
                if (header.name_ == "content-type") {
                    response.content_type_ = header.value_;
                } else if (header.name_ == "content-encoding") {
                    response.content_encoding_ = header.value_;
                } else if (header.name_ == "x-dispatch") {
                    response.dispatch_header_ = header.value_;
                } else if (header.name_ == "x-error-handler") {
                    response.error_header_ = header.value_;
                } else if (header.name_ == "x-large-response") {
                    response.large_response_header_bytes_ = header.value_.size();
                } else if (header.name_ == "sec-websocket-version") {
                    response.websocket_version_header_ = header.value_;
                } else if (header.name_ == "connection") {
                    response.connection_header_ = header.value_;
                } else if (header.name_ == "upgrade") {
                    response.upgrade_header_ = header.value_;
                } else if (header.name_ == "sec-websocket-accept") {
                    response.websocket_accept_header_ = header.value_;
                }
            }
            break;
        case ruvia::http3_client_response_event_kind::body:
            ++response.body_events_;
            response.body_.append(event.body_.data(), event.body_.size());
            break;
        case ruvia::http3_client_response_event_kind::message_end:
            ++response.message_ends_;
            break;
        case ruvia::http3_client_response_event_kind::informational_head:
            ++response.interim_heads_;
            for (const auto& header : event.head_->headers_) {
                if (header.name_ == "link") {
                    response.early_link_.assign(header.value_);
                }
            }
            break;
        case ruvia::http3_client_response_event_kind::tunnel_data:
        case ruvia::http3_client_response_event_kind::reset:
        case ruvia::http3_client_response_event_kind::push_promise:
            break;
        case ruvia::http3_client_response_event_kind::trailer_field:
            if (event.trailer_.name_ == "x-complete") {
                response.complete_trailer_.assign(event.trailer_.value_);
            }
            break;
    }
}

inline ruvia::http3_client_response_result decode_published(const published_wire& wire,
    ruvia::http_known_method request_method, std::uint64_t stream_id,
    std::pmr::memory_resource* resource, decoded_response& output) {
    if (!wire.final_wire_bytes_ || *wire.final_wire_bytes_ != wire.bytes_.size()) {
        return {ruvia::http3_client_response_status::stream_error,
            ruvia::http3_connection_error_scope::stream,
            ruvia::http3_connection_error_code::message_error};
    }
    ruvia::http3_client_response decoder(stream_id, request_method, resource);
    return decoder.feed(std::span<const char>(wire.bytes_.data(), wire.bytes_.size()), true, false,
        &on_response, &output);
}

inline void publish_and_drain(dispatch_type& dispatch, fixture& fixture_value, std::uint64_t stream_id,
    published_wire& output, ruvia::testing::test_context& ruvia_ctx,
    bool assert_data_backpressure = false) {
    const message_id_type id{epoch, generation, stream_id};
    std::size_t attempts = 0;
    bool saw_data_backpressure = false;
    while (!dispatch.complete() && ++attempts < 10000) {
        const auto result_value = dispatch.publish_step();
        if (result_value.status_ == dispatch_type::publish_status_type::bytes_published) {
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::none);
            if (assert_data_backpressure && !saw_data_backpressure && !dispatch.complete()) {
                const auto before = dispatch.published_wire_bytes();
                const auto blocked = dispatch.publish_step();
                if (blocked.status_ == dispatch_type::publish_status_type::backpressured) {
                    saw_data_backpressure = true;
                    RUVIA_CHECK(blocked.block_reason_ == block_reason_type::data);
                    RUVIA_CHECK_EQ(dispatch.published_wire_bytes(), before);
                }
            }
            drain_buffer(fixture_value.outbound_, id, output);
        } else if (result_value.status_ == dispatch_type::publish_status_type::backpressured) {
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::data);
            drain_buffer(fixture_value.outbound_, id, output);
        } else if (result_value.status_ == dispatch_type::publish_status_type::fin_published) {
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::none);
            drain_buffer(fixture_value.outbound_, id, output);
        } else if (result_value.status_ == dispatch_type::publish_status_type::complete) {
            RUVIA_CHECK(result_value.block_reason_ == block_reason_type::none);
        } else {
            throw std::runtime_error("HTTP/3 response publication failed");
        }
    }
    if (assert_data_backpressure) {
        RUVIA_CHECK(saw_data_backpressure);
    }
    if (attempts >= 10000 || !dispatch.complete()) {
        throw std::runtime_error("HTTP/3 response publication did not complete");
    }
    drain_buffer(fixture_value.outbound_, id, output);
}

inline ruvia::task<void> run_owner(dispatch_type& dispatch, dispatch_type::run_status_type& result_value, bool& joined,
    ruvia::worker_signal& finished) {
    result_value = co_await dispatch.run_handler();
    joined = true;
    finished.notify();
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

inline void feed_peer_settings(fixture& fixture_value, std::optional<std::uint64_t> max_field_section_size,
    bool native_datagrams = false) {
    std::array<char, 64> payload_value{};
    ruvia::http3_settings settings;
    settings.max_field_section_size_ = max_field_section_size;
    settings.h3_datagram_ = native_datagrams;
    const auto encoded = ruvia::encode_http3_settings(payload_value, settings);
    if ((encoded.index() != 0)) {
        throw std::runtime_error("HTTP/3 fixture SETTINGS encoding failed");
    }
    std::string control_wire(1, '\0');  // Peer unidirectional control stream type.
    control_wire += frame(static_cast<std::uint64_t>(ruvia::http3_frame_type::settings),
        std::string_view(payload_value.data(), std::get<0>(encoded)));
    const auto result_value = fixture_value.session_.feed(2, control_wire);
    if (result_value.scope_ != ruvia::http3_connection_error_scope::none ||
        result_value.status_ != ruvia::http3_connection_status::need_more_data) {
        throw std::runtime_error("HTTP/3 fixture peer SETTINGS was rejected");
    }
}

}  // namespace
