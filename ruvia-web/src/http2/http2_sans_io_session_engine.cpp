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
#include "http2/http2_sans_io_request_body.h"
#include "http2/http2_sans_io_response_stream_sink.h"
#include "http2/http2_sans_io_route_selection.h"
#include "http2/http2_sans_io_ws_transport.h"
#include "ratelimit/rate_limit_decision.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_server_access_log.h"
#include "server/request_body_limit.h"
#include "server/request_memory_arena.h"
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
      inbound_buffers_(session_.options().inbound_buffer_pool_ != nullptr
                           ? session_.options().inbound_buffer_pool_
                           : worker_value.resource(),
          session_.options().max_inbound_buffer_bytes_per_connection_),
      remote_address_(session_.services().get_conn_info().remote().address()),
      connection_(ruvia::http2_connection::server({.resource_ = worker_value.resource()})),
      write_signal_(session_.services().worker()),
      output_budget_(session_.services().worker()),
      handler_finished_(session_.services().worker()),
      writer_finished_(session_.services().worker()),
      stream_runtimes_(worker_value.resource(), termination_, &inbound_buffers_),
      buffered_response_writer_(connection_, stream_runtimes_, worker_value, write_signal_, output_budget_) {}

bool http2_sans_io_session_engine::wants_write() const noexcept {
    return connection_.wants_write();
}

void http2_sans_io_session_engine::take_output(std::pmr::string& output) {
    // Preserve assign() semantics when the caller reuses its scratch string. The
    // core batch operation only consumes output and invokes its observer after the
    // copy succeeds, so allocation failure leaves protocol bytes and budget intact.
    output.clear();
    const auto result_value = connection_.take_output_batch(http2_data_output_credit_bytes, output, [](void* context_value, std::uint32_t stream_id, std::size_t payload_bytes) noexcept { static_cast<http2_data_output_budget*>(context_value)->note_data_output(stream_id, payload_bytes); }, &output_budget_);
    if (result_value.status_ == http2_output_batch_status::unaligned) {
        // This driver exclusively consumes complete batches; an unaligned cursor
        // would mean another consumer violated that ownership contract.
        std::terminate();
    }
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
            signal->wake();
        }
    });
    write_signal_.notify();
}

void http2_sans_io_session_engine::reset_stream_no_throw(
    std::uint32_t stream_id, http2_error_code error) noexcept {
    try {
        (void)connection_.submit_reset(stream_id, error);
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
    const auto& original = head->request();
    if (!http_ascii_equals_ignore_case(original.scheme(), request.scheme_) ||
        !http_authorities_equal(borrowed_text(original.authority()), borrowed_text(request.authority_),
            original.scheme() == "https" ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    auto promised = connection_.submit_push_request(associated_stream_id, request);
    if ((promised.index() != 0)) {
        if (std::get<1>(promised) == http2_push_submit_error::invalid_request) {
            throw std::invalid_argument("invalid HTTP/2 push request");
        }
        co_return false;
    }
    const auto id = std::get<0>(promised).stream_id();
    try {
        const auto request_view = connection_.server_request_view(id);
        auto* runtime = request_view ? http2_select_stream_route(routes_, *request_view, stream_runtimes_, id) : nullptr;
        if (runtime == nullptr || !runtime->hold_request_head(std::move(std::get<0>(promised)))) {
            throw std::logic_error("HTTP/2 push dispatch admission failed");
        }
    } catch (...) {
        reset_stream_no_throw(id, http2_error_code::cancel);
        remove_stream_runtime(id);
        wake_writer();
        throw;
    }
    stream_runtimes_.find(id)->bind_push_parent(associated_stream_id);
    if (!admit_stream(id)) {
        reset_stream_no_throw(id, http2_error_code::cancel);
        wake_writer();
        co_return false;
    }
    wake_writer();
    co_return true;
}

task<void> http2_sans_io_session_engine::dispatch_one_inner(std::uint32_t stream_id) {
    const auto request_start = std::chrono::steady_clock::now();
    const auto& options = session_.options();
    auto& scanner_entry = session_.scanner_entry();
    const auto base_services = session_.services().with_inbound_buffer_pool(inbound_buffers_);

    std::array<std::byte, request_arena_stack_bytes> arena_block;
    std::optional<request_memory> request_memory_storage;
    request_memory& request_memory_value =
        emplace_request_memory(request_memory_storage, worker_, std::span<std::byte>(arena_block));
    auto* stream_runtime = stream_runtimes_.find(stream_id);
    auto* request_head = stream_runtime != nullptr ? stream_runtime->request_head() : nullptr;
    if (request_head == nullptr) {
        co_return;
    }
    const auto request_method = request_head->request().known_method();
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
    auto request_build = make_http2_server_request(connection_, stream_id, request_memory_value.resource(),
        buffered_body == nullptr ? std::string_view{} : buffered_body->bytes());
    if ((request_build.index() != 0)) {
        auto request = make_parsed_http_request(
            "GET", "/", {}, {}, request_memory_value.resource())
                           .first;
        auto response = co_await routes_.handle_error(request, request_memory_value,
            copy_http_protocol_error_info(request_memory_value.resource(), std::get<1>(request_build)),
            base_services);
        (void)co_await buffered_response_writer_.write(stream_id, response,
            plan_buffered_http_response_write(request_method, response));
        co_return;
    }

    http_request request = std::move(std::get<0>(request_build));
    http_response response({.resource_ = request_memory_value.resource()});
    // Request negotiation must retain precompressed static sidecars even when
    // this worker cannot create a runtime encoder. The capability is enforced
    // later by the selected response representation.
    const auto response_coding_negotiation = http_response_coding_for(request);
    auto response_coding_policy = http_response_coding_policy::disabled();
    if (const auto* selection = response_coding_negotiation.selected()) {
        response_coding_policy = http_response_coding_policy::selected(*selection);
    } else {
        // Buffered routes may still produce a representation-free 204/205/304.
        // Preserve the negotiation failure until the response status is known;
        // rejecting here would make those responses incorrectly become 406.
        response_coding_policy = http_response_coding_policy::no_acceptable_coding();
    }
    const auto response_coding_availability =
        options.compression_.has_value() ? http_response_coding_availability::identity_and_compression
                                         : http_response_coding_availability::identity_only;
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
    auto request_services = base_services.with_push_output(push_output).with_request_trailers(stream_runtime->trailers()).with_request_priority_update(stream_runtime->priority_update()).with_interim_output(interim_output).with_connection_advertisements(advertisements);
    do {
        const auto& resolution = selected_route->resolution();
        const auto* resolved = resolution.resolved();

        // Armed on the stream's own state so concurrent requests on one
        // connection each get their own clock. It happens before every
        // server-layer rejection below: custom on_error/middleware is a handler
        // too and must see the request stop token.
        const auto handler_deadline = effective_handler_deadline(
            options.deadline_ ? std::optional{options.deadline_->handler_} : std::nullopt,
            resolved != nullptr ? resolved->route().deadline_ms() : 0);
        if (handler_deadline > std::chrono::milliseconds::zero()) {
            selected_route->arm_deadline(
                base_services.worker(), base_services.get_stop_token(), handler_deadline);
            request_services = request_services.with_request_deadline(*selected_route->deadline());
        }

        const auto expectation_plan =
            request_head->expectation_plan(http_unsupported_expectation_policy::reject);
        if (const auto* rejection = expectation_plan.rejection()) {
            response = co_await routes_.handle_error(request, request_memory_value,
                copy_http_protocol_error_info(request_memory_value.resource(), rejection->protocol_error()),
                request_services);
            break;
        }

        // Resolved per request: one HTTP/2 connection multiplexes many, each
        // carrying its own forwarding headers.
        const auto client_address = base_services.resolve_conn_info(request).client().address();
        const auto app_rate_limit = decide_request_rate_limit(base_services.rate_limiter(), client_address);
        if (const auto* rejection = app_rate_limit.rejection()) {
            response = co_await routes_.handle_error(
                request, request_memory_value, rate_limit_rejection_error(), request_services);
            apply_rate_limit_rejection_headers(response, *rejection);
            break;
        }
        std::optional<body_reader_binding<http2_sans_io_request_body_reader>> body_reader_storage;
        if (streaming_body != nullptr && !request_head->snapshot().connect_pending_) {
            body_reader_storage.emplace(
                connection_, stream_id, streaming_body->queue(), *stream_signal, write_signal_);
        }
        auto dispatch_services = request_services;
        if (body_reader_storage) {
            dispatch_services =
                dispatch_services.with_streaming_request_body(body_reader_storage->facade());
        }

        const auto* websocket_endpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().get_websocket();
        const auto* response_stream_endpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().response_stream();
        if (response_coding_policy.negotiation_failed() && response_stream_endpoint != nullptr) {
            // Streaming routes commit before a buffered response status can be
            // inspected. websocket Extended CONNECT does not select an HTTP
            // response representation.
            response = co_await routes_.handle_error(request, request_memory_value,
                http_error_info({
                    .status_ = ruvia::http_status::not_acceptable,
                    .code_ = "not_acceptable",
                    .message_ = "no acceptable response content coding",
                }),
                request_services);
            break;
        }
        if (const auto* tunnel_endpoint = resolved == nullptr ? nullptr : resolved->route().endpoint().tunnel()) {
            const bool udp = tunnel_endpoint->protocol() == "connect-udp";
            if (udp && (validate_http_connect_udp_request(request).index() != 0)) {
                response = co_await routes_.handle_error(request, request_memory_value,
                    http_error_info({.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"}), request_services);
                break;
            }
            if (streaming_body == nullptr || !request_head->snapshot().connect_pending_) {
                reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                wake_writer();
                co_return;
            }
            using transport_type = http2_sans_io_tunnel_transport<asio::any_io_executor>;
            std::optional<http_tunnel_session<transport_type>> tunnel_session;
            auto establish_and_run = [&](context& context_value) -> task<void> {
                auto head = context_access::streaming_head(context_value);
                if (udp) {
                    auto negotiated = prepare_http_connect_udp_response(std::move(head), http_protocol_version::http2);
                    if ((negotiated.index() != 0)) {
                        throw std::invalid_argument("invalid CONNECT-UDP response metadata");
                    }
                    head = std::move(std::get<0>(negotiated));
                }
                const auto committed = connection_.submit_connect_response_head(stream_id, head);
                if (committed != http2_submit_status::accepted) {
                    throw std::invalid_argument("HTTP/2 CONNECT response head rejected");
                }
                if (!stream_runtimes_.mark_tunnel(stream_id)) {
                    std::terminate();
                }
                set_inactivity_phase();
                context_access::mark_tunnel_handshake_started(context_value);
                wake_writer();
                tunnel_session.emplace(transport_type(connection_, stream_id, streaming_body->queue(), *stream_signal,
                                           write_signal_, output_budget_, executor_),
                    base_services.worker(), *context_value.pool());
                co_await invoke_tunnel_handler(*tunnel_session, scanner_entry, tunnel_endpoint->handler(), context_value);
            };
            const auto terminal = make_callable_ref<void, context&>(establish_and_run);
            std::optional<http_response> buffered;
            std::exception_ptr exception;
            try {
                buffered = co_await routes_.dispatch_tunnel(request, *resolved, request_memory_value, terminal, dispatch_services);
            } catch (...) {
                exception = std::current_exception();
            }
            if (tunnel_session.has_value()) {
                co_await finish_tunnel_session(*tunnel_session, exception, options.connection_failure_, remote_address_, scanner_entry, tunnel_endpoint->config().peer_transport_fin_timeout_);
                co_return;
            }
            if (exception != nullptr) {
                std::rethrow_exception(exception);
            }
            if (!buffered.has_value()) {
                co_return;
            }
            response = std::move(*buffered);
            break;
        }
        if (websocket_endpoint != nullptr) {
            const auto handshake_validation = ruvia::validate_http2_websocket_handshake(
                connection_, stream_id, request);
            if (handshake_validation.accepted() != nullptr) {
                if (streaming_body == nullptr) {
                    reset_stream_no_throw(stream_id, http2_error_code::internal_error);
                    wake_writer();
                    co_return;
                }
                using ws_transport_type = http2_sans_io_ws_transport<asio::any_io_executor>;
                using ws_connection = websocket_connection<ws_transport_type>;
                std::optional<ws_connection> websocket_connection;
                auto upgrade_and_run = [&](context& context_value) -> task<void> {
                    const auto response_headers_value = websocket_response_headers(context_value);
                    const auto handshake_result = connection_.submit_websocket_handshake(
                        stream_id, request, handshake_validation,
                        {.supported_subprotocols_ = websocket_endpoint->subprotocols(),
                            .response_headers_ = response_headers_value});
                    const auto* submitted_handshake = handshake_result.submitted();
                    if (submitted_handshake == nullptr) {
                        // The handshake is not started, so stream dispatch still
                        // answers this pending CONNECT with an error response.
                        throw std::invalid_argument("HTTP/2 WebSocket handshake rejected");
                    }
                    if (!stream_runtimes_.mark_tunnel(stream_id)) {
                        std::terminate();
                    }
                    set_inactivity_phase();
                    context_access::mark_websocket_handshake_started(context_value);
                    wake_writer();
                    websocket_connection.emplace(
                        ws_transport_type(connection_, stream_id, streaming_body->queue(), *stream_signal,
                            write_signal_, output_budget_, executor_),
                        base_services.worker(), scanner_entry, websocket_endpoint->lifecycle(),
                        protocol_byte_limit::limited(options.max_websocket_message_bytes_),
                        &inbound_buffers_, std::string_view{},
                        submitted_handshake->compression(), websocket_endpoint->deflate().compression_level_);
                    co_await invoke_websocket_handler(
                        *websocket_connection, scanner_entry, websocket_endpoint->handler(), context_value);
                };
                const auto terminal = make_callable_ref<void, context&>(upgrade_and_run);
                std::optional<http_response> buffered;
                std::exception_ptr exception;
                try {
                    buffered = co_await routes_.dispatch_websocket(
                        request, *resolved, request_memory_value, terminal, dispatch_services);
                } catch (...) {
                    exception = std::current_exception();
                }
                if (websocket_connection.has_value()) {
                    co_await finish_websocket_session(
                        *websocket_connection, exception, options.connection_failure_, remote_address_);
                    co_return;
                }
                if (exception != nullptr) {
                    std::rethrow_exception(exception);
                }
                if (!buffered.has_value()) {
                    co_return;
                }
                response = std::move(*buffered);
                break;
            }
            const auto* failure = handshake_validation.failure();
            if (failure == nullptr) {
                throw std::logic_error("HTTP/2 WebSocket validation returned no outcome");
            }
            response = co_await routes_.handle_error(request, request_memory_value,
                copy_http_protocol_error_info(request_memory_value.resource(), failure->protocol_error()),
                request_services);
            failure->apply_required_response_headers(response);
        } else if (response_stream_endpoint != nullptr) {
            http2_sans_io_response_stream_sink sink_value(connection_, stream_id,
                response_stream_endpoint->kind(), write_signal_, *stream_signal, output_budget_,
                worker_resource(),
                request.known_method(), *response_coding_policy.selection(),
                response_coding_availability);
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
                record_http_access(options.access_log_, request,
                    base_services.resolve_conn_info(request).client().address(), *committed_status,
                    request_start);
                co_return;
            }
            if (auto* route_response = result_value.route_response()) {
                response = std::move(*route_response).take_response();
            } else if (auto* recovered = result_value.recovered_failure()) {
                response = std::move(*recovered).take_response();
            } else {
                throw std::logic_error(
                    "response stream dispatch returned no HTTP/2 terminal alternative");
            }
        } else {
            response = co_await routes_.dispatch_buffered_response(request, resolution, request_memory_value,
                options.document_root_.binding(), dispatch_services,
                base_services.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                           : static_file_selection_mode::identity_only);
        }

    } while (false);

    const auto preparation = co_await prepare_application_response(
        request, response_coding_policy, response, options, routes_, request_memory_value, request_services);
    if (!preparation) {
        reset_stream_no_throw(stream_id, http2_error_code::cancel);
        wake_writer();
        co_return;
    }
    const auto write_plan = preparation->write_plan_;
    const auto result_value = co_await buffered_response_writer_.write(stream_id, response, write_plan);
    if (const auto committed_status = result_value.committed_status()) {
        record_http_access(options.access_log_, request,
            base_services.resolve_conn_info(request).client().address(), *committed_status,
            request_start);
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

void http2_sans_io_session_engine::drain_events() {
    const auto& options = session_.options();
    const auto reset_event_stream = [&](std::uint32_t stream_id, http2_error_code error) {
        auto* signal = stream_runtimes_.signal_for(stream_id);
        reset_stream_no_throw(stream_id, error);
        if (signal != nullptr) {
            signal->wake();
        } else {
            remove_stream_runtime(stream_id);
        }
        wake_writer();
    };
    const auto resolve_stream_route = [&](std::uint32_t stream_id) {
        const auto request_view = connection_.server_request_view(stream_id);
        if (!request_view.has_value()) {
            return static_cast<http2_sans_io_stream_runtime*>(nullptr);
        }
        return http2_select_stream_route(routes_, *request_view, stream_runtimes_, stream_id);
    };
    const auto on_message_head = [&](http2_request_head_event* message_head) {
        const auto stream_id = message_head->stream_id();
        ++accepted_request_heads_;
        if (!connection_.draining() && options.max_requests_per_connection_.has_value() &&
            accepted_request_heads_ >= *options.max_requests_per_connection_) {
            connection_.begin_drain();
            wake_writer();
        }
        const auto expectation_plan =
            message_head->expectation_plan(http_unsupported_expectation_policy::reject);
        const auto snapshot = message_head->snapshot();
        auto* stream_runtime = resolve_stream_route(stream_id);
        if (stream_runtime == nullptr || !stream_runtime->hold_request_head(std::move(*message_head))) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
            return;
        }
        if (expectation_plan.send_continue() != nullptr) {
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
        const bool connect_request = snapshot.connect_pending_;
        const auto* selected_route = stream_runtime->selected_route();
        const bool streaming_body = !connect_request && selected_route != nullptr &&
                                    selected_route->body().streaming() != nullptr &&
                                    snapshot.body_open_;
        if (expectation_plan.rejection() != nullptr || connect_request || streaming_body) {
            if (!admit_stream(stream_id)) {
                reset_event_stream(stream_id, http2_error_code::internal_error);
            }
        }
    };
    const auto on_body_chunk = [&](auto* body_chunk) {
        const auto stream_id = body_chunk->stream_id();
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
            // expectation was rejected: the handler never receives that content
            // and the request borrows the buffered bytes as they were at
            // dispatch. Appending would reallocate under that borrowed view, so
            // later DATA is discarded and only its flow-control credit returned.
            (void)connection_.acknowledge(std::move(body_chunk->take_credit()));
            wake_writer();
            return;
        }
        const auto* resolved_route_value = selected_route->resolution().resolved();
        const auto total_limit = request_body_byte_limit(request_body.mode(), options.max_stream_body_bytes_,
            options.max_buffered_body_bytes_,
            resolved_route_value != nullptr ? resolved_route_value->route().max_request_body_bytes() : 0);
        auto stored = [&] {
            if (request_body.streaming() != nullptr) {
                return request_body.store(body_chunk->bytes(), total_limit,
                    options.max_buffered_body_bytes_, std::move(body_chunk->take_credit()));
            }
            return request_body.store(
                body_chunk->bytes(), total_limit, options.max_buffered_body_bytes_);
        }();
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
            (void)connection_.acknowledge(std::move(body_chunk->take_credit()));
            wake_writer();
        }
    };
    const auto on_tunnel_data = [&](auto* tunnel_data) {
        const auto stream_id = tunnel_data->stream_id();
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
        if (!streaming_body->queue().enqueue_bounded(tunnel_data->bytes(),
                std::move(tunnel_data->take_credit()), options.max_buffered_body_bytes_)) {
            reset_event_stream(stream_id, http2_error_code::cancel);
            return;
        }
        signal->wake();
        wake_writer();
    };
    const auto on_tunnel_end = [&](const auto* tunnel_end) {
        if (auto* signal = stream_runtimes_.signal_for(tunnel_end->stream_id())) {
            signal->wake();
        }
    };
    const auto on_message_end = [&](const auto* message_end) {
        const auto stream_id = message_end->stream_id();
        auto* stream_runtime = stream_runtimes_.find(stream_id);
        if (stream_runtime == nullptr) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
            return;
        }
        for (const auto& field : message_end->trailers()) {
            if (stream_runtime->trailers().append(field.name(), field.value()).index() != 0) {
                throw std::logic_error("HTTP/2 decoder published invalid request trailers");
            }
        }
        if (auto* signal = stream_runtime->signal()) {
            signal->wake();
        } else if (!admit_stream(stream_id)) {
            reset_event_stream(stream_id, http2_error_code::internal_error);
        }
    };
    const auto on_stream_closed = [&](const auto* stream_closed) {
        const auto stream_id = stream_closed->stream_id();
        stream_runtimes_.for_each([&](http2_sans_io_stream_runtime& child_value) {
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
            signal->wake();
        } else {
            remove_stream_runtime(stream_id);
        }
    };

    for (;;) {
        auto event = connection_.next_event();
        if (!event.has_value()) {
            break;
        }
        if (auto* request_head = event->request_head()) {
            on_message_head(request_head);
        } else if (auto* body_chunk = event->message_body_chunk()) {
            on_body_chunk(body_chunk);
        } else if (auto* tunnel_data = event->tunnel_data()) {
            on_tunnel_data(tunnel_data);
        } else if (const auto* tunnel_end = event->tunnel_end()) {
            on_tunnel_end(tunnel_end);
        } else if (const auto* message_end = event->message_end()) {
            on_message_end(message_end);
        } else if (const auto* stream_closed = event->stream_closed()) {
            on_stream_closed(stream_closed);
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
