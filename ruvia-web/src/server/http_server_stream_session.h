#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/http/http1_server_request_parser.h"

#include "context/context_services.h"
#include "http/http_protocol_error_info.h"
#include "http2/cleartext_upgrade.h"
#include "router/route_context.h"
#include "server/http1_interim_response_sink.h"
#include "server/http1_request_sequence.h"
#include "server/http1_response_output.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"
#include "server/http_response_writer.h"
#include "server/http_server_auto_https.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_connection_guards.h"
#include "server/http_server_request_state.h"
#include "server/http_server_response_state.h"
#include "server/http_server_response_stream_route.h"
#include "server/http_server_stream_body_route.h"
#include "server/http_server_tunnel_route.h"
#include "server/http_server_websocket_route.h"
#include "server/response_file_source.h"
#include "server/worker_connections.h"

// Member-template definitions for worker_connections, kept out of its header so the
// class stays readable. Included as an ordinary header: everything used here is
// included here.

namespace ruvia::detail {

// Whether a pipelined request can be served while earlier responses wait in
// the connection's output: its handling neither reads the socket (no request
// content, hence no 100 Continue) nor writes to it ahead of its own buffered
// response (no stream, tunnel or upgrade). Interim heads flush the output
// themselves.
[[nodiscard]] inline bool http1_request_follows_held_output(
    const http1_server_request_parse_state& parsed_value, const route_resolution& resolution) noexcept {
    if (parsed_value.body_plan_.requires_consumption()) {
        return false;
    }
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        return true;
    }
    const auto* buffered = resolved->route().endpoint().buffered();
    return buffered != nullptr && buffered->request_body_mode() == request_body_mode::buffered;
}

// What every request of one connection shares: the resident state run_stream
// keeps for the connection's whole life. serve_requests borrows it while the
// connection is active.
template <typename stream_type>
struct http1_connection_session final {
    http_server_session_config& listener_;
    stream_type& stream_;
    connection_scanner::entry_type& scanner_entry_;
    // The connection's route services, bound to its inbound buffer budget.
    const context_services& route_services_;
    inbound_buffer_resource& inbound_buffers_;
    http1_request_sequence& request_sequence_;
    const http_request_admission_policy& admission_policy_;
    const response_file_io& file_output_;
    std::string_view remote_address_;
    // nginx-aligned wait semantics. The first request on a connection, and any
    // partially-received request header, are bounded by request_header_timeout
    // (reading_initial). The idle wait for the *next* request on a reused
    // keep-alive connection -- no bytes of it received yet -- is bounded by
    // idle_timeout (idle), matching nginx keepalive_timeout. Flips true
    // once a request has been served so later idle waits use the keepalive
    // deadline instead of the header deadline.
    bool served_keepalive_request_{false};
};

[[nodiscard]] inline std::exception_ptr try_start_buffered_response_write(
    http_response_head_buffer& response_head, const http_response& response,
    const http1_buffered_response_plan& planned_response, bool next_request_buffered,
    std::optional<http1_response_write_start>& started_write) {
    const auto held_output_bytes = response_head.view().size();
    try {
        started_write.emplace(start_response_write(response_head, response, planned_response,
            next_request_buffered ? pipelined_output_batch_bytes : 0));
        return nullptr;
    } catch (...) {
        if (response_head.view().size() != held_output_bytes) {
            // Never flush an orphan head or append an error response as
            // its body. Propagate the real failure and abort this connection.
            throw;
        }
        return std::current_exception();
    }
}

template <typename stream_type>
task<http_connection_close> worker_connections::run_stream(http_server_session_config& listener_value, stream_type& stream,
    tcp_socket_type& socket, context_services base_route_services) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the
    // inbound buffer budget. Everything a request needs beyond that is held
    // only while the connection is actively serving and given back the moment
    // it goes idle, so an idle keep-alive connection holds none of it: the
    // heavy working set (read buffer, request arena, parse result, response
    // head) is borrowed from a per-worker pool, and the request state lives in
    // the frame of serve_requests.
    connection_scanner::entry_type scanner_entry;
    connection_scanner::guard_type scanner_guard(scanner_, scanner_entry, socket);
    const auto admission_policy = http_request_admission_policy::server(options_);
    const response_file_io file_output{
        .pool_ = options_.blocking_pool_,
        .worker_ = worker_,
        .stop_ = stop_token_,
        .resource_ = memory_.resource(),
    };
    inbound_buffer_resource connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    base_route_services = base_route_services.with_inbound_buffer_pool(connection_buffers);
    http1_request_sequence request_sequence(options_.server_.max_requests_per_connection_, state_);
    http1_connection_session<stream_type> session{
        .listener_ = listener_value,
        .stream_ = stream,
        .scanner_entry_ = scanner_entry,
        .route_services_ = base_route_services,
        .inbound_buffers_ = connection_buffers,
        .request_sequence_ = request_sequence,
        .admission_policy_ = admission_policy,
        .file_output_ = file_output,
        .remote_address_ = base_route_services.get_conn_info().remote().address(),
    };
    connection_work_set* work_set = nullptr;
    work_set_return work_set_return(work_sets_, work_set);
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, idle_resident_read_bytes> idle_read_buffer;

    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>) {
        enable_plain_tcp_sync_write(socket);
    }
    for (;;) {
        scanner_entry.set_phase(connection_scanner::phase_type::idle);

        // Borrow-on-use / return-on-idle for the whole work set: the
        // connection holds none while it waits for its next request, and reads
        // that request's first bytes into the small connection-resident buffer
        // instead, so an idle keep-alive connection occupies no work set
        // (memory scales with in-flight requests, not total connections).
        // Reading directly -- rather than a bufferless readiness wait -- costs
        // no extra reactor pass when data is already queued. serve_requests
        // returns only once no input is buffered: a pipelined burst stays in
        // it. TLS reads through the stream as well, so plaintext the engine
        // already decrypted completes the read at once; whatever exceeds the
        // landing pad stays in the engine for the next read.
        //
        // A draining worker admits no further request.
        if (!http_server_worker_running(state_)) {
            co_return http_connection_close::abort;
        }
        // Idle wait for the next keep-alive request uses idle_timeout;
        // the connection's first request uses request_header_timeout.
        scanner_entry.read_request_head(session.served_keepalive_request_
                                            ? connection_scanner::phase_type::idle
                                            : connection_scanner::phase_type::reading_initial);
        auto idle_completion = co_await ruvia::async_asio<std::size_t>(
            [&stream, &idle_read_buffer](auto handler) mutable {
                stream.async_read_some(asio::buffer(idle_read_buffer.data(), idle_read_buffer.size()), std::move(handler));
            });
        const auto idle_ec = idle_completion.error_code();
        // A clean peer end is answered as in the request read loop.
        if (idle_ec) {
            co_return idle_ec == asio::error::eof && !stop_token_.stop_requested()
                ? http_connection_close::peer_finished
                : http_connection_close::abort;
        }
        // Draining closed this connection while it awaited the request.
        if (!http_server_worker_running(state_)) {
            co_return http_connection_close::abort;
        }
        work_set = work_sets_.acquire();
        static_assert(idle_resident_read_bytes <= initial_read_buffer_bytes);
        work_set->read_buffer_.assign(std::string_view(idle_read_buffer.data(), idle_completion.result()));
        scanner_entry.touch();
        if (const auto close_mode = co_await serve_requests(session, *work_set)) {
            co_return *close_mode;
        }
        work_sets_.release(std::exchange(work_set, nullptr));
    }
}

// Serves the connection's requests while it has input, and returns nullopt
// once it holds no buffered bytes (run_stream then returns the work set and
// waits for the next request), or how the connection closes. One frame per
// active period: a pipelined burst stays in it.
template <typename stream_type>
task<std::optional<http_connection_close>> worker_connections::serve_requests(
    http1_connection_session<stream_type>& session, connection_work_set& work_set) {
    auto& stream = session.stream_;
    auto& listener_value = session.listener_;
    auto& scanner_entry = session.scanner_entry_;
    auto& connection_buffers = session.inbound_buffers_;
    auto& request_sequence = session.request_sequence_;
    const auto& routes_value = routes_;
    const auto& base_route_services = session.route_services_;
    const auto& admission_policy = session.admission_policy_;
    const auto& file_output = session.file_output_;
    const auto remote_address = session.remote_address_;
    constexpr bool plain_tcp = std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>;
    auto& read_buffer = work_set.read_buffer_;
    auto& parser = work_set.parser_;
    auto& parsed_value = work_set.parsed_;
    auto& response_head = work_set.response_head_;
    auto& route_resolution_value = work_set.route_resolution_;
    for (;;) {
        // Pipelined responses held back for one coalesced write. Bound to the
        // work set, which keeps any held output until it is written.
        http1_response_output<stream_type> response_output(
            stream, response_head, work_set.deferred_access_, options_.access_log_, scanner_entry);

        request_memory request_memory_value(memory_, work_set.arena_block_);
        // Work-set storage outlives this arena. Release its descriptor block
        // on every exit (including exceptions/cancellation) before the arena dies.
        struct request_header_lifetime final {
            http_request& request_;
            ~request_header_lifetime() {
                request_.reset();
            }
        } header_lifetime{parsed_value.request_};
        http_response response({.resource_ = request_memory_value.resource()});
        // Holds the next pipelined request when a body reader over-read it into
        // its own buffer, which dies with the route, until the read buffer is
        // cleaned up below. Declared before request_completion, which borrows
        // it; a suffix still in the read buffer is compacted in place instead.
        std::pmr::string pipeline_stash(request_memory_value.resource());
        http1_interim_response_sink interim_sink(stream, parsed_value.request_, memory_.resource(), response_output);
        const auto request_services =
            base_route_services.with_interim_output(interim_sink.output());
        std::optional<http_request_admission> admission;
        std::optional<http1_session_request_completion> request_completion;
        // Rejections that close the connection funnel through one co_await
        // site after the read loop: every co_await expression in a coroutine
        // reserves its own frame slots for the call's temporaries (GCC does
        // not overlap them), so inlining handle_error at each rejection site
        // costs ~660 resident bytes per site in every active connection's frame.
        std::optional<http_request_rejection> closing_rejection;
        std::size_t header_search_offset = 0;
        // Request timing starts with the request's first bytes: the idle wait
        // above already has them; a pipelined request's arrived earlier.
        auto request_start = std::chrono::steady_clock::now();
        for (;;) {
            if constexpr (plain_tcp) {
                if (!read_buffer.empty() && !session.served_keepalive_request_ && listener_value.redirect() == nullptr &&
                    probe_http2_cleartext_preface(read_buffer.pending()) != http2_cleartext_preface_probe::http1) {
                    const auto protocol = co_await dispatch_cleartext_http2_preface(http2_server_session_setup<stream_type>{
                                                                                        .stream_ = stream, .socket_ = stream, .memory_ = memory_, .routes_ = routes_, .options_ = options_, .scanner_entry_ = scanner_entry, .services_ = base_route_services, .worker_state_ = state_},
                        read_buffer, false);
                    if (protocol == cleartext_http2_dispatch_result::session_finished) {
                        co_return http_connection_close::abort;
                    }
                    if (protocol == cleartext_http2_dispatch_result::continue_read_loop) {
                        continue;
                    }
                }
            }
            const auto buffer_view = read_buffer.pending();
            parser.parse_head(buffer_view, parsed_value, header_search_offset, request_memory_value.resource());
            const auto* const ready_head = parsed_value.head_ready();
            if (ready_head != nullptr) {
                route_resolution_value = routes_value.resolve(parsed_value.request_);
            }
            // Held-back pipelined responses leave before anything that could
            // make the peer wait for them or reach the stream ahead of them:
            // a socket read, a rejection that closes, request content, or a
            // streamed, tunnelled or upgraded response.
            if (!response_output.empty() &&
                (ready_head == nullptr || !http1_request_follows_held_output(parsed_value, route_resolution_value))) {
                if (!response_output.start()) {
                    const auto completion = co_await response_output.async_write_remaining();
                    response_output.finish(completion.error_code(), completion.result());
                }
                if (response_output.complete().error_) {
                    co_return http_connection_close::abort;
                }
                scanner_entry.set_phase(connection_scanner::phase_type::idle);
            }
            if (const auto* request_head = ready_head) {
                // Reset phase so request_header_timeout stops counting against dispatch
                // time. Body readers will set reading_payload on their own; upgraded
                // sessions enter long_lived; a response stream's sink times its own
                // writes; the buffered write path sets writing before responding.
                // Until one of those transitions, idle_timeout governs as the
                // deadman switch for hung handlers.
                scanner_entry.set_phase(connection_scanner::phase_type::idle);
                const auto expectation_plan =
                    parsed_value.body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
                admission.emplace(admit_http_request(
                    {
                        .request_ = parsed_value.request_,
                        .resolution_ = route_resolution_value,
                        .coding_ = parsed_value.response_coding_selection(),
                        .resource_ = request_memory_value.resource(),
                        .expectation_ = &expectation_plan,
                        .start_ = request_start,
                    },
                    admission_policy, request_services));

                if (const auto* redirect = listener_value.redirect()) {
                    if (admission->endpoint() == http_request_endpoint::rejection) {
                        closing_rejection = *admission->rejection();
                        break;
                    }
                    if (auto_https_redirect_authority(parsed_value.request_).empty()) {
                        closing_rejection = http_request_rejection::error(http_error_info(
                            {.status_ = ruvia::http_status::bad_request, .message_ = "missing Host header"}));
                        break;
                    }
                    response = make_auto_https_redirect_response(
                        parsed_value.request_, request_memory_value, redirect->https_port_);
                    if (http_response_needs_not_acceptable(
                            admission->tail().coding_policy(), parsed_value.request_, response)) {
                        closing_rejection =
                            http_request_rejection::error(http_error_info(http_not_acceptable_coding_error()));
                        break;
                    }
                    request_completion.emplace(close_with_framework_response(response, parsed_value.connection_plan_));
                    scanner_entry.touch();
                    break;
                }
                if (const auto* rejection = admission->rejection()) {
                    closing_rejection = *rejection;
                    break;
                }
                if (const auto body_failure =
                        content_length_limit_failure(parsed_value.body_plan_, admission->body_limit())) {
                    closing_rejection = http_request_rejection::error(copy_http_protocol_error_info(
                        request_memory_value.resource(), body_failure->protocol_error()));
                    break;
                }

                const auto* resolved = admission->resolved();
                if (resolved == nullptr) {
                    co_await routes_value.dispatch_buffered_response(parsed_value.request_,
                        route_resolution_value, request_memory_value, options_.document_root_.binding(),
                        admission->services(), admission->static_file_mode(), response);
                    // An unresolved request never consumes its body, regardless
                    // of whether the shared Web dispatch selected a document-root
                    // file, 404, 405, or OPTIONS response.
                    auto connection_plan = apply_request_body_consumption(
                        parsed_value.connection_plan_, parsed_value.body_plan_.requires_consumption()
                                                           ? http1_request_body_consumption::incomplete
                                                           : http1_request_body_consumption::complete);
                    request_completion.emplace(finalize_buffered_route_response(
                        response, connection_plan, request_sequence, request_head->header_bytes()));
                    scanner_entry.touch();
                    break;
                }

                // One bundle of what every route dispatch below needs from
                // this session; each dispatcher adds only its own arguments.
                const http1_route_dispatch<stream_type> route_dispatch{
                    .stream_ = stream,
                    .memory_ = memory_,
                    .scanner_entry_ = scanner_entry,
                    .parsed_ = parsed_value,
                    .response_coding_ = *admission->tail().coding_policy().selection(),
                    .response_coding_availability_ = admission->coding_availability(),
                    .routes_ = routes_value,
                    .request_memory_ = request_memory_value,
                    .base_route_services_ = admission->services(),
                    .options_ = options_,
                    .response_ = response,
                    .request_sequence_ = request_sequence,
                    .inbound_buffer_pool_ = connection_buffers,
                };

                const auto endpoint_kind = admission->endpoint();
                if (endpoint_kind == http_request_endpoint::tunnel ||
                    endpoint_kind == http_request_endpoint::websocket) {
                    const auto pending = buffer_view.substr(request_head->header_bytes());
                    auto completion = endpoint_kind == http_request_endpoint::tunnel
                                          ? co_await dispatch_http_tunnel_route(route_dispatch, admission->tail(),
                                                *resolved, admission->udp_tunnel(), pending)
                                          : co_await dispatch_http_websocket_route(
                                                route_dispatch, admission->tail(), *resolved, pending);
                    if (!completion) {
                        co_return http_connection_close::abort;
                    }
                    request_completion.emplace(std::move(*completion));
                    break;
                }

                if (endpoint_kind == http_request_endpoint::response_stream) {
                    request_completion.emplace(co_await dispatch_http_response_stream_route(route_dispatch,
                        response_head, *request_head, *resolved, buffer_view, admission->body_limit(),
                        pipeline_stash));
                    // The stream serialized its own head into the (then empty)
                    // output buffer; it is not response output to keep.
                    response_head.reset();
                    break;
                }
                if (endpoint_kind == http_request_endpoint::stream_body) {
                    request_completion.emplace(co_await dispatch_http_stream_body_route(route_dispatch,
                        *request_head, route_resolution_value, admission->body_limit(), buffer_view,
                        pipeline_stash));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const auto body_and_pipeline = http_body_and_pipeline(*request_head, buffer_view);

                    // The body reader/loader setup can throw (e.g. constructing a
                    // transfer-coding decoder for a bad Transfer-Encoding), so it
                    // stays guarded. The dispatch turns any handler or routing
                    // failure into a response, so it sits outside the guard; the
                    // one exception it lets through is an inbound_transport_error
                    // (an interim head or 100 Continue the peer never received),
                    // which ends the connection through the session's caller.
                    http_lazy_buffered_body_route_state<stream_type> body_state;
                    if (const auto body_setup_exception = try_prepare_http_lazy_buffered_body_route(
                            body_state, route_dispatch, admission->body_limit(), body_and_pipeline)) {
                        request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                            body_setup_exception, parsed_value, routes_value, request_memory_value,
                            admission->services(), response));
                        break;
                    }

                    const auto route_services = body_state.with_loader(admission->services());
                    std::exception_ptr dispatch_exception;
                    try {
                        // Keep the context in this existing frame rather than
                        // allocating a dispatch coroutine for a matched route.
                        // It must die before body completion hands off the
                        // pipeline and before the request arena.
                        auto context_value = make_route_context(request_memory_value,
                            parsed_value.request_, *resolved, route_services.with_routes(routes_value));
                        std::exception_ptr exception;
                        try {
                            response = co_await routes_value.invoke_route(*resolved, context_value);
                        } catch (...) {
                            exception = std::current_exception();
                        }
                        if (exception != nullptr) {
                            response = co_await routes_value.finalize_failure(context_value, exception);
                        }
                    } catch (...) {
                        dispatch_exception = std::current_exception();
                    }
                    if (dispatch_exception != nullptr) {
                        // Context construction and failed error finalization
                        // retain the dispatcher's outer request-only fallback.
                        // Transport failures still escape to the session.
                        response = co_await routes_value.handle_exception(
                            parsed_value.request_, request_memory_value, dispatch_exception, route_services);
                    }

                    request_completion.emplace(complete_successful_http_body_route(scanner_entry,
                        response, parsed_value.connection_plan_, request_sequence,
                        buffer_view.size(), pipeline_stash, body_state));
                    break;
                }
            }

            if (const auto* failure = parsed_value.failure()) {
                if constexpr (plain_tcp) {
                    if (listener_value.redirect() == nullptr &&
                        ruvia::should_drop_invalid_cleartext_http1_input(buffer_view, failure->source())) {
                        co_return http_connection_close::abort;
                    }
                }
                const auto error = failure->protocol_error();
                closing_rejection = http_request_rejection::error(
                    copy_http_protocol_error_info(request_memory_value.resource(), error));
                break;
            }

            header_search_offset = buffer_view.size();

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idle_timeout); once any header bytes are
            // buffered, or on the first request, request_header_timeout governs.
            // Either way no request is admitted yet: a draining worker admits
            // none, and a drain that begins during the read closes it.
            if (!http_server_worker_running(state_)) {
                co_return http_connection_close::abort;
            }
            scanner_entry.read_request_head(read_buffer.empty() && session.served_keepalive_request_
                                                ? connection_scanner::phase_type::idle
                                                : connection_scanner::phase_type::reading_initial);
            const auto read_space = read_buffer.prepare_read();
            if (read_space.empty()) {
                // The buffer only fills at max_http_header_bytes, where
                // parse_head has already classified and rejected the head.
                throw std::logic_error("HTTP/1 request head filled the read buffer unrejected");
            }

            auto read_completion = co_await ruvia::async_asio<std::size_t>([&stream, read_space](auto handler) mutable {
                stream.async_read_some(asio::buffer(read_space.data(), read_space.size()), std::move(handler));
            });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec) {
                // A clean peer end (FIN, or TLS close_notify) is answered with
                // this side's close alert unless the worker is stopping, which
                // closes immediately; anything else closes immediately.
                co_return ec == asio::error::eof && !stop_token_.stop_requested()
                    ? http_connection_close::peer_finished
                    : http_connection_close::abort;
            }

            // A request head still incomplete when draining began is not
            // admitted, even if this read completed before the drain closed it.
            if (!http_server_worker_running(state_)) {
                co_return http_connection_close::abort;
            }
            if (read_buffer.empty()) {
                request_start = std::chrono::steady_clock::now();
            }
            read_buffer.commit_read(bytes_read);
            scanner_entry.touch();
        }

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch. A request rejected before admission (malformed
        // head) runs with the connection's services and no coding promise.
        // A rejection is a generated terminal error: it must not inherit the
        // negotiated coding promise, or a client that accepts no coding would
        // see it replaced by a 406 through a second error-handler call.
        if (closing_rejection) {
            if (admission) {
                admission->tail().disable_response_coding();
            }
            response = co_await respond_http_request_rejection(routes_value, parsed_value.request_,
                request_memory_value, *closing_rejection,
                admission ? admission->services() : request_services);
            request_completion.emplace(close_with_framework_response(response, parsed_value.connection_plan_));
        }

        if (!request_completion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        if (const auto* rejected = request_completion->rejected_final_response()) {
            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                std::make_exception_ptr(rejected->exception()), parsed_value, routes_value,
                request_memory_value, admission->services(), response));
        }
        http_response_tail fallback_tail(
            parsed_value.request_, http_response_coding_policy::disabled(), remote_address, request_start);
        auto& tail = admission ? admission->tail() : fallback_tail;
        auto connection_plan = request_completion->connection_plan();
        if (request_completion->buffered_response() != nullptr) {
            // Preparation may still run handler-side work (error recovery,
            // offloaded compression); it stays timed as idle. Only the socket
            // writes below run as `writing` (write_remaining() sets it).
            const auto& services = admission ? admission->services() : request_services;
            std::optional<http1_buffered_response_plan> planned_response;
            std::optional<http1_response_write_start> started_write;
            bool file_response = false;
            bool head_failure_recovered = false;
            for (;;) {
                const auto preparation = tail.prepare(response, options_);
                const auto prepared = preparation.ready() != nullptr
                                          ? *preparation.ready()
                                          : co_await tail.finish_prepare(preparation, response, options_,
                                                routes_value, request_memory_value, services);
                if (prepared.recovered_) {
                    connection_plan = require_http1_final_response_commit(response, connection_plan);
                    request_completion = request_completion->with_buffered_connection_plan(connection_plan);
                    connection_plan = request_completion->connection_plan();
                }
                planned_response.emplace(get_http1_buffered_response_plan(prepared.write_plan_, connection_plan));
                file_response = response.file_body() || response.has_multipart_file_body();
                if (file_response && !response_output.empty()) {
                    if (co_await response_output.flush()) {
                        co_return http_connection_close::abort;
                    }
                }
                // A response waits in the output only while the connection stays
                // open and already holds its next request, so the response that
                // ends a pipelined run carries the held ones in one write.
                const auto* const compaction = request_completion->buffer_completion().compaction();
                const bool next_request_buffered = connection_plan.disposition() == http1_close_policy::allow_reuse &&
                                                   compaction != nullptr &&
                                                   compaction->consumed_bytes() < read_buffer.pending().size() &&
                                                   http_server_worker_running(state_);
                // Head validation fails before touching output. Body coalescing can
                // fail after appending a head, leaving an incomplete buffered message.
                const auto head_failure = try_start_buffered_response_write(
                    response_head, response, *planned_response, next_request_buffered, started_write);
                if (head_failure == nullptr) {
                    break;
                }
                // An error response that cannot be serialized either is a real
                // fault of the error handler.
                if (head_failure_recovered) {
                    std::rethrow_exception(head_failure);
                }
                // Nothing of this response reached the wire. Like a stream whose
                // head fails, it becomes the generic error response that closes
                // the connection; the complete responses held before it go first.
                if (co_await response_output.flush()) {
                    co_return http_connection_close::abort;
                }
                tail.disable_response_coding();
                request_completion.emplace(co_await complete_failed_http_route(scanner_entry, head_failure,
                    parsed_value, routes_value, request_memory_value, services, response));
                connection_plan = request_completion->connection_plan();
                head_failure_recovered = true;
            }
            const auto& response_plan = *planned_response;
            const auto& write_start = *started_write;
            if (write_start.held_) {
                // Logged once the write that carries its head commits it.
                tail.defer_access(options_.access_log_, work_set.deferred_access_, write_start.head_bytes_,
                    response_plan.response_status());
                scanner_entry.set_phase(connection_scanner::phase_type::idle);
            } else {
                std::optional<http1_buffered_response_write_result> write_result;
                if (file_response) {
                    scanner_entry.set_phase(connection_scanner::phase_type::writing);
                    write_result.emplace(co_await finish_file_response_write(stream, response_head, response,
                        response_plan, write_start, file_output, scanner_entry));
                    response_head.reset();
                } else {
                    if (!response_output.start(write_start.body_,
                            connection_plan.disposition() == http1_close_policy::close_after_response &&
                                !stop_token_.stop_requested())) {
                        const auto completion = co_await response_output.async_write_remaining();
                        response_output.finish(completion.error_code(), completion.result());
                    }
                    const auto written = response_output.complete();
                    write_result.emplace(classify_http1_buffered_response_write(
                        response_plan, write_start.head_bytes_, written.error_, written.bytes_));
                }
                scanner_entry.set_phase(connection_scanner::phase_type::idle);
                if (const auto committed_status = write_result->committed_status_) {
                    tail.record_access(options_.access_log_, *committed_status);
                }
                if (!write_result->completed_) {
                    co_return http_connection_close::abort;
                }
            }
        } else if (const auto* committed = request_completion->committed_stream()) {
            scanner_entry.set_phase(connection_scanner::phase_type::idle);
            tail.record_access(options_.access_log_, committed->status());
            if (committed->truncated()) {
                co_return http_connection_close::abort;
            }
        } else {
            throw std::logic_error("HTTP/1 request completion has no wire alternative");
        }

        // A draining worker closes after the response, even one whose head
        // already allowed reuse because the drain began while it streamed.
        if (connection_plan.disposition() == http1_close_policy::close_after_response ||
            !http_server_worker_running(state_)) {
            co_return http_connection_close::after_response;
        }
        read_buffer.complete_request(request_completion->buffer_completion());
        read_buffer.trim();
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        session.served_keepalive_request_ = true;
        if (read_buffer.empty()) {
            co_return std::nullopt;
        }
        scanner_entry.set_phase(connection_scanner::phase_type::idle);
    }
}

}  // namespace ruvia::detail
