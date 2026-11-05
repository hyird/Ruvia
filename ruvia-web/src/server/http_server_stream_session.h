#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <openssl/ssl.h>

#include "ruvia/core/async.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http2_framing.h"

#include "context/context_services.h"
#include "http/http_protocol_error_info.h"
#include "http2/http2_sans_io_session.h"
#include "http2/http2_sans_io_session_context.h"
#include "http2/http2_server_session_entry.h"
#include "server/http1_interim_response_sink.h"
#include "server/http1_request_sequence.h"
#include "server/http1_response_output.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"
#include "server/http_response_writer.h"
#include "server/http_server_auto_https.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_connection_guards.h"
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
[[nodiscard]] inline std::optional<http_request_body_failure> content_length_limit_failure(
    const http1_request_body_plan& body_plan, protocol_byte_limit limit) noexcept {
    const auto* known_length = body_plan.known_length();
    return known_length == nullptr ? std::nullopt
                                   : http_request_body_size_failure(known_length->content_length(), limit);
}
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

// How a failed request-head read closes. A clean peer end (FIN, or TLS
// close_notify), or a timeout or drain that the scanner signalled by
// cancelling the read, ends a connection that owes no response: TLS answers
// with close_notify. Any other failure closes at once.
[[nodiscard]] inline http_connection_close http1_request_head_read_close(
    const std::error_code& error, const connection_scanner::entry& scanner_entry) noexcept {
    return error == asio::error::eof || scanner_entry.close_requested() ? http_connection_close::notify
                                                                        : http_connection_close::abort;
}

// A drain cancels a request-head read even when the request has already
// arrived: the reactor cancels the pending read before performing it. As
// Pingora's biased select reads ready input before it observes shutdown, the
// read is then issued once more when input is already waiting -- in the
// socket, or decrypted in the TLS engine. The close deadline the drain set
// still bounds that read.
template <typename stream_type>
[[nodiscard]] bool input_arrived_before_drain(stream_type& stream, const std::error_code& error,
    const connection_scanner::entry& scanner_entry, http_server_worker_state worker_state) noexcept {
    if (error != asio::error::operation_aborted || !scanner_entry.close_requested() ||
        http_server_worker_running(worker_state)) {
        return false;
    }
    std::error_code available_error;
    if (stream.lowest_layer().available(available_error) != 0 && !available_error) {
        return true;
    }
    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>) {
        return false;
    } else {
        return SSL_pending(stream.native_handle()) > 0;
    }
}

// What every request of one connection shares: the resident state run_stream
// keeps for the connection's whole life. serve_requests borrows it while the
// connection is active.
template <typename stream_type>
struct http1_connection_session final {
    http_server_session_config& listener_;
    stream_type& stream_;
    connection_scanner::entry& scanner_entry_;
    // The connection's route services, bound to its inbound buffer budget.
    context_services& route_services_;
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
    std::optional<http1_response_write_start>& started_write, bool coalesce_for_tls = false) {
    const auto held_output_bytes = response_head.view().size();
    try {
        started_write.emplace(start_response_write(response_head, response, planned_response,
            next_request_buffered ? pipelined_output_batch_bytes : 0, coalesce_for_tls));
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
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, socket);
    const auto admission_policy = http_request_admission_policy::server(options_);
    const response_file_io file_output{
        .worker_ = worker_,
        .stop_ = stop_token_,
        .timeout_ = options_.server_.write_timeout_,
    };
    inbound_buffer_resource connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    base_route_services = base_route_services.with_inbound_buffer_pool(connection_buffers);
    base_route_services.bind_routes(routes_);
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
        // A draining worker starts no further read. Bytes a read already
        // returned are parsed first: a request head they complete is served
        // with keep-alive off (next_response_close_policy), as Pingora serves
        // already-buffered requests during shutdown; an incomplete one ends at
        // the next read in serve_requests.
        if (!http_server_worker_running(state_)) {
            co_return http_connection_close::notify;
        }
        // Idle wait for the next keep-alive request uses idle_timeout;
        // the connection's first request uses request_header_timeout.
        scanner_entry.read_request_head(session.served_keepalive_request_
                                            ? connection_scanner::phase::idle
                                            : connection_scanner::phase::reading_initial);
        std::error_code idle_ec;
        std::size_t idle_bytes = 0;
        for (bool reissued = false;;) {
            auto idle_completion = co_await ruvia::async_asio<std::size_t>(
                [&stream, &idle_read_buffer](auto handler) mutable {
                    stream.async_read_some(asio::buffer(idle_read_buffer), std::move(handler));
                });
            idle_ec = idle_completion.error_code();
            idle_bytes = idle_completion.result();
            if (!idle_ec || reissued || !input_arrived_before_drain(stream, idle_ec, scanner_entry, state_)) {
                break;
            }
            reissued = true;
        }
        // A clean peer end, timeout or drain is answered as in the request read loop.
        if (idle_ec) {
            co_return http1_request_head_read_close(idle_ec, scanner_entry);
        }
        work_set = work_sets_.acquire();
        static_assert(idle_resident_read_bytes <= initial_read_buffer_bytes);
        work_set->read_buffer_.assign(std::string_view(idle_read_buffer.data(), idle_bytes));
        if (idle_bytes == idle_read_buffer.size()) {
            if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>) {
                if (stream.non_blocking()) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (!read_space.empty()) {
                        std::error_code extra_ec;
                        const auto extra_bytes = stream.read_some(asio::buffer(read_space), extra_ec);
                        if (!extra_ec && extra_bytes != 0) {
                            work_set->read_buffer_.commit_read(extra_bytes);
                        }
                    }
                }
            } else {
                for (int pending = SSL_pending(stream.native_handle()); pending > 0;
                    pending = SSL_pending(stream.native_handle())) {
                    const auto read_space = work_set->read_buffer_.prepare_read();
                    if (read_space.empty()) {
                        break;
                    }
                    std::size_t extra_bytes = 0;
                    const auto to_read = (std::min)(read_space.size(), static_cast<std::size_t>(pending));
                    if (SSL_read_ex(stream.native_handle(), read_space.data(), to_read, &extra_bytes) != 1 ||
                        extra_bytes == 0) {
                        break;
                    }
                    work_set->read_buffer_.commit_read(extra_bytes);
                }
            }
        }
        scanner_entry.touch();
        const auto served = co_await serve_requests(session, *work_set);
        if (const auto* close_mode = std::get_if<http_connection_close>(&served)) {
            co_return *close_mode;
        }
        if (std::get<http1_serve_handoff>(served) == http1_serve_handoff::cleartext_http2) {
            // The session consumes the buffered input in its first feed, before
            // it suspends, and reads into its own buffer afterwards: the HTTP/1
            // working memory goes back to the pool for the session's lifetime.
            const std::pmr::string pending_input(work_set->read_buffer_.pending(), memory_.resource());
            work_sets_.release(std::exchange(work_set, nullptr));
            // The HTTP/1 request channels pointed into serve_requests' frame.
            auto http2_services = base_route_services;
            http2_services.bind_request_channels({});
            co_return co_await run_http2_sans_io_session(stream, routes_, memory_,
                http2_sans_io_session_context(std::move(http2_services), options_, scanner_entry), pending_input);
        }
        work_sets_.release(std::exchange(work_set, nullptr));
    }
}

// Serves the connection's requests while it has input, and returns
// await_request once it holds no buffered bytes (run_stream then returns the
// work set and waits for the next request), cleartext_http2 on a complete
// prior-knowledge preface, or how the connection closes. One frame per active
// period: a pipelined burst stays in it.
template <typename stream_type>
task<std::variant<http1_serve_handoff, http_connection_close>> worker_connections::serve_requests(
    http1_connection_session<stream_type>& session, connection_work_set& work_set) {
    auto& stream = session.stream_;
    auto& listener_value = session.listener_;
    auto& scanner_entry = session.scanner_entry_;
    auto& connection_buffers = session.inbound_buffers_;
    auto& request_sequence = session.request_sequence_;
    const auto& routes_value = routes_;
    auto& base_route_services = session.route_services_;
    const auto& admission_policy = session.admission_policy_;
    const auto& file_output = session.file_output_;
    const auto remote_address = session.remote_address_;
    constexpr bool plain_tcp = std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>;
    auto& read_buffer = work_set.read_buffer_;
    auto& parsed_value = work_set.parsed_;
    auto& response_head = work_set.response_head_;
    auto& route_resolution_value = work_set.route_resolution_;
    for (;;) {
        // Pipelined responses held back for one coalesced write. Bound to the
        // work set, which keeps any held output until it is written.
        http1_response_output<stream_type> response_output(
            stream, response_head, work_set.deferred_access_, options_.access_log_, remote_address, scanner_entry);

        request_memory request_memory_value(memory_, work_set.arena_block_);
        // Work-set storage outlives this arena. Release its descriptor block
        // on every exit (including exceptions/cancellation) before the arena dies.
        struct request_header_lifetime final {
            http_request& request_;
            ~request_header_lifetime() {
                request_.reset();
            }
        } header_lifetime{parsed_value.request_};
        http_response response(request_memory_value.resource());
        // Holds the next pipelined request when a body reader over-read it into
        // its own buffer, which dies with the route, until the read buffer is
        // cleaned up below. Declared before request_completion, which borrows
        // it; a suffix still in the read buffer is compacted in place instead.
        std::pmr::string pipeline_stash(request_memory_value.resource());
        http1_interim_response_sink interim_sink(stream, parsed_value.request_, memory_.resource(), response_output);
        base_route_services.bind_interim_output(interim_sink.output());
        std::optional<http_request_admission> admission;
        std::optional<http1_session_request_completion> request_completion;
        // Rejections funnel through one co_await site after the read loop:
        // every co_await expression in a coroutine reserves its own frame
        // slots for the call's temporaries (GCC does not overlap them), so
        // inlining handle_error at each rejection site costs ~660 resident
        // bytes per site in every active connection's frame.
        std::optional<http_request_rejection> request_rejection;
        std::size_t header_search_offset = 0;
        // Request timing starts with the request's first bytes: the idle wait
        // above already has them; a pipelined request's arrived earlier.
        const bool record_access_timing = static_cast<bool>(options_.access_log_.callback_);
        auto request_start = record_access_timing ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        for (;;) {
            if constexpr (plain_tcp) {
                // The client preface must be the first bytes of the connection
                // (RFC 9113 section 3.4). auto_https reserves the cleartext
                // listener for HTTP/1 redirects, and a connection that already
                // completed an HTTP/1 exchange stays HTTP/1, so both refuse
                // prior-knowledge HTTP/2.
                const auto preface_probe =
                    read_buffer.empty() || session.served_keepalive_request_ || listener_value.redirect() != nullptr
                        ? http2_cleartext_preface_probe::http1
                        : probe_http2_cleartext_preface(read_buffer.pending());
                if (preface_probe == http2_cleartext_preface_probe::complete_preface) {
                    co_return http1_serve_handoff::cleartext_http2;
                }
                if (preface_probe == http2_cleartext_preface_probe::drop_connection) {
                    co_return http_connection_close::abort;
                }
                if (preface_probe == http2_cleartext_preface_probe::need_more_preface) {
                    if (!co_await read_more_cleartext_http2_preface(stream, scanner_entry, read_buffer)) {
                        co_return http_connection_close::abort;
                    }
                    continue;
                }
            }
            const auto buffer_view = read_buffer.pending();
            parsed_value.parse_head(buffer_view, header_search_offset, request_memory_value.resource());
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
                scanner_entry.set_phase(connection_scanner::phase::idle);
            }
            if (const auto* request_head = ready_head) {
                // Reset phase so request_header_timeout stops counting against dispatch
                // time. Body readers will set reading_payload on their own; upgraded
                // sessions enter long_lived; a response stream's sink times its own
                // writes; the buffered write path sets writing before responding.
                // Until one of those transitions, idle_timeout governs as the
                // deadman switch for hung handlers.
                scanner_entry.set_phase(connection_scanner::phase::idle);
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
                    admission_policy, base_route_services));

                if (const auto* redirect = listener_value.redirect()) {
                    if (admission->endpoint() == http_request_endpoint::rejection) {
                        request_rejection = *admission->rejection();
                        break;
                    }
                    if (auto_https_redirect_authority(parsed_value.request_).empty()) {
                        request_rejection = http_request_rejection::error(http_error_info(
                            {.status_ = ruvia::http_status::bad_request, .message_ = "missing Host header"}));
                        break;
                    }
                    response = make_auto_https_redirect_response(
                        parsed_value.request_, request_memory_value, redirect->https_port_);
                    if (http_response_needs_not_acceptable(
                            admission->tail().coding_policy(), parsed_value.request_, response)) {
                        request_rejection =
                            http_request_rejection::error(http_error_info(http_not_acceptable_coding_error()));
                        break;
                    }
                    request_completion.emplace(close_with_framework_response(response, parsed_value.connection_plan_));
                    scanner_entry.touch();
                    break;
                }
                if (const auto* rejection = admission->rejection()) {
                    request_rejection = *rejection;
                    break;
                }
                if (const auto body_failure =
                        content_length_limit_failure(parsed_value.body_plan_, admission->body_limit())) {
                    request_rejection = http_request_rejection::error(copy_http_protocol_error_info(
                        request_memory_value.resource(), http_request_body_protocol_error(*body_failure)));
                    break;
                }

                const auto* resolved = admission->resolved();
                if (resolved == nullptr) {
                    co_await routes_value.dispatch_buffered_response(parsed_value.request_,
                        route_resolution_value, request_memory_value, options_.document_root_.binding(),
                        admission->services(), admission->static_file_mode(), response);
                    // An unresolved request never reads its body, whether the
                    // shared Web dispatch selected a document-root file, 404,
                    // 405, or OPTIONS response.
                    request_completion.emplace(finalize_unread_body_response(
                        response, parsed_value, *request_head, buffer_view, request_sequence));
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
                        co_return endpoint_kind == http_request_endpoint::websocket&& stream.lowest_layer().is_open()
                            ? http_connection_close::linger
                            : http_connection_close::abort;
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
                    const bool has_body = parsed_value.body_plan_.requires_consumption();
                    http_lazy_buffered_body_route_state<stream_type> body_state;
                    if (has_body) {
                        const auto body_and_pipeline = http_body_and_pipeline(*request_head, buffer_view);

                        // The body reader/loader setup can throw (e.g. constructing a
                        // transfer-coding decoder for a bad Transfer-Encoding), so it
                        // stays guarded. The dispatch turns any handler or routing
                        // failure into a response, so it sits outside the guard; the
                        // one exception it lets through is an inbound_transport_error
                        // (an interim head or 100 Continue the peer never received),
                        // which ends the connection through the session's caller.
                        if (const auto body_setup_exception = try_prepare_http_lazy_buffered_body_route(
                                body_state, route_dispatch, admission->body_limit(), body_and_pipeline)) {
                            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                                body_setup_exception, parsed_value, routes_value, request_memory_value,
                                admission->services(), response));
                            break;
                        }
                    }

                    std::optional<context_services> body_route_services;
                    if (has_body) {
                        body_route_services.emplace(body_state.with_loader(admission->services()));
                    }
                    const auto& route_services =
                        body_route_services ? *body_route_services : admission->services();
                    (void)co_await routes_value.dispatch_resolved_route(
                        parsed_value.request_, request_memory_value, *resolved, route_services, nullptr, response);

                    if (has_body) {
                        request_completion.emplace(complete_successful_http_body_route(scanner_entry,
                            response, parsed_value.connection_plan_, request_sequence,
                            buffer_view.size(), pipeline_stash, body_state.body().reader()));
                    } else {
                        scanner_entry.touch();
                        request_completion.emplace(finalize_buffered_route_response(
                            response, parsed_value.connection_plan_, request_sequence, request_head->header_bytes()));
                    }
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
                request_rejection = http_request_rejection::error(
                    copy_http_protocol_error_info(request_memory_value.resource(), error));
                break;
            }

            header_search_offset = buffer_view.size();

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idle_timeout); once any header bytes are
            // buffered, or on the first request, request_header_timeout governs.
            // Either way no request is admitted yet: a draining worker starts
            // no read for one, and a drain that begins during the read cancels
            // it. Bytes the read already returned are still parsed above.
            if (!http_server_worker_running(state_)) {
                co_return http_connection_close::notify;
            }
            scanner_entry.read_request_head(read_buffer.empty() && session.served_keepalive_request_
                                                ? connection_scanner::phase::idle
                                                : connection_scanner::phase::reading_initial);
            const auto read_space = read_buffer.prepare_read();
            if (read_space.empty()) {
                // The buffer only fills at max_http_header_bytes, where
                // parse_head has already classified and rejected the head.
                throw std::logic_error("HTTP/1 request head filled the read buffer unrejected");
            }

            std::error_code ec;
            std::size_t bytes_read = 0;
            for (bool reissued = false;;) {
                auto read_completion =
                    co_await ruvia::async_asio<std::size_t>([&stream, read_space](auto handler) mutable {
                        stream.async_read_some(asio::buffer(read_space), std::move(handler));
                    });
                ec = read_completion.error_code();
                bytes_read = read_completion.result();
                if (!ec || reissued || !input_arrived_before_drain(stream, ec, scanner_entry, state_)) {
                    break;
                }
                reissued = true;
            }
            if (ec) {
                // A clean peer end, or a timeout or drain that cancelled the
                // read, is answered with this side's close alert; anything else
                // closes immediately, as does a stopping worker.
                co_return http1_request_head_read_close(ec, scanner_entry);
            }
            if (record_access_timing && read_buffer.empty()) {
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
        // A rate-limited request is well framed: like a 404 it keeps the
        // connection when its body is already in hand. Every other rejection
        // closes it.
        if (request_rejection) {
            if (admission) {
                admission->tail().disable_response_coding();
            }
            response = co_await respond_http_request_rejection(routes_value, parsed_value.request_,
                request_memory_value, *request_rejection,
                admission ? admission->services() : base_route_services);
            const auto* rejected_head = parsed_value.head_ready();
            request_completion.emplace(request_rejection->rate_limit() != nullptr && rejected_head != nullptr
                                           ? finalize_unread_body_response(response, parsed_value, *rejected_head,
                                                 read_buffer.pending(), request_sequence)
                                           : close_with_framework_response(response, parsed_value.connection_plan_));
        }

        if (!request_completion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        if (const auto* rejected = request_completion->rejected_final_response()) {
            request_completion.emplace(co_await complete_failed_http_route(scanner_entry,
                std::make_exception_ptr(rejected->exception()), parsed_value, routes_value,
                request_memory_value, admission->services(), response));
        }
        http_response_tail fallback_tail(parsed_value.request_,
            http_response_coding_policy::disabled(), remote_address, remote_address, request_start);
        auto& tail = admission ? admission->tail() : fallback_tail;
        auto connection_plan = request_completion->connection_plan();
        if (request_completion->buffered_response() != nullptr) {
            // Preparation may still run handler-side work (error recovery,
            // offloaded compression); it stays timed as idle. Only the socket
            // writes below run as `writing` (write_remaining() sets it).
            const auto& services = admission ? admission->services() : base_route_services;
            std::optional<http1_buffered_response_plan> planned_response;
            std::optional<http1_response_write_start> started_write;
            bool file_response = false;
            bool head_failure_recovered = false;
            std::optional<http1_file_response_body<stream_type>> file_body;
            for (;;) {
                const auto preparation = tail.prepare(response, options_, worker_);
                const auto prepared = preparation.ready() != nullptr
                                          ? *preparation.ready()
                                          : co_await tail.finish_prepare(preparation, response, options_,
                                                routes_value, request_memory_value, services);
                if (prepared.recovered_) {
                    connection_plan = http1_commit_final_response(response, connection_plan);
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
                auto head_failure = try_start_buffered_response_write(
                    response_head, response, *planned_response, next_request_buffered, started_write, !plain_tcp);
                if (head_failure == nullptr && file_response) {
                    // The first file chunk is read (and the file validated)
                    // before its head leaves, so a file that cannot be served
                    // still gets an error response.
                    file_body.emplace(response, file_output);
                    scanner_entry.set_phase(connection_scanner::phase::writing);
                    if (!(co_await file_body->start(*planned_response))) {
                        scanner_entry.set_phase(connection_scanner::phase::idle);
                        const auto& file_failure = *file_body->failure();
                        options_.connection_failure_.invoke(remote_address, file_failure.fault());
                        head_failure = file_failure.response_error();
                        file_body.reset();
                        response_head.reset();
                    }
                }
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
                    response_plan.body_plan().response_status());
                scanner_entry.set_phase(connection_scanner::phase::idle);
            } else {
                std::optional<http1_buffered_response_write_result> write_result;
                if (file_response) {
                    scanner_entry.set_phase(connection_scanner::phase::writing);
                    write_result.emplace(co_await file_body->finish(
                        stream, response_head, response_plan, write_start, scanner_entry));
                    response_head.reset();
                    if (const auto* file_failure = file_body->failure()) {
                        options_.connection_failure_.invoke(remote_address, file_failure->fault());
                    }
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
                scanner_entry.set_phase(connection_scanner::phase::idle);
                if (const auto committed_status = write_result->committed_status_) {
                    tail.record_access(options_.access_log_, *committed_status);
                }
                if (!write_result->completed_) {
                    co_return http_connection_close::abort;
                }
            }
        } else if (const auto* committed = request_completion->committed_stream()) {
            scanner_entry.set_phase(connection_scanner::phase::idle);
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
            co_return http_connection_close::linger;
        }
        read_buffer.complete_request(request_completion->buffer_completion());
        read_buffer.trim();
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        session.served_keepalive_request_ = true;
        if (read_buffer.empty()) {
            co_return http1_serve_handoff::await_request;
        }
    }
}

}  // namespace ruvia::detail
