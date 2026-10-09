#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/http/http1_request_parser.h"

#include "context/context_services.h"
#include "http2/cleartext_upgrade.h"
#include "ratelimit/rate_limit_decision.h"
#include "server/http1_closing_rejection.h"
#include "server/http1_interim_response_sink.h"
#include "server/http1_request_sequence.h"
#include "server/http_buffered_response.h"
#include "server/http_response_writer.h"
#include "server/http_server_access_log.h"
#include "server/http_server_auto_https.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_connection_guards.h"
#include "server/http_server_idle_work_set.h"
#include "server/http_server_request_state.h"
#include "server/http_server_response_state.h"
#include "server/http_server_response_stream_route.h"
#include "server/http_server_stream_body_route.h"
#include "server/http_server_tunnel_route.h"
#include "server/http_server_websocket_route.h"
#include "server/request_deadline.h"
#include "server/request_memory_arena.h"
#include "server/worker_connections.h"

// Member-template definitions for worker_connections, kept out of its header so the
// class stays readable. Included as an ordinary header: everything used here is
// included here.

namespace ruvia::detail {

template <typename stream_type>
task<void> worker_connections::run_stream(http_server_session_config& listener_value, stream_type& stream,
    tcp_socket_type& socket, context_services base_route_services) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the count
    // of buffered bytes. The heavy per-request working set (read buffer, request arena,
    // parse result, response head, file chunk) is borrowed from a per-worker
    // pool only while the connection is actively serving and returned the moment
    // it goes idle, so an idle keep-alive connection holds none of it.
    ruvia::connection_scanner::entry_type scanner_entry;
    ruvia::connection_scanner::guard_type scanner_guard(&scanner_, scanner_entry, socket);
    const auto& routes_value = routes_;
    const auto remote_address = base_route_services.get_conn_info().remote().address();
    // Re-resolved per request: one keep-alive connection carries many requests,
    // each with its own forwarding headers. Falls back to the peer until the
    // first request line is parsed.
    auto client_address = remote_address;
    inbound_buffer_resource connection_buffers(
        options_.inbound_buffer_pool_ != nullptr ? options_.inbound_buffer_pool_ : memory_.resource(),
        options_.max_inbound_buffer_bytes_per_connection_);
    base_route_services = base_route_services.with_inbound_buffer_pool(connection_buffers);
    http1_request_sequence request_sequence(options_.max_requests_per_connection_);
    std::size_t used_bytes = 0;
    connection_work_set* work_set = nullptr;
    work_set_return work_set_return(work_sets_, work_set);
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, idle_resident_read_bytes> idle_read_buffer;
    std::size_t idle_read_bytes = 0;
    // nginx-aligned wait semantics. The first request on a connection, and any
    // partially-received request header, are bounded by request_header_timeout
    // (reading_initial). The idle wait for the *next* request on a reused
    // keep-alive connection -- no bytes of it received yet -- is bounded by
    // idle_timeout (idle), matching nginx keepalive_timeout. Flips true
    // once a request has been served so later idle waits use the keepalive
    // deadline instead of the header deadline.
    bool served_keepalive_request = false;

    constexpr bool plain_tcp = std::is_same_v<std::remove_cvref_t<stream_type>, tcp_socket_type>;
    for (;;) {
        scanner_entry.set_phase(ruvia::connection_scanner::phase_type::idle);

        // Borrow-on-use / return-on-idle for the whole work set: when the
        // connection has no buffered bytes, return the work set to the
        // per-worker pool and read the next request's first bytes into the
        // small connection-resident buffer instead, so an idle keep-alive
        // connection occupies no work set (memory scales with in-flight
        // requests, not total connections). Reading directly -- rather than a
        // bufferless readiness wait -- costs no extra reactor pass when data
        // is already queued. A pipelined burst (used_bytes > 0) keeps its work
        // set and skips this. Plain TCP only: a TLS engine may buffer a
        // decrypted record that raw socket readiness cannot see, so an idle
        // wait there could stall; TLS holds across the connection.
        if constexpr (plain_tcp) {
            if (plain_tcp_should_wait_for_next_request(used_bytes)) {
                release_idle_work_set(work_sets_, work_set);
                // Idle wait for the next keep-alive request uses idle_timeout;
                // the connection's first request uses request_header_timeout.
                scanner_entry.set_phase(served_keepalive_request
                                            ? ruvia::connection_scanner::phase_type::idle
                                            : ruvia::connection_scanner::phase_type::reading_initial);
                auto idle_completion = co_await ruvia::async_asio<std::size_t>(
                    [&socket, &idle_read_buffer](auto handler) mutable {
                        socket.async_read_some(
                            asio::buffer(idle_read_buffer.data(), idle_read_buffer.size()),
                            std::move(handler));
                    });
                const auto idle_ec = idle_completion.error_code();
                const auto idle_bytes = idle_completion.result();
                if (idle_ec || !http_server_worker_running(state_)) {
                    co_return;
                }
                idle_read_bytes = idle_bytes;
            }
        }
        if (work_set == nullptr) {
            work_set = work_sets_.acquire();
        }
        auto& read_buffer = work_set->read_buffer_;
        auto& parser = work_set->parser_;
        auto& parsed_value = work_set->parsed_;
        auto& response_head = work_set->response_head_;
        auto& file_chunk = work_set->file_chunk_;
        auto& route_resolution_value = work_set->route_resolution_;

        if constexpr (plain_tcp) {
            if (idle_read_bytes > 0) {
                static_assert(idle_resident_read_bytes <= initial_read_buffer_bytes);
                std::memcpy(read_buffer.data(), idle_read_buffer.data(), idle_read_bytes);
                used_bytes = idle_read_bytes;
                idle_read_bytes = 0;
                scanner_entry.touch();
            }
        }

        std::optional<request_memory> request_memory_storage;
        auto& request_memory_value = emplace_request_memory(request_memory_storage, memory_,
            std::span<std::byte>(work_set->arena_block_, sizeof(work_set->arena_block_)));
        // Work-set storage outlives this arena. Release its descriptor block
        // on every exit (including exceptions/cancellation) before the arena dies.
        struct request_header_lifetime final {
            http_request& request_;
            ~request_header_lifetime() {
                request_.reset();
            }
        } header_lifetime{parsed_value.request_};
        http_response response({.resource_ = request_memory_value.resource()});
        http_response_coding_policy response_coding_policy = http_response_coding_policy::disabled();
        // Holds the next pipelined request from the moment a body route hands it
        // over until the read buffer is cleaned up below. Declared before
        // request_completion, which borrows it, and empty for the common case of a
        // client that does not pipeline.
        std::pmr::string pipeline_stash(request_memory_value.resource());
        // Declared here, before request_completion and everything that borrows
        // the services below, so the deadline's stop source outlives every
        // dispatch that observes its token.
        std::optional<request_deadline> request_deadline;
        http1_interim_response_sink interim_sink(stream, memory_.resource());
        context_services request_services = base_route_services.with_interim_output(interim_sink.output());
        std::optional<http1_session_request_completion> request_completion;
        // Rejections that close the connection funnel through one co_await
        // site after the read loop: every co_await expression in a coroutine
        // reserves its own frame slots for the call's temporaries (GCC does
        // not overlap them), so inlining handle_error at each rejection site
        // costs ~660 resident bytes per site in every connection's frame.
        http1_closing_rejection closing_rejection;
        std::size_t header_search_offset = 0;
        const auto request_start = std::chrono::steady_clock::now();
        for (;;) {
            if constexpr (plain_tcp) {
                if (used_bytes > 0) {
                    const auto h2_result = co_await dispatch_cleartext_http2_preface(
                        http2_server_session_setup<stream_type>{
                            .stream_ = stream,
                            .socket_ = socket,
                            .memory_ = memory_,
                            .routes_ = routes_,
                            .options_ = options_,
                            .scanner_entry_ = scanner_entry,
                            .services_ = base_route_services,
                            .worker_state_ = state_,
                        },
                        read_buffer, used_bytes, listener_value.redirect() != nullptr);
                    if (h2_result == cleartext_http2_dispatch_result::session_finished) {
                        co_return;
                    }
                    if (h2_result == cleartext_http2_dispatch_result::continue_read_loop) {
                        continue;
                    }
                }
            }
            const auto buffer_view = std::string_view(read_buffer.data(), used_bytes);
            response_coding_policy = http_response_coding_policy::disabled();
            parser.parse_head(buffer_view, parsed_value, header_search_offset, request_memory_value.resource());
            if (const auto* request_head = parsed_value.head_ready()) {
                // Reset phase so request_header_timeout stops counting against dispatch
                // time. Body readers will set reading_payload on their own; the
                // streaming/websocket paths set their own phases below; the
                // buffered write path sets writing before responding. Until
                // one of those transitions, idle_timeout governs as the
                // deadman switch for hung handlers.
                scanner_entry.set_phase(ruvia::connection_scanner::phase_type::idle);
                // Negotiate against every coding first. A document root or a
                // context::static_file route may serve an indexed precompressed
                // sidecar even when this worker has no runtime encoder. The
                // actual encoder capability is carried to representation
                // preparation instead of pruning the request's choices here.
                const auto response_coding_negotiation = parsed_value.response_coding_selection();
                if (const auto* selection = response_coding_negotiation.selected()) {
                    response_coding_policy = http_response_coding_policy::selected(*selection);
                } else {
                    // Buffered routes may still produce a representation-free
                    // 204/205/304. Keep the negotiation failure typed until
                    // the response status is known instead of rejecting every
                    // request before its handler runs.
                    response_coding_policy = http_response_coding_policy::no_acceptable_coding();
                }
                // Reset per request: a keep-alive connection serves many, and
                // each gets its own deadline or none. Resolve before any
                // server-layer rejection below: a custom on_error/on_not_found
                // handler is a handler too and must see the request stop token.
                request_deadline.reset();
                request_services = base_route_services.with_interim_output(interim_sink.output());
                route_resolution_value = routes_value.resolve(parsed_value.request_);
                // Keyed on the client, not the hop: behind a trusted proxy every
                // request would otherwise share the proxy's single key.
                client_address =
                    base_route_services.resolve_conn_info(parsed_value.request_).client().address();
                const auto* resolved = route_resolution_value.resolved();
                const auto handler_deadline = effective_handler_deadline(
                    options_.deadline_ ? std::optional{options_.deadline_->handler_} : std::nullopt,
                    resolved != nullptr ? resolved->route().deadline_ms() : 0);
                if (handler_deadline > std::chrono::milliseconds::zero()) {
                    request_deadline.emplace(stop_token_);
                    request_deadline->arm(worker_, handler_deadline);
                    request_services = request_services.with_request_deadline(*request_deadline);
                }

                const auto expectation_plan =
                    parsed_value.body_plan_.expectation_plan(http_unsupported_expectation_policy::reject);
                if (const auto* rejection = expectation_plan.rejection()) {
                    // Expect extensions are valid HTTP syntax. The protocol parser
                    // reports the semantic fact; this Web product deliberately does
                    // not implement extensions beyond 100-continue and chooses the
                    // RFC 9110-permitted 417 response before reading request content.
                    closing_rejection = http1_closing_rejection::error(copy_http_protocol_error_info(
                        request_memory_value.resource(), rejection->protocol_error()));
                    break;
                }
                if (const auto* redirect = listener_value.redirect()) {
                    if (parsed_value.request_.header("Host").value_or(std::string_view{}).empty()) {
                        closing_rejection = http1_closing_rejection::error(
                            http_error_info({.status_ = ruvia::http_status::bad_request,
                                .message_ = "missing Host header"}));
                        break;
                    }
                    response = make_auto_https_redirect_response(
                        parsed_value.request_, request_memory_value, redirect->https_port_);
                    if (http_response_needs_not_acceptable(
                            response_coding_policy, parsed_value.request_, response)) {
                        response = co_await routes_value.handle_error(parsed_value.request_, request_memory_value,
                            http_error_info({
                                .status_ = ruvia::http_status::not_acceptable,
                                .code_ = "not_acceptable",
                                .message_ = "no acceptable response content coding",
                            }),
                            request_services);
                        // The redirect branch commits directly because its
                        // connection is intentionally closing; make the
                        // generated policy error terminal rather than letting
                        // it inherit the original negotiated coding promise.
                        response_coding_policy = http_response_coding_policy::disabled();
                    }
                    const auto connection_plan = require_http1_final_response_commit(
                        response, parsed_value.connection_plan_.require_close());
                    request_completion.emplace(
                        http1_session_request_completion::make_buffered_closing(connection_plan));
                    scanner_entry.touch();
                    break;
                }

                const auto app_rate_limit =
                    decide_request_rate_limit(&capabilities_.rate_limiter(), client_address);
                if (const auto* rejection = app_rate_limit.rejection()) {
                    closing_rejection =
                        http1_closing_rejection::get_rate_limit(rate_limit_rejection_error(), *rejection);
                    break;
                }

                if (resolved == nullptr) {
                    if (const auto body_failure = content_length_limit_failure(parsed_value.body_plan_,
                            protocol_byte_limit::limited(options_.max_buffered_body_bytes_))) {
                        closing_rejection = http1_closing_rejection::error(copy_http_protocol_error_info(
                            request_memory_value.resource(), body_failure->protocol_error()));
                        break;
                    }
                    response =
                        co_await routes_value.dispatch_buffered_response(parsed_value.request_, route_resolution_value,
                            request_memory_value, options_.document_root_.binding(), request_services,
                            request_services.precompressed_static_files()
                                ? static_file_selection_mode::precompressed
                                : static_file_selection_mode::identity_only);
                    // An unresolved request never consumes its body, regardless
                    // of whether the shared Web dispatch selected a document-root
                    // file, 404, 405, or OPTIONS response.
                    auto connection_plan = apply_request_body_consumption(
                        parsed_value.connection_plan_, parsed_value.body_plan_.requires_consumption()
                                                           ? http1_request_body_consumption::incomplete
                                                           : http1_request_body_consumption::complete);
                    connection_plan =
                        finalize_buffered_route_response(response, connection_plan, request_sequence);
                    request_completion.emplace(http1_session_request_completion::make_buffered_unrestored(
                        connection_plan, request_head->header_bytes()));
                    scanner_entry.touch();
                    break;
                }

                // One bundle of what every route dispatch below needs from
                // this session; each dispatcher adds only its own arguments.
                const auto route_dispatch = [&] {
                    return http1_route_dispatch<stream_type>{
                        .stream_ = stream,
                        .memory_ = memory_,
                        .scanner_entry_ = scanner_entry,
                        .parsed_ = parsed_value,
                        .response_coding_ = *response_coding_policy.selection(),
                        .response_coding_availability_ =
                            options_.compression_.has_value()
                                ? http_response_coding_availability::identity_and_compression
                                : http_response_coding_availability::identity_only,
                        .routes_ = routes_value,
                        .request_memory_ = request_memory_value,
                        .base_route_services_ = request_services,
                        .options_ = options_,
                        .response_ = response,
                        .request_sequence_ = request_sequence,
                        .inbound_buffer_pool_ = &connection_buffers,
                    };
                };

                const auto& route = resolved->route();
                const auto& endpoint = route.endpoint();
                if (response_coding_policy.negotiation_failed() &&
                    endpoint.response_stream() != nullptr) {
                    // A response stream commits its representation before a
                    // buffered status can be inspected. websocket upgrades do
                    // not select an HTTP response representation.
                    closing_rejection = http1_closing_rejection::error(
                        http_error_info({.status_ = ruvia::http_status::not_acceptable,
                            .code_ = "not_acceptable",
                            .message_ = "no acceptable response content coding"}));
                    break;
                }
                const auto max_request_body_bytes =
                    request_body_byte_limit(endpoint.request_body_mode(), options_.max_stream_body_bytes_,
                        options_.max_buffered_body_bytes_, resolved->route().max_request_body_bytes());
                if (const auto body_failure =
                        content_length_limit_failure(parsed_value.body_plan_, max_request_body_bytes)) {
                    closing_rejection = http1_closing_rejection::error(copy_http_protocol_error_info(
                        request_memory_value.resource(), body_failure->protocol_error()));
                    break;
                }

                if (endpoint.tunnel() != nullptr) {
                    const auto pending = std::string_view(read_buffer.data() + request_head->header_bytes(),
                        used_bytes - request_head->header_bytes());
                    auto completion = co_await dispatch_http_tunnel_route(route_dispatch(), *resolved, pending);
                    if (!completion) {
                        co_return;
                    }
                    request_completion.emplace(std::move(*completion));
                    break;
                }

                if (endpoint.get_websocket() != nullptr) {
                    const auto pending_frames =
                        std::string_view(read_buffer.data() + request_head->header_bytes(),
                            used_bytes - request_head->header_bytes());
                    auto websocket_completion = co_await dispatch_http_websocket_route(
                        route_dispatch(), *resolved, pending_frames);
                    if (!websocket_completion.has_value()) {
                        co_return;
                    }
                    request_completion.emplace(std::move(*websocket_completion));
                    break;
                }

                if (endpoint.response_stream() != nullptr) {
                    request_completion.emplace(co_await dispatch_http_response_stream_route(
                        route_dispatch(), response_head, *request_head, *resolved,
                        http_body_and_pipeline(*request_head, read_buffer, used_bytes), max_request_body_bytes));
                    break;
                }
                const auto* buffered_endpoint = endpoint.buffered();
                if (buffered_endpoint != nullptr &&
                    buffered_endpoint->request_body_mode() == request_body_mode::stream) {
                    request_completion.emplace(co_await dispatch_http_stream_body_route(route_dispatch(),
                        *request_head, route_resolution_value, read_buffer, used_bytes, pipeline_stash));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const auto body_and_pipeline =
                        http_body_and_pipeline(*request_head, read_buffer, used_bytes);

                    // The body reader/loader setup can throw (e.g. constructing a
                    // transfer-coding decoder for a bad Transfer-Encoding), so it
                    // stays guarded. The dispatch itself never throws:
                    // dispatch_buffered_response turns any handler or routing
                    // failure into a response, so it sits outside the guard.
                    std::exception_ptr body_setup_exception;
                    http_lazy_buffered_body_route_state<stream_type> body_state;
                    try {
                        prepare_http_lazy_buffered_body_route(
                            body_state, route_dispatch(), max_request_body_bytes, body_and_pipeline);
                    } catch (...) {
                        body_setup_exception = std::current_exception();
                    }

                    if (body_setup_exception != nullptr) {
                        request_completion.emplace(
                            co_await complete_failed_http_body_route(scanner_entry, body_setup_exception,
                                parsed_value, routes_value, request_memory_value, request_services, response));
                        break;
                    }

                    response = co_await routes_value.dispatch_buffered_response(parsed_value.request_,
                        route_resolution_value, request_memory_value, options_.document_root_.binding(),
                        body_state.with_loader(request_services));

                    request_completion.emplace(complete_successful_http_body_route(scanner_entry,
                        response, parsed_value.connection_plan_, request_sequence, body_state.consumption(),
                        pipeline_stash,
                        [&body_state](std::pmr::string& stash) { body_state.take_pipeline(stash); }));
                    break;
                }
            }

            if (const auto* failure = parsed_value.failure()) {
                if constexpr (plain_tcp) {
                    if (listener_value.redirect() == nullptr &&
                        ruvia::should_drop_invalid_cleartext_http1_input(buffer_view,
                            failure->source() == http1_server_request_parse_failure_source::request_line
                                ? http1_request_parse_failure_source::request_line
                                : http1_request_parse_failure_source::message)) {
                        co_return;
                    }
                }
                const auto error = failure->protocol_error();
                closing_rejection = http1_closing_rejection::error(
                    copy_http_protocol_error_info(request_memory_value.resource(), error));
                break;
            }

            header_search_offset = used_bytes;

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idle_timeout); once any header bytes are
            // buffered, or on the first request, request_header_timeout governs.
            scanner_entry.set_phase((used_bytes == 0 && served_keepalive_request)
                                        ? ruvia::connection_scanner::phase_type::idle
                                        : ruvia::connection_scanner::phase_type::reading_initial);
            grow_read_buffer(read_buffer, used_bytes);
            if (used_bytes == read_buffer.size()) {
                const auto error = http_parse_protocol_error(http_parse_error::header_too_large);
                closing_rejection = http1_closing_rejection::error(
                    copy_http_protocol_error_info(request_memory_value.resource(), error));
                break;
            }

            auto read_completion = co_await ruvia::async_asio<std::size_t>(
                [&stream, &read_buffer, used_bytes](auto handler) mutable {
                    stream.async_read_some(
                        asio::buffer(read_buffer.data() + used_bytes, read_buffer.size() - used_bytes),
                        std::move(handler));
                });
            const auto ec = read_completion.error_code();
            const auto bytes_read = read_completion.result();
            if (ec) {
                co_return;
            }

            used_bytes += bytes_read;
            scanner_entry.touch();
        }

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch.
        if (const auto* closing_error = closing_rejection.error()) {
            response = co_await routes_value.handle_error(
                parsed_value.request_, request_memory_value, *closing_error, request_services);
            if (const auto* rate_limit = closing_rejection.get_rate_limit()) {
                apply_rate_limit_rejection_headers(response, *rate_limit);
            }
            request_completion.emplace(http1_session_request_completion::make_buffered_closing(
                require_http1_final_response_commit(response, parsed_value.connection_plan_.require_close())));
        }

        if (!request_completion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        auto connection_plan = request_completion->connection_plan();
        if (request_completion->buffered_response() != nullptr) {
            scanner_entry.set_phase(ruvia::connection_scanner::phase_type::writing);
            const auto preparation = co_await prepare_application_response(
                parsed_value.request_, response_coding_policy, response, options_, routes_value,
                request_memory_value, request_services);
            if (!preparation) {
                co_return;
            }
            if (preparation->recovered_) {
                connection_plan = require_http1_final_response_commit(response, connection_plan);
                request_completion = request_completion->with_buffered_connection_plan(connection_plan);
                connection_plan = request_completion->connection_plan();
            }
            const auto write_plan = preparation->write_plan_;
            const auto response_plan = get_http1_buffered_response_plan(write_plan, connection_plan);
            const auto write_result = co_await write_response(
                stream, memory_, &response_head, &file_chunk, response, response_plan);
            scanner_entry.set_phase(ruvia::connection_scanner::phase_type::idle);
            if (const auto committed_status = write_result.committed_status()) {
                record_http_access(options_.access_log_, parsed_value.request_, client_address,
                    *committed_status, request_start);
            }
            if (write_result.completed() == nullptr) {
                co_return;
            }
        } else if (const auto* committed = request_completion->committed_stream()) {
            scanner_entry.set_phase(ruvia::connection_scanner::phase_type::idle);
            record_http_access(options_.access_log_, parsed_value.request_, client_address, committed->status(),
                request_start);
        } else {
            throw std::logic_error("HTTP/1 request completion has no wire alternative");
        }

        if (connection_plan.disposition() == http1_close_policy::close_after_response ||
            !http_server_worker_running(state_)) {
            co_return;
        }
        apply_reusable_http1_request_buffer_completion(
            request_completion->buffer_completion(), read_buffer, used_bytes);
        trim_read_buffer_storage(read_buffer, used_bytes);
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        served_keepalive_request = true;
    }
}

}  // namespace ruvia::detail
