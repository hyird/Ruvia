#include "http2/http2_sans_io_session_engine.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "body/http_request_body_facade.h"
#include "context/http_connection_advertisement_output.h"
#include "context/http_interim_response_output.h"
#include "context/http_push_output.h"
#include "http/http_protocol_error_info.h"
#include "http/http_tunnel_session.h"
#include "http2/http2_request_memory.h"
#include "http2/http2_sans_io_request_body.h"
#include "http2/http2_sans_io_request_body_reader.h"
#include "http2/http2_sans_io_response_stream_sink.h"
#include "http2/http2_sans_io_route_selection.h"
#include "http2/http2_sans_io_tunnel_transport.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_upgrade_route.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_session.h"
#include "websocket/websocket_response_headers.h"

namespace ruvia::detail {

http2_sans_io_session_engine::http2_sans_io_session_engine(asio::any_io_executor executor,
    asio::ip::tcp::socket& socket, const route_table& routes_value, worker_memory& worker_value,
    http2_sans_io_session_context session_value)
    : executor_(std::move(executor)),
      socket_(socket),
      routes_(routes_value),
      worker_(worker_value),
      session_(std::move(session_value)),
      inbound_buffers_(*session_.options().inbound_budget_,
          session_.options().server_.max_inbound_buffer_bytes_per_connection_),
      remote_address_(session_.services().get_conn_info().remote().address()),
      connection_(ruvia::http2_connection::server({.resource_ = worker_value.resource()})),
      write_signal_(session_.services().worker()),
      output_budget_(session_.services().worker()),
      handler_finished_(session_.services().worker()),
      writer_finished_(session_.services().worker()),
      admission_policy_(http_request_admission_policy::server(session_.options())),
      stream_runtimes_(worker_value.resource(), termination_, &inbound_buffers_),
      buffered_response_writer_(connection_, stream_runtimes_, write_signal_, output_budget_,
          response_file_io{.pool_ = session_.options().blocking_pool_,
              .worker_ = session_.services().worker(),
              .stop_ = session_.services().get_stop_token(),
              .resource_ = worker_value.resource()}) {
    // A plaintext handoff reuses the HTTP/1 scanner entry. It now tracks a
    // multiplexed session, not an unadmitted HTTP/1 request-head read.
    set_inactivity_phase();
    session_.scanner_entry().register_periodic_check(drain_registration_, this, [](void*, std::int64_t) noexcept {}, [](void* target) noexcept { static_cast<http2_sans_io_session_engine*>(target)->request_drain(); });
}

bool http2_sans_io_session_engine::wants_write() const noexcept {
    return connection_.wants_write();
}

void http2_sans_io_session_engine::take_output(std::pmr::string& output) {
    // Preserve assign() semantics when the caller reuses its scratch string. The
    // core batch operation only consumes output and invokes its observer after the
    // copy succeeds, so allocation failure leaves protocol bytes and budget intact.
    output.clear();
    (void)connection_.take_output_batch(http2_data_output_credit_bytes, output, [](void* context_value, std::uint32_t stream_id, std::size_t payload_bytes) noexcept { static_cast<http2_data_output_budget*>(context_value)->note_data_output(stream_id, payload_bytes); }, &output_budget_);
    output_write_in_flight_ = !output.empty();
}

bool http2_sans_io_session_engine::write_failed() const noexcept {
    return lifecycle_.write_failed();
}

bool http2_sans_io_session_engine::writer_should_exit() const noexcept {
    return lifecycle_.stopping() && active_handler_tasks_ == 0;
}

task<void> http2_sans_io_session_engine::wait_for_write() {
    co_await write_signal_.wait();
}

void http2_sans_io_session_engine::output_write_completed() noexcept {
    output_write_in_flight_ = false;
    finish_drain_if_ready();
    output_budget_.reconcile(connection_, true);
}

void http2_sans_io_session_engine::writer_write_failed(std::error_code error) noexcept {
    lifecycle_.mark_write_failed();
    terminate(error);
}

void http2_sans_io_session_engine::writer_submitting() noexcept {
    lifecycle_.mark_writer_submitted();
}

void http2_sans_io_session_engine::writer_launch_failed() noexcept {
    lifecycle_.mark_writer_launch_failed();
}

void http2_sans_io_session_engine::writer_completed(std::exception_ptr exception) noexcept {
    if (exception != nullptr) {
        lifecycle_.record_writer_failure(std::move(exception));
        terminate(std::make_error_code(std::errc::operation_canceled));
    }
    lifecycle_.mark_writer_done();
    writer_finished_.notify();
}

bool http2_sans_io_session_engine::connection_failed() const noexcept {
    return connection_.connection_error().has_value();
}

bool http2_sans_io_session_engine::terminated() const noexcept {
    return termination_.terminated();
}

bool http2_sans_io_session_engine::header_block_in_progress() const noexcept {
    return connection_.header_block_in_progress();
}

std::size_t http2_sans_io_session_engine::active_runtime_count() const noexcept {
    return stream_runtimes_.size();
}

bool http2_sans_io_session_engine::worker_running() const noexcept {
    return session_.worker_running();
}

void http2_sans_io_session_engine::request_drain() noexcept {
    if (!connection_.draining()) {
        connection_.begin_drain();
    }
    wake_writer();
    finish_drain_if_ready();
}

void http2_sans_io_session_engine::finish_drain_if_ready() noexcept {
    if (!drain_finished_ && connection_.draining() && stream_runtimes_.size() == 0 &&
        active_handler_tasks_ == 0 && !connection_.wants_write() && !output_write_in_flight_) {
        drain_finished_ = true;
        std::error_code error;
        socket_.cancel(error);
    }
}

void http2_sans_io_session_engine::set_inactivity_phase() noexcept {
    session_.scanner_entry().set_phase(http2_sans_io_inactivity_phase(
        connection_.header_block_in_progress(), stream_runtimes_.size(),
        stream_runtimes_.tunnel_count() != 0));
}

void http2_sans_io_session_engine::remove_stream_runtime(std::uint32_t stream_id) noexcept {
    if (stream_runtimes_.remove(stream_id)) {
        set_inactivity_phase();
    }
}

void http2_sans_io_session_engine::touch_activity() noexcept {
    session_.scanner_entry().touch();
}

void http2_sans_io_session_engine::wake_writer() noexcept {
    output_budget_.wake();
    write_signal_.notify();
}

void http2_sans_io_session_engine::terminate(std::error_code error) noexcept {
    if (!termination_.terminate(error)) {
        return;
    }
    output_budget_.wake();
    std::error_code ignored;
    socket_.cancel(ignored);
    stream_runtimes_.for_each([](http2_sans_io_stream_runtime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->stop();
        }
    });
    write_signal_.notify();
}

void http2_sans_io_session_engine::reset_stream_no_throw(
    std::uint32_t stream_id, http2_error_code error) noexcept {
    try {
        (void)connection_.submit_reset(stream_id, error);
        if (auto* signal = stream_runtimes_.signal_for(stream_id)) {
            signal->stop();
        }
    } catch (...) {
        terminate(std::make_error_code(std::errc::not_enough_memory));
    }
    output_budget_.release_and_reconcile(stream_id, connection_);
    output_budget_.wake();
}

std::pmr::memory_resource* http2_sans_io_session_engine::worker_resource() const noexcept {
    return worker_.resource();
}

task<bool> http2_sans_io_session_engine::push_request(std::uint32_t associated_stream_id, http_push_request_view request) {
    auto* parent_value = stream_runtimes_.find(associated_stream_id);
    const auto* head = parent_value != nullptr ? parent_value->request_head() : nullptr;
    if (head == nullptr || (associated_stream_id & 1U) == 0) {
        co_return false;
    }
    const auto original = head->request_view();
    if (!http_ascii_equals_ignore_case(original.scheme_, request.scheme_) ||
        !http_authorities_equal(borrowed_text(original.authority_), borrowed_text(request.authority_),
            http_ascii_equals_ignore_case(original.scheme_, "https") ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    auto promised = connection_.submit_push_request(associated_stream_id, request);
    if ((promised.index() != 0)) {
        if (std::get<1>(promised) == http2_push_submit_error::invalid_request) {
            throw std::invalid_argument("invalid HTTP/2 push request");
        }
        co_return false;
    }
    auto& promised_event = std::get<0>(promised);
    const auto id = promised_event.stream_id();
    http2_sans_io_stream_runtime* runtime = nullptr;
    try {
        runtime = http2_select_stream_route(routes_, promised_event.request_view(), stream_runtimes_, id);
        if (runtime == nullptr || !runtime->hold_request_head(std::move(promised_event))) {
            throw std::logic_error("HTTP/2 push dispatch admission failed");
        }
    } catch (...) {
        reset_stream_no_throw(id, http2_error_code::cancel);
        remove_stream_runtime(id);
        wake_writer();
        throw;
    }
    runtime->bind_push_parent(associated_stream_id);
    if (!admit_stream(id)) {
        reset_stream_no_throw(id, http2_error_code::cancel);
        wake_writer();
        co_return false;
    }
    wake_writer();
    co_return true;
}

struct http2_sans_io_session_engine::tunnel_upgrade_operation final {
    using transport_type = http2_sans_io_tunnel_transport<asio::any_io_executor>;
    using session_type = http_tunnel_session<transport_type>;

    http2_sans_io_session_engine& engine_;
    std::uint32_t stream_id_;
    bool udp_;
    http2_streaming_request_body& streaming_body_;
    http2_sans_io_stream_signal& stream_signal_;
    const resolved_route& resolved_;
    const tunnel_route_endpoint& tunnel_endpoint_;
    const context_services& base_services_;
    std::optional<session_type>& tunnel_session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        auto head = context_access::streaming_head(context_value);
        if (udp_) {
            auto negotiated = prepare_http_connect_udp_response(std::move(head), http_protocol_version::http2);
            if ((negotiated.index() != 0)) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            head = std::move(std::get<0>(negotiated));
        }
        const auto committed = engine_.connection_.submit_connect_response_head(stream_id_, head);
        if (committed != http2_submit_status::accepted) {
            throw std::invalid_argument("HTTP/2 CONNECT response head rejected");
        }
        if (!engine_.stream_runtimes_.mark_tunnel(stream_id_)) {
            std::terminate();
        }
        engine_.set_inactivity_phase();
        context_access::commit_upgrade(context_value);
        engine_.wake_writer();
        tunnel_session_.emplace(
            transport_type(engine_.connection_, stream_id_, streaming_body_.queue(), stream_signal_,
                engine_.write_signal_, engine_.output_budget_, engine_.executor_),
            base_services_.worker(), *context_value.pool(), engine_.inbound_buffers_);
        co_await invoke_tunnel_handler(
            *tunnel_session_, engine_.session_.scanner_entry(), resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_tunnel_session(session_value, std::move(failure),
            engine_.session_.options().connection_failure_, engine_.remote_address_,
            engine_.session_.scanner_entry(), tunnel_endpoint_.config().peer_transport_fin_timeout_);
    }
};

struct http2_sans_io_session_engine::websocket_upgrade_operation final {
    using transport_type = http2_sans_io_tunnel_transport<asio::any_io_executor>;
    using session_type = server_websocket_connection<transport_type>;

    http2_sans_io_session_engine& engine_;
    std::uint32_t stream_id_;
    const http_request& request_;
    const websocket_handshake_validation_result& handshake_validation_;
    http2_streaming_request_body& streaming_body_;
    http2_sans_io_stream_signal& stream_signal_;
    const resolved_route& resolved_;
    const websocket_route_endpoint& websocket_endpoint_;
    const context_services& base_services_;
    std::optional<session_type>& websocket_session_;

    [[nodiscard]] task<void> operator()(context& context_value) {
        const auto& options = engine_.session_.options();
        const auto response_headers_value = websocket_response_headers(context_value);
        const auto handshake_result = engine_.connection_.submit_websocket_handshake(stream_id_, request_,
            handshake_validation_,
            {.supported_subprotocols_ = websocket_endpoint_.subprotocols(),
                .response_headers_ = response_headers_value,
                .deflate_ = websocket_endpoint_.deflate()});
        const auto* submitted_handshake = std::get_if<http2_websocket_negotiation>(&handshake_result);
        if (submitted_handshake == nullptr) {
            // The handshake is not started, so stream dispatch still
            // answers this pending CONNECT with an error response.
            throw std::invalid_argument("HTTP/2 WebSocket handshake rejected");
        }
        if (!engine_.stream_runtimes_.mark_tunnel(stream_id_)) {
            std::terminate();
        }
        engine_.set_inactivity_phase();
        context_access::commit_upgrade(context_value);
        engine_.wake_writer();
        websocket_session_.emplace(
            transport_type(engine_.connection_, stream_id_, streaming_body_.queue(), stream_signal_,
                engine_.write_signal_, engine_.output_budget_, engine_.executor_),
            base_services_.worker(), engine_.session_.scanner_entry(), websocket_endpoint_.lifecycle(),
            protocol_byte_limit::limited(options.server_.max_websocket_message_bytes_),
            &engine_.inbound_buffers_, std::string_view{},
            websocket_connection_options{.compression_ = submitted_handshake->compression(),
                .compression_level_ = websocket_endpoint_.deflate().compression_level_,
                .mem_level_ = websocket_endpoint_.deflate().mem_level_,
                .shared_deflate_ = &options.codecs_->websocket_deflate()});
        co_await invoke_websocket_handler(
            *websocket_session_, engine_.session_.scanner_entry(), resolved_.stream_handler(), context_value);
    }

    [[nodiscard]] task<void> operator()(session_type& session_value, std::exception_ptr failure) const {
        return finish_websocket_session(session_value, std::move(failure),
            engine_.session_.options().connection_failure_, engine_.remote_address_);
    }
};

task<void> http2_sans_io_session_engine::dispatch_one_inner(std::uint32_t stream_id) {
    const auto request_start = std::chrono::steady_clock::now();
    const auto& options = session_.options();
    const auto base_services = session_.services().with_inbound_buffer_pool(inbound_buffers_);

    std::array<std::byte, http2_request_arena_stack_bytes> arena_block;
    std::optional<request_memory> request_memory_storage;
    request_memory& request_memory_value =
        emplace_http2_request_memory(request_memory_storage, worker_, std::span<std::byte>(arena_block));
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* request_head = stream_runtime != nullptr ? stream_runtime->request_head() : nullptr;
    if (request_head == nullptr) {
        co_return;
    }
    auto* selected_route = stream_runtime->selected_route();
    if (selected_route == nullptr) {
        reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        wake_writer();
        co_return;
    }
    auto& request_body = selected_route->body();
    auto* streaming_body = request_body.streaming();
    const auto* buffered_body = request_body.buffered();
    auto* stream_signal = stream_runtime->signal();
    if (stream_signal == nullptr) {
        reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        wake_writer();
        co_return;
    }
    http_request request = request_head->build_request(request_memory_value.resource(),
        buffered_body == nullptr ? std::string_view{} : buffered_body->bytes());
    http_response response({.resource_ = request_memory_value.resource()});
    struct response_output_target {
        ::ruvia::http2_connection& connection_;
        worker_signal& writer_;
        std::uint32_t stream_id_;
    } output_target{connection_, write_signal_, stream_id};
    http_interim_response_output interim_output(worker_resource(), &output_target, [](void* raw, const http_interim_response_head& head) -> task<void> {
        auto& target = *static_cast<response_output_target*>(raw);
        const auto status = target.connection_.submit_interim_response_head(target.stream_id_, head);
        if (status != http2_submit_status::accepted) {
            throw std::invalid_argument("HTTP/2 interim response rejected");
        }
        target.writer_.notify();
        co_return;
    });
    http_connection_advertisement_output advertisements(worker_resource(), &output_target, [](void* raw, std::span<const std::string_view> origins) -> task<void> {
            auto& target = *static_cast<response_output_target*>(raw);
            if (target.connection_.submit_origin_advertisement(origins) != http2_submit_status::accepted) {
                throw std::invalid_argument("HTTP/2 ORIGIN advertisement rejected");
            }
            target.writer_.notify();
            co_return; }, [](void* raw, std::string_view value) -> task<void> {
            auto& target = *static_cast<response_output_target*>(raw);
            if (target.connection_.submit_alternative_service_advertisement(target.stream_id_, {}, value) != http2_submit_status::accepted) {
                throw std::invalid_argument("HTTP/2 ALTSVC advertisement rejected");
            }
            target.writer_.notify();
            co_return; });
    struct push_target {
        http2_sans_io_session_engine& owner_;
        std::uint32_t stream_id_;
    } push_target_value{*this, stream_id};
    http_push_output push_output(worker_resource(), &push_target_value, [](void* raw, http_push_request_view promised_request) -> task<bool> {
        auto& target = *static_cast<push_target*>(raw);
        co_return co_await target.owner_.push_request(target.stream_id_, promised_request);
    });
    const auto& resolution = selected_route->resolution();
    const auto expectation_plan = request_head->expectation_plan(http_unsupported_expectation_policy::reject);
    auto admission_services = base_services.with_request_channels({
        .interim_output_ = &interim_output,
        .connection_advertisements_ = &advertisements,
        .push_output_ = &push_output,
        .priority_update_ = &stream_runtime->priority_update(),
    });
    admission_services.bind_request_trailers(stream_runtime->trailers());
    auto admission = admit_http_request(
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
            .resource_ = request_memory_value.resource(),
            .expectation_ = &expectation_plan,
            .start_ = request_start,
        },
        admission_policy_, admission_services);
    const auto& request_services = admission.services();
    const auto* resolved = admission.resolved();

    std::optional<body_reader_binding<http2_sans_io_request_body_reader>> body_reader_storage;
    if (streaming_body != nullptr && !request_head->snapshot().connect_pending_) {
        const bool continue_on_read = expectation_plan.send_continue() != nullptr && resolved != nullptr;
        body_reader_storage.emplace(connection_, stream_id, streaming_body->queue(), *stream_signal, write_signal_,
            continue_on_read ? &interim_output : nullptr);
    }
    auto dispatch_services = request_services;
    if (body_reader_storage) {
        dispatch_services.bind_streaming_request_body(body_reader_storage->facade());
    }

    switch (admission.endpoint()) {
        case http_request_endpoint::rejection:
        case http_request_endpoint::not_acceptable:
            admission.tail().disable_response_coding();
            response = co_await respond_http_request_rejection(
                routes_, request, request_memory_value, *admission.rejection(), request_services);
            break;
        case http_request_endpoint::tunnel: {
            if (streaming_body == nullptr || !request_head->snapshot().connect_pending_) {
                reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                wake_writer();
                co_return;
            }
            const auto& tunnel_endpoint = *resolved->route().endpoint().tunnel();
            std::optional<tunnel_upgrade_operation::session_type> tunnel_session;
            tunnel_upgrade_operation upgrade{
                .engine_ = *this,
                .stream_id_ = stream_id,
                .udp_ = admission.udp_tunnel(),
                .streaming_body_ = *streaming_body,
                .stream_signal_ = *stream_signal,
                .resolved_ = *resolved,
                .tunnel_endpoint_ = tunnel_endpoint,
                .base_services_ = base_services,
                .tunnel_session_ = tunnel_session,
            };
            auto outcome = co_await run_http_upgrade_route(
                routes_, request, *resolved, request_memory_value, dispatch_services, tunnel_session, upgrade, upgrade);
            auto* buffered = outcome.response();
            if (buffered == nullptr) {
                co_return;
            }
            response = std::move(*buffered);
            break;
        }
        case http_request_endpoint::websocket: {
            const auto& websocket_endpoint = *resolved->route().endpoint().websocket();
            const auto handshake_validation = ruvia::validate_http2_websocket_handshake(
                connection_, stream_id, request, websocket_endpoint.handshake_validation());
            if (handshake_validation.accepted() == nullptr) {
                const auto* failure = handshake_validation.failure();
                if (failure == nullptr) {
                    throw std::logic_error("HTTP/2 WebSocket validation returned no outcome");
                }
                response = co_await routes_.handle_error(request, request_memory_value,
                    copy_http_protocol_error_info(request_memory_value.resource(), failure->protocol_error()),
                    request_services);
                failure->apply_required_response_headers(response);
                break;
            }
            if (streaming_body == nullptr) {
                reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                wake_writer();
                co_return;
            }
            std::optional<websocket_upgrade_operation::session_type> websocket_session;
            websocket_upgrade_operation upgrade{
                .engine_ = *this,
                .stream_id_ = stream_id,
                .request_ = request,
                .handshake_validation_ = handshake_validation,
                .streaming_body_ = *streaming_body,
                .stream_signal_ = *stream_signal,
                .resolved_ = *resolved,
                .websocket_endpoint_ = websocket_endpoint,
                .base_services_ = base_services,
                .websocket_session_ = websocket_session,
            };
            auto outcome = co_await run_http_upgrade_route(routes_, request, *resolved,
                request_memory_value, dispatch_services, websocket_session, upgrade, upgrade);
            auto* buffered = outcome.response();
            if (buffered == nullptr) {
                co_return;
            }
            response = std::move(*buffered);
            break;
        }
        case http_request_endpoint::response_stream: {
            http2_sans_io_response_stream_sink sink_value(connection_, stream_id,
                write_signal_, *stream_signal, output_budget_,
                worker_resource(), request.known_method(), *options.codecs_, *admission.tail().coding_policy().selection(),
                admission.coding_availability(), options.cors_ ? &*options.cors_ : nullptr);
            auto result_value = co_await dispatch_response_stream_with(sink_value, routes_, request, *resolved,
                request_memory_value, dispatch_services,
                [this, stream_id]() noexcept { return connection_.stream_aborted(stream_id); });
            if (result_value.peer_aborted_before_commit() != nullptr) {
                co_return;
            }
            if (const auto committed_status = result_value.committed_status()) {
                if (const auto* failed = result_value.failed_after_commit()) {
                    reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                    wake_writer();
                    options.connection_failure_.invoke(remote_address_, failed->exception());
                }
                admission.tail().record_access(options.access_log_, *committed_status);
                co_return;
            }
            if (auto* route_response = result_value.route_response()) {
                response = std::move(*route_response).take_response();
            } else if (auto* recovered = result_value.recovered_failure()) {
                response = std::move(*recovered).take_response();
            } else {
                throw std::logic_error("response stream dispatch returned no HTTP/2 terminal alternative");
            }
            break;
        }
        case http_request_endpoint::stream_body:
        case http_request_endpoint::buffered:
            co_await routes_.dispatch_buffered_response(request, resolution, request_memory_value,
                options.document_root_.binding(), dispatch_services, admission.static_file_mode(), response);
            break;
    }

    const auto first_preparation = admission.tail().prepare(response, options);
    const auto preparation = first_preparation.ready() != nullptr ? *first_preparation.ready()
                                                                  : co_await admission.tail().finish_prepare(first_preparation, response, options, routes_, request_memory_value, request_services);
    if (request_services.get_stop_token().stop_requested()) {
        reset_stream_no_throw(stream_id, http2_error_code::cancel);
        wake_writer();
        co_return;
    }
    const auto result_value = co_await buffered_response_writer_.write(stream_id, response, preparation.write_plan_);
    if (const auto committed_status = result_value.committed_status()) {
        admission.tail().record_access(options.access_log_, *committed_status);
    }
}

task<void> http2_sans_io_session_engine::dispatch_one(std::uint32_t stream_id) {
    try {
        co_await dispatch_one_inner(stream_id);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!connection_.stream_aborted(stream_id)) {
            reset_stream_no_throw(stream_id, http2_error_code::internal_error);
        }
        session_.options().connection_failure_.invoke(remote_address_, failure);
    }
    if (auto* runtime = stream_runtimes_.find(stream_id)) {
        if (auto* request_head = runtime->request_head()) {
            (void)connection_.release(std::move(*request_head));
        }
    }
    remove_stream_runtime(stream_id);
    wake_writer();
}

bool http2_sans_io_session_engine::admit_stream(std::uint32_t stream_id) {
    auto* signal = stream_runtimes_.begin_dispatch(stream_id, session_.services().worker());
    if (signal == nullptr) {
        return false;
    }
    bool counted = false;
    try {
        ++active_handler_tasks_;
        counted = true;
        asio::co_spawn(executor_, ruvia::as_awaitable(dispatch_one(stream_id)),
            asio::bind_allocator(
                asio::recycling_allocator<void>(), [this](std::exception_ptr exception) noexcept {
                    if (exception != nullptr) {
                        terminate(std::make_error_code(std::errc::operation_canceled));
                    }
                    --active_handler_tasks_;
                    finish_drain_if_ready();
                    if (active_handler_tasks_ == 0) {
                        handler_finished_.notify();
                    }
                    write_signal_.notify();
                }));
    } catch (...) {
        if (counted) {
            --active_handler_tasks_;
        }
        remove_stream_runtime(stream_id);
        return false;
    }
    return true;
}

void http2_sans_io_session_engine::reset_event_stream(
    std::uint32_t stream_id, http2_error_code error) noexcept {
    auto* signal = stream_runtimes_.signal_for(stream_id);
    reset_stream_no_throw(stream_id, error);
    if (signal != nullptr) {
        signal->wake();
    } else {
        remove_stream_runtime(stream_id);
    }
    wake_writer();
}

http2_sans_io_stream_runtime* http2_sans_io_session_engine::resolve_stream_route(
    const http2_request_head_event& message_head) {
    const auto request_view = message_head.request_view();
    if (request_view.method_.empty()) {
        return nullptr;
    }
    return http2_select_stream_route(routes_, request_view, stream_runtimes_, message_head.stream_id());
}

void http2_sans_io_session_engine::on_message_head(http2_request_head_event& message_head) {
    const auto& options = session_.options();
    const auto stream_id = message_head.stream_id();
    ++accepted_request_heads_;
    if (!connection_.draining() && options.server_.max_requests_per_connection_.has_value() &&
        accepted_request_heads_ >= *options.server_.max_requests_per_connection_) {
        connection_.begin_drain();
        wake_writer();
    }
    const auto expectation_plan =
        message_head.expectation_plan(http_unsupported_expectation_policy::reject);
    const auto snapshot = message_head.snapshot();
    auto* stream_runtime = resolve_stream_route(message_head);
    if (stream_runtime == nullptr || !stream_runtime->hold_request_head(std::move(message_head))) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    const bool connect_request = snapshot.connect_pending_;
    const auto* selected_route = stream_runtime->selected_route();
    const bool resolved_route = selected_route != nullptr && selected_route->resolution().resolved() != nullptr;
    const bool streaming_body = !connect_request && selected_route != nullptr &&
                                selected_route->body().streaming() != nullptr &&
                                snapshot.body_open_;
    // Same policy as HTTP/1: 100 Continue is sent when the server starts
    // consuming the content. A buffered route consumes it before dispatch, so
    // it is sent now; a streaming handler's first body read sends it; an
    // unresolved request (404, 405, document root) never reads the content
    // and answers with its final status alone (RFC 9110 §10.1.1).
    if (expectation_plan.send_continue() != nullptr && resolved_route && !connect_request &&
        selected_route->body().buffered() != nullptr) {
        const auto status = connection_.submit_interim_response_head(
            stream_id, http_interim_response_head(ruvia::http_status::continue_value));
        if (status == http2_submit_status::accepted) {
            wake_writer();
        } else {
            if (status != http2_submit_status::closed) {
                reset_event_stream(stream_id, http2_error_code::internal_error);
            } else {
                remove_stream_runtime(stream_id);
            }
            return;
        }
    }
    // An unresolved request is answered from its head alone; waiting for
    // content the client may withhold (Expect) would stall the stream.
    if (expectation_plan.rejection() != nullptr || connect_request || streaming_body || !resolved_route) {
        if (!admit_stream(stream_id)) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
        }
    }
}

void http2_sans_io_session_engine::on_body_chunk(http2_message_body_chunk_event& body_chunk) {
    const auto& options = session_.options();
    const auto stream_id = body_chunk.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    if (stream_runtime == nullptr) {
        return;
    }
    auto* selected_route = stream_runtime->selected_route();
    if (selected_route == nullptr) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    auto& request_body = selected_route->body();
    if (request_body.buffered() != nullptr && stream_runtime->dispatched()) {
        // A buffered body is dispatched before its end only when its
        // expectation was rejected or its route is unresolved: the handler
        // never receives that content and the request borrows the buffered
        // bytes as they were at dispatch. Appending would reallocate under
        // that borrowed view, so later DATA is discarded and only its
        // flow-control credit returned.
        (void)connection_.acknowledge(body_chunk.take_credit());
        wake_writer();
        return;
    }
    const auto total_limit =
        http_request_body_limit(selected_route->resolution(), request_body.mode(), admission_policy_);
    const auto stored = request_body.streaming() != nullptr
                            ? request_body.store(body_chunk.bytes(), total_limit,
                                  options.server_.max_buffered_body_bytes_, body_chunk.take_credit())
                            : request_body.store(
                                  body_chunk.bytes(), total_limit, options.server_.max_buffered_body_bytes_);
    if (stored.stored() == nullptr) {
        const bool known_rejection =
            stored.protocol_failure() != nullptr || stored.backlog_overflow() != nullptr;
        reset_event_stream(stream_id,
            known_rejection ? http2_error_code::cancel : http2_error_code::internal_error);
        return;
    }
    if (request_body.streaming() != nullptr) {
        auto* signal = stream_runtime->signal();
        if (signal == nullptr) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
            return;
        }
        signal->wake();
    } else {
        (void)connection_.acknowledge(body_chunk.take_credit());
        wake_writer();
    }
}

void http2_sans_io_session_engine::on_tunnel_data(http2_tunnel_data_event& tunnel_data) {
    const auto& options = session_.options();
    const auto stream_id = tunnel_data.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* signal = stream_runtime != nullptr ? stream_runtime->signal() : nullptr;
    if (stream_runtime == nullptr || signal == nullptr) {
        return;
    }
    auto* selected_route = stream_runtime->selected_route();
    auto* streaming_body =
        selected_route != nullptr ? selected_route->body().streaming() : nullptr;
    if (streaming_body == nullptr) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    if (!streaming_body->queue().enqueue_bounded(tunnel_data.bytes(),
            tunnel_data.take_credit(), options.server_.max_buffered_body_bytes_)) {
        reset_event_stream(stream_id, http2_error_code::cancel);
        return;
    }
    signal->wake();
    wake_writer();
}

void http2_sans_io_session_engine::on_tunnel_end(const http2_tunnel_end_event& tunnel_end) noexcept {
    if (auto* signal = stream_runtimes_.signal_for(tunnel_end.stream_id())) {
        signal->wake();
    }
}

void http2_sans_io_session_engine::on_message_end(const http2_message_end_event& message_end) {
    const auto stream_id = message_end.stream_id();
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    if (stream_runtime == nullptr) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
        return;
    }
    for (const auto& field : message_end.trailers()) {
        if (stream_runtime->trailers().append(field.name(), field.value()).index() != 0) {
            throw std::logic_error("HTTP/2 decoder published invalid request trailers");
        }
    }
    if (auto* selected_route = stream_runtime->selected_route()) {
        if (auto* streaming_body = selected_route->body().streaming()) {
            // Before admission: the handler may start inline and read at once.
            streaming_body->queue().mark_ended();
        }
    }
    if (auto* signal = stream_runtime->signal()) {
        signal->wake();
    } else if (!admit_stream(stream_id)) {
        reset_event_stream(stream_id, http2_error_code::internal_error);
    }
}

void http2_sans_io_session_engine::on_stream_closed(const http2_stream_closed_event& stream_closed) noexcept {
    const auto stream_id = stream_closed.stream_id();
    stream_runtimes_.for_each([this, stream_id](http2_sans_io_stream_runtime& child_value) {
        if (child_value.push_parent() == stream_id) {
            reset_stream_no_throw(child_value.stream_id(), http2_error_code::cancel);
            if (auto* child_signal = child_value.signal()) {
                child_signal->wake();
            }
        }
    });
    output_budget_.release_and_reconcile(stream_id, connection_);
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* signal = stream_runtime != nullptr ? stream_runtime->signal() : nullptr;
    if (signal != nullptr) {
        if (connection_.stream_aborted(stream_id)) {
            signal->stop();
        } else {
            signal->wake();
        }
    } else {
        remove_stream_runtime(stream_id);
    }
}

void http2_sans_io_session_engine::drain_events() {
    for (;;) {
        auto event = connection_.next_event();
        if (!event.has_value()) {
            break;
        }
        if (auto* request_head = event->request_head()) {
            on_message_head(*request_head);
        } else if (auto* body_chunk = event->message_body_chunk()) {
            on_body_chunk(*body_chunk);
        } else if (auto* tunnel_data = event->tunnel_data()) {
            on_tunnel_data(*tunnel_data);
        } else if (const auto* tunnel_end = event->tunnel_end()) {
            on_tunnel_end(*tunnel_end);
        } else if (const auto* message_end = event->message_end()) {
            on_message_end(*message_end);
        } else if (const auto* stream_closed = event->stream_closed()) {
            on_stream_closed(*stream_closed);
        } else if (const auto* update = event->priority_update(); update != nullptr && !update->push_) {
            if (auto* runtime = stream_runtimes_.find(static_cast<std::uint32_t>(update->element_id_))) {
                runtime->reprioritize(update->fields_.request_priority());
            }
        }
    }
    for (const auto stream_id : connection_.take_drained_data_streams()) {
        if (auto* signal = stream_runtimes_.signal_for(stream_id)) {
            signal->wake();
        }
    }
    output_budget_.wake();
}

http2_feed_result http2_sans_io_session_engine::feed_and_drain(std::string_view bytes_value) {
    for (;;) {
        const auto result_value = connection_.feed(bytes_value);
        drain_events();
        if (result_value != http2_feed_result::events_pending) {
            return result_value;
        }
    }
}

task<void> http2_sans_io_session_engine::finish() {
    lifecycle_.begin_stopping();
    wake_writer();
    while (active_handler_tasks_ != 0) {
        co_await handler_finished_.wait();
    }
    wake_writer();
    while (lifecycle_.writer_join_pending()) {
        co_await writer_finished_.wait();
    }
    lifecycle_.rethrow_writer_failure();
}

}  // namespace ruvia::detail
