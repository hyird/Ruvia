#include "client/http1_client_exchange.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/ssl/error.hpp>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_body_decoder.h"
#include "ruvia/http/http1_request_content_writer.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"

#include "body/http_body_buffer.h"
#include "client/http1_request_head.h"
#include "client/http_client_connection.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"
#include "http/http_socket_tunnel_transport.h"
#include "http/tls_tunnel_output.h"

namespace ruvia::detail {
namespace {

// The largest request head the client sends: the header limit plus the
// request line and generated fields.
constexpr std::size_t max_client_request_head_bytes = max_http_header_bytes + 1024;
constexpr std::size_t response_read_chunk_bytes = 16384;

// One contiguous write() part. A temporary argument outlives the write: it is
// destroyed only at the end of the full-expression that awaits it.
[[nodiscard]] std::array<asio::const_buffer, 1> single_part(std::string_view bytes) noexcept {
    return {asio::buffer(bytes)};
}

[[nodiscard]] bool is_upload_io_failure(const std::exception_ptr& failure) noexcept {
    try {
        std::rethrow_exception(failure);
    } catch (const http_client_error& error) {
        return error.code() == http_client_error::code_type::io_error;
    } catch (...) {
        return false;
    }
}

[[noreturn]] void throw_body_timeout() {
    throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
}

// True when the final head accepts the request's CONNECT or CONNECT-UDP
// tunnel; a CONNECT-UDP head is validated before it is accepted.
[[nodiscard]] bool accepts_tunnel(const http_client_request_storage& request,
    const http_client_response_state& state_value, const http1_parsed_client_response_head& head) {
    if (!request.is_tunnel()) {
        return false;
    }
    if (!request.udp_tunnel()) {
        return head.plan().connect_tunnel() != nullptr;
    }
    if (head.plan().connect_tunnel() == nullptr && head.plan().protocol_upgrade() == nullptr) {
        return false;
    }
    if (validate_http_connect_udp_response(state_value.protocol_version_, state_value.status_.status().value(), state_value.headers_).has_value()) {
        throw http_client_error(http_client_error::code_type::protocol_error, "invalid CONNECT-UDP response head");
    }
    return true;
}

// Publishes one decoder step's body bytes or trailers to the consumer, or
// throws its failure.
void publish_body_progress(http_client_response_state& state_value,
    const http1_client_response_body_decoder::result_type& decoded, std::size_t max_response_bytes) {
    if (const auto* body = decoded.output()) {
        const auto bytes_value = body->bytes();
        const auto retained = state_value.buffered_.size() - state_value.offset_ + state_value.pending_.size();
        if (state_value.collect_all_ &&
            bytes_value.size() > max_response_bytes - std::min(retained, max_response_bytes)) {
            // The encoded body was never exposed and can no longer produce a
            // valid collected result. Release its storage with the failure.
            state_value.discard_response_body();
            throw http_client_error(
                http_client_error::code_type::response_too_large, "HTTP response exceeds configured byte limit");
        }
        // The consumer's last borrowed view lives in buffered, not pending.
        // Appending producer output must never relocate that view.
        state_value.pending_.append(bytes_value);
        state_value.data_signal_.notify();
    } else if (const auto* trailers = decoded.trailers()) {
        const auto ok = visit_http_response_trailers(trailers->bytes(), [&](std::string_view name, std::string_view value) {
            state_value.trailers_.emplace_back(name, value, state_value.resource_);
            return true;
        });
        if (!ok) {
            throw std::logic_error("HTTP decoder published invalid response trailers");
        }
    } else if (const auto* failure = decoded.protocol_failure()) {
        throw http_client_error(http_client_error::code_type::protocol_error,
            std::string(http1_client_response_body_error_message(failure->error())));
    } else if (decoded.decoder_failure() != nullptr) {
        throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/1 response body decoder failed");
    }
}

}  // namespace

task<void> http1_client_exchange::write(std::span<const asio::const_buffer> parts, const operation_timeout& timeout) {
    const auto written =
        co_await connection_.transport_.write(parts, timeout.constrained_by(pool_.config().write_timeout_));
    if (!written.ok()) {
        throw_http_client_transport_error(written, "upstream closed while receiving the HTTP request");
    }
}

task<client_transport_result> http1_client_exchange::read_append(
    http_io_buffer& target, std::size_t max_bytes, const operation_timeout& timeout) {
    const auto read_space = target.prepare(max_bytes);
    const auto read = co_await connection_.transport_.read_some(read_space, timeout);
    // close() may clear the target while the read is pending; never publish
    // bytes of a closed connection into it.
    if (connection_.connected_) {
        target.commit(read.bytes_);
    }
    if (read.ok() || read.status_ == client_transport_status::eof) {
        co_return read;
    }
    throw_http_client_transport_error(read, "upstream closed the HTTP connection");
}

task<void> http1_client_exchange::run(
    const http_client_request_storage& request, const operation_timeout& timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    if (!request.tunnel_protocol().empty() && !request.udp_tunnel()) {
        throw http_client_error(http_client_error::code_type::protocol_unavailable, "Extended CONNECT requires HTTP/2");
    }
    auto& connection = connection_;
    connection.transport_.reset_abort();
    connection.generation_ = connection.generation_ + 1 == 0 ? 1 : connection.generation_ + 1;
    struct exchange_generation final {
        http_client_connection& connection_;
        http_client_response_state& response_;
        ~exchange_generation() {
            ++connection_.generation_;
            connection_.http1_response_ = nullptr;
            response_.cancellation_id_ = 0;
        }
    } exchange_generation_value{connection, state_value};
    worker_cancellation_registration cancellation(pool_.cancellation_target(), connection.cancellation_id_);
    state_value.cancellation_id_ = cancellation.id();
    // Bound for the exchange so a cancellation or shutdown stops the
    // response's output and wakes its producer waits.
    connection.http1_response_ = &state_value;
    state_value.transport_ = http_client_response_transport::http1;
    cancellation.arm(stop_token_value);

    auto* const response_resource = state_value.resource_;
    std::pmr::vector<http_header_view> headers(pool_.resource());
    std::pmr::string cookie_header(pool_.resource());
    auto source_value = pool_.policy().prepare_request(request, headers, cookie_header);

    auto origin = pool_.origin();
    const bool connect_udp = request.udp_tunnel();
    if (connect_udp) {
        const auto authority = parse_http_authority(borrowed_text(request.tunnel_authority()));
        if (!authority) {
            throw http_client_error(http_client_error::code_type::invalid_request, "invalid CONNECT-UDP authority");
        }
        const http_origin_options options{.host_ = authority->host(), .port_ = authority->port()};
        origin = pool_.config().scheme_ == http_scheme::https ? http_origin_view::https(options)
                                                              : http_origin_view::http(options);
    }

    auto* const upload = state_value.upload_ ? &*state_value.upload_ : nullptr;
    http1_client_request_writer writer(response_resource);
    const auto prepare_head = [&](std::span<char> buffer) {
        if (connect_udp) {
            return writer.prepare_connect_udp(origin, source_value.target_, headers, buffer);
        }
        if (request.is_tunnel()) {
            return writer.prepare_connect(borrowed_text(request.tunnel_authority()), headers, buffer);
        }
        if (upload != nullptr) {
            return writer.prepare_streaming(origin,
                {.method_ = source_value.method_,
                    .target_ = source_value.target_,
                    .headers_ = headers,
                    .content_length_ = upload->config_.content_length_},
                buffer, {.expectation_ = upload->config_.expectation_});
        }
        return writer.prepare(origin, source_value, buffer);
    };
    // The head is written into the connection's retained, uninitialized buffer.
    auto& write_buffer = connection_.write_buffer_;
    write_buffer.clear();
    auto prepared_result = prepare_http1_request_head(write_buffer, max_client_request_head_bytes, prepare_head);
    const auto* prepared = std::get_if<prepared_http1_client_request>(&prepared_result);
    if (!prepared) {
        const auto* failure = std::get_if<http1_client_request_prepare_error>(&prepared_result);
        throw http_client_error(http_client_error::code_type::invalid_request,
            failure != nullptr
                ? std::string(http1_client_request_prepare_error_message(*failure))
                : "HTTP request head is too large");
    }
    http1_client_response_parser parser(prepared->exchange_state(), response_resource);
    if (const auto* content = prepared->content_plan().immediate(); content && !content->bytes().empty()) {
        // Keep the retained head alive through the composed scatter-gather write.
        const std::array<asio::const_buffer, 2> parts{
            asio::buffer(prepared->head().data(), prepared->head().size()),
            asio::buffer(content->bytes().data(), content->bytes().size())};
        co_await write(parts, timeout);
        if (parser.complete_request_content() != http1_client_request_content_completion_status::completed) {
            std::terminate();
        }
    } else {
        // Streaming/continue-gated content must still send its head separately.
        co_await write(single_part(prepared->head()), timeout);
    }
    write_buffer.clear_retaining_small();

    if (upload == nullptr) {
        co_await receive(request, timeout, state_value, parser);
        co_return;
    }
    task_scope writers(pool_.worker(), {.resource_ = pool_.resource()});
    std::exception_ptr writer_failure;
    writers.spawn(write_upload(state_value, timeout,
        http1_request_content_writer(*prepared->content_plan().streaming()), parser, writer_failure));
    std::exception_ptr receive_failure;
    try {
        co_await receive(request, timeout, state_value, parser);
    } catch (...) {
        receive_failure = std::current_exception();
    }
    if (receive_failure) {
        upload->output_.stop();
        connection_.close();
    } else {
        // Response framing can finish before the request content. Publish that
        // boundary independently, but keep the connection leased until the
        // writer finishes the request or is explicitly stopped.
        state_value.complete_ = true;
        state_value.data_signal_.notify();
    }
    worker_timer_registration upload_completion_timer;
    if (!upload->output_.ended_ && !upload->output_.stopped_) {
        if (const auto remaining = timeout.remaining()) {
            (void)pool_.worker().schedule_timer(upload_completion_timer, worker_timer_deadline_after(*remaining),
                [this, generation = connection_.generation_](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        connection_.cancel(generation, client_abort_reason::timeout);
                    }
                });
        }
    }
    co_await writers.join();
    upload_completion_timer.cancel();
    if (!upload->output_.ended_) {
        connection_.close();
    }
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
    connection_.throw_if_aborted();
}

task<void> http1_client_exchange::receive(const http_client_request_storage& request, const operation_timeout& timeout,
    http_client_response_state& state_value, http1_client_response_parser& parser) {
    auto& read_buffer = connection_.read_buffer_;
    // The parsed head is move-constructible only, so it is emplaced once.
    std::optional<http1_parsed_client_response_head> head;
    while (!head) {
        if (auto next = take_final_head(state_value, parser)) {
            head.emplace(std::move(*next));
        } else if (!(co_await read_append(read_buffer, response_read_chunk_bytes, timeout)).ok()) {
            throw http_client_error(
                http_client_error::code_type::io_error, "upstream closed before the HTTP response head");
        }
    }
    publish_final_head(request, state_value, *head);
    if (accepts_tunnel(request, state_value, *head)) {
        state_value.tunnel_->accepted_ = true;
        state_value.head_ready_ = true;
        state_value.head_signal_.notify();
        co_await run_tunnel(state_value, timeout);
        co_return;
    }
    if (state_value.tunnel_) {
        state_value.tunnel_->output_.stop();
    }
    const auto& plan = head->plan();
    if (plan.connect_tunnel() != nullptr || plan.protocol_upgrade() != nullptr) {
        throw http_client_error(http_client_error::code_type::protocol_error,
            "HTTP tunnel and protocol upgrade responses require a dedicated API");
    }
    http1_client_response_body_decoder body_decoder(plan, state_value.resource_);
    const bool content_semantics_present = plan.without_content() == nullptr;
    configure_http_client_response_decoding(state_value);
    state_value.head_ready_ = true;
    state_value.head_signal_.notify();

    // The body loop stays in this frame: one exchange costs one receive frame.
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    // Empty framing still runs the decoder and boundary checks, but its
    // mandatory nonempty scratch can stay inline. Other responses borrow
    // uninitialized worker storage without inflating this coroutine frame.
    const auto* known_length = plan.known_length();
    const bool empty_body = !content_semantics_present ||
                            (known_length != nullptr && !known_length->requires_body_consumption());
    std::array<char, 1> empty_output;
    http_io_buffer output_storage(pool_.resource());
    const auto output = empty_body ? std::span<char>(empty_output)
                                   : output_storage.prepare(http_body_buffer_bytes);
    bool eof = false;
    http1_close_policy persistence = http1_close_policy::close_after_response;
    for (;;) {
        // Backpressure applies to decoding buffered compressed input too,
        // not just to the next transport read. One step cannot overfill the
        // producer queue; read_all retains its separate total byte policy.
        // The wait is a coroutine only for the rare full queue.
        if (!state_value.collect_all_ && state_value.pending_.size() >= max_response_bytes) {
            co_await wait_for_body_space(state_value, timeout);
        }
        connection_.throw_if_aborted();
        const auto capacity = state_value.collect_all_
                                  ? output.size()
                                  : std::min(output.size(), max_response_bytes - state_value.pending_.size());
        const auto scratch = output.first(capacity);
        const auto decoded = eof ? body_decoder.finish_input(read_buffer.view(), scratch)
                                 : body_decoder.decode(read_buffer.view(), scratch);
        const auto consumed = decoded.consumed_bytes();
        publish_body_progress(state_value, decoded, max_response_bytes);
        // Output/trailers have been owned before their source is compacted.
        read_buffer.consume(consumed);
        if (const auto* complete = decoded.complete()) {
            persistence = complete->persistence();
            break;
        }
        if (decoded.need_input() == nullptr || (consumed != 0 && !read_buffer.empty())) {
            continue;
        }
        if (eof) {
            throw std::logic_error("HTTP body decoder requested input after EOF");
        }
        const auto read = co_await read_append(read_buffer, response_read_chunk_bytes, timeout);
        // A TLS peer that drops TCP without close_notify leaves the message end
        // unauthenticated: an attacker could cut a close-delimited body short
        // (RFC 9112 §9.8). Every other framing still needed bytes here too.
        if (read.error_ == asio::ssl::error::stream_truncated) {
            throw http_client_error(http_client_error::code_type::io_error,
                "upstream closed the TLS connection without close_notify");
        }
        eof = !read.ok();
    }
    // Bytes beyond this response's framing cannot belong to a response the
    // client requested. Preserve the complete framed response, but retire the
    // transport so those bytes are discarded rather than parsed as a future
    // response. A rejected CONNECT likewise leaves subsequent ownership
    // unknown (RFC 9112 §9.6).
    const bool reusable = persistence == http1_close_policy::allow_reuse &&
                          !request.is_tunnel() && read_buffer.empty();
    if (timeout.expired()) {
        throw_body_timeout();
    }
    decode_http_client_response_body(state_value, content_semantics_present, max_response_bytes);
    if (timeout.expired()) {
        throw_body_timeout();
    }
    if (!reusable) {
        connection_.close();
    }
    connection_.retain_idle_buffers();
}

std::optional<http1_parsed_client_response_head> http1_client_exchange::take_final_head(
    http_client_response_state& state_value, http1_client_response_parser& parser) {
    auto& read_buffer = connection_.read_buffer_;
    for (;;) {
        auto parse_result = parser.parse(read_buffer.view());
        if (parse_result.failure()) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                std::string(http1_client_response_parse_error_message(*parse_result.failure())));
        }
        if (parse_result.need_more()) {
            if (read_buffer.size() >= max_http_header_bytes) {
                throw http_client_error(http_client_error::code_type::protocol_error, "HTTP response head is too large");
            }
            return std::nullopt;
        }
        auto* parsed_value = parse_result.parsed();
        if (!parsed_value) {
            std::terminate();
        }
        const auto* informational = parsed_value->plan().informational();
        if (informational == nullptr) {
            return std::move(*parsed_value);
        }
        auto head = std::move(*parsed_value).take_head();
        state_value.retain_informational(head.status(), std::move(head).take_headers());
        if (state_value.upload_ &&
            parsed_value->plan().request_content_signal() == http_client_request_content_signal::continue_value) {
            state_value.upload_->content_released_ = true;
            state_value.upload_->output_.notify_data();
        }
        read_buffer.consume(parsed_value->consumed_bytes());
    }
}

void http1_client_exchange::publish_final_head(const http_client_request_storage& request,
    http_client_response_state& state_value, http1_parsed_client_response_head& head) {
    if (state_value.upload_ && !state_value.upload_->output_.ended_ &&
        head.plan().request_content_signal() == http_client_request_content_signal::exchange_complete) {
        state_value.upload_->output_.stop();
    }
    state_value.status_ = head.head().received_status();
    state_value.protocol_version_ = head.head().protocol_version();
    state_value.response_body_plan_ =
        plan_http_response_body(classify_http_method(request.method()), state_value.status_.status());
    state_value.headers_ = std::move(head).take_head().take_headers();
    // A received final head changes the jar before consumers can issue
    // another request, regardless of body completion or abandonment.
    pool_.policy().retain_response_cookies(request.target(), state_value.headers_);
    connection_.read_buffer_.consume(head.consumed_bytes());
}

task<void> http1_client_exchange::wait_for_body_space(
    http_client_response_state& state_value, const operation_timeout& timeout) {
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    while (!state_value.collect_all_ && state_value.pending_.size() >= max_response_bytes) {
        connection_.throw_if_aborted();
        const auto remaining = timeout.remaining();
        if (remaining && remaining->count() == 0) {
            throw_body_timeout();
        }
        bool expired = false;
        worker_timer_registration timer;
        if (remaining) {
            (void)pool_.worker().schedule_timer(timer, worker_timer_deadline_after(*remaining),
                [&expired, &state_value](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expired = true;
                        state_value.space_signal_.notify();
                    }
                });
        }
        co_await state_value.space_signal_.wait();
        timer.cancel();
        connection_.throw_if_aborted();
        if (expired || timeout.expired()) {
            throw_body_timeout();
        }
        if (state_value.abandoned_) {
            throw http_client_error(http_client_error::code_type::cancelled, "HTTP response body was abandoned");
        }
    }
}

void http1_client_exchange::expire_tunnel() noexcept {
    connection_.transport_.abort(client_abort_reason::timeout);
    connection_.abort_http1_response();
    connection_.close();
}

task<void> http1_client_exchange::write_tunnel_output(http_client_output_queue& output,
    const operation_timeout& timeout, tls_tunnel_output* tls, std::exception_ptr& failure) {
    const bool secure = connection_.transport_.secure();
    auto& stream = connection_.transport_.stream();
    try {
        while (!output.ended_ && !output.stopped_) {
            if (!output.chunk_ready_ && !output.end_requested_) {
                co_await output.data_.wait();
                continue;
            }
            worker_timer_registration write_timer;
            if (const auto remaining = timeout.constrained_by(pool_.config().write_timeout_).remaining()) {
                (void)pool_.worker().schedule_timer(write_timer, worker_timer_deadline_after(*remaining),
                    [this](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            expire_tunnel();
                        }
                    });
            }
            const auto ending = output.end_requested_ && !output.chunk_ready_ ? http_stream_end::end
                                                                              : http_stream_end::keep_open;
            const auto bytes_value = output.chunk_ready_ ? std::string_view(output.chunk_) : std::string_view{};
            std::size_t completed_bytes = 0;
            std::error_code error;
            if (tls != nullptr) {
                http_socket_tunnel_transport transport(stream, tls);
                error = co_await transport.write_bytes(bytes_value, ending, &completed_bytes);
            } else {
                http_socket_tunnel_transport transport(stream.next_layer().socket(), nullptr);
                error = co_await transport.write_bytes(bytes_value, ending, &completed_bytes);
            }
            pool_.wire_counters().sent_ += completed_bytes;
            write_timer.cancel();
            connection_.throw_if_aborted();
            if (error) {
                throw http_client_error(secure && is_tls_error(error) ? http_client_error::code_type::tls_failed
                                                                      : http_client_error::code_type::io_error,
                    error_code_text(error));
            }
            if (ending == http_stream_end::end) {
                output.finish();
            } else {
                output.acknowledge_chunk();
            }
        }
    } catch (...) {
        failure = std::current_exception();
        connection_.close();
        output.stop();
    }
}

task<void> http1_client_exchange::run_tunnel(http_client_response_state& state_value, const operation_timeout& timeout) {
    auto& connection = connection_;
    auto& output = state_value.tunnel_->output_;
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    const bool secure = connection.transport_.secure();
    auto& stream = connection.transport_.stream();
    // Everything that can throw after the TLS writer starts runs inside the
    // receive try below, so the started writer is always joined before `tls`
    // is destroyed; the scope and timer slots are allocated first.
    task_scope writers(pool_.worker(), {.resource_ = pool_.resource()});
    worker_timer_registration lifetime_timer;
    std::optional<tls_tunnel_output> tls;
    if (secure) {
        tls.emplace(*stream.native_handle(), stream.next_layer().socket(), pool_.worker(), *pool_.resource());
        tls->start();
    }
    std::exception_ptr writer_failure;
    std::exception_ptr receive_failure;
    try {
        if (const auto remaining = timeout.remaining()) {
            (void)pool_.worker().schedule_timer(lifetime_timer, worker_timer_deadline_after(*remaining),
                [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expire_tunnel();
                    }
                });
        }
        writers.spawn(write_tunnel_output(output, timeout, tls ? &*tls : nullptr, writer_failure));
        auto& read_buffer = connection.read_buffer_;
        constexpr std::size_t read_chunk_bytes = 4096;
        for (;;) {
            while (state_value.pending_.size() >= max_response_bytes && !state_value.abandoned_) {
                co_await state_value.space_signal_.wait();
                connection.throw_if_aborted();
            }
            if (state_value.abandoned_) {
                throw http_client_error(http_client_error::code_type::cancelled, "CONNECT tunnel abandoned");
            }
            const auto room = max_response_bytes - state_value.pending_.size();
            if (!read_buffer.empty()) {
                const auto count = std::min(read_buffer.size(), room);
                state_value.pending_.append(read_buffer.data(), count);
                read_buffer.consume(count);
                state_value.data_signal_.notify();
                continue;
            }
            // The consumer may swap pending_ away while a read is in flight, so
            // reads land in the connection buffer and move on the next pass.
            const auto read = co_await read_append(read_buffer, std::min(read_chunk_bytes, room), timeout);
            if (!read.ok()) {
                // Same rule as a message body: a TLS peer that drops TCP
                // without close_notify leaves the tunnel end unauthenticated
                // (RFC 8446 section 6.1), so it is a failure, not an orderly end.
                if (read.error_ == asio::ssl::error::stream_truncated) {
                    throw http_client_error(http_client_error::code_type::io_error,
                        "upstream closed the TLS connection without close_notify");
                }
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
                break;
            }
        }
    } catch (...) {
        receive_failure = std::current_exception();
        connection.close();
        output.stop();
    }
    co_await writers.join();
    if (tls) {
        co_await tls->join();
    }
    lifetime_timer.cancel();
    connection.close();
    connection.retain_idle_buffers();
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
}

task<void> http1_client_exchange::write_upload(http_client_response_state& state_value, const operation_timeout& timeout,
    http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure) {
    auto& upload = *state_value.upload_;
    worker_timer_registration continue_timer;
    if (!upload.content_released_) {
        (void)pool_.worker().schedule_timer(continue_timer, worker_timer_deadline_after(upload.config_.continue_timeout_),
            [&upload, &parser](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.output_.stopped_) {
                    parser.release_request_content();
                    upload.content_released_ = true;
                    upload.output_.notify_data();
                }
            });
    }
    try {
        for (;;) {
            if (upload.output_.stopped_) {
                writer.abort();
                co_return;
            }
            connection_.throw_if_aborted();
            if (timeout.expired()) {
                throw http_client_error(http_client_error::code_type::timeout, "HTTP upload timed out");
            }
            if (!upload.content_released_ || (!upload.output_.chunk_ready_ && !upload.output_.end_requested_)) {
                co_await upload.output_.data_.wait();
                continue;
            }
            writer.release_content();
            continue_timer.cancel();
            if (upload.output_.chunk_ready_) {
                const auto chunk = writer.plan_chunk(std::span<const char>(upload.output_.chunk_.data(), upload.output_.chunk_.size()));
                if (chunk.index() != 0) {
                    throw http_client_error(http_client_error::code_type::invalid_request, "HTTP upload content length mismatch");
                }
                const auto& planned = std::get<0>(chunk);
                // The write lane has its own deadline, so the concurrent
                // response reader keeps its read deadline.
                const std::array<asio::const_buffer, 3> parts{asio::buffer(planned.prefix()),
                    asio::buffer(planned.payload_), asio::buffer(planned.suffix_)};
                co_await write(parts, timeout);
                writer.commit_chunk();
                upload.output_.acknowledge_chunk();
                continue;
            }
            std::pmr::vector<http_header_view> trailers(state_value.resource_);
            trailers.reserve(upload.trailers_.size());
            for (const auto& field : upload.trailers_) {
                trailers.emplace_back(field.name(), field.value());
            }
            // Without trailers, HTTP owns an empty or fixed-size last-chunk ending.
            // Keep that small storage in this operation; only trailers need a buffer.
            std::array<char, http1_last_chunk_prefix.size() + http1_trailer_section_terminator.size()> inline_ending;
            http_io_buffer scratch(state_value.resource_);
            const auto finish_buffer = trailers.empty() ? std::span<char>(inline_ending)
                                                        : scratch.prepare(max_client_request_head_bytes);
            const auto ending = writer.plan_finish(finish_buffer, trailers);
            if (ending.index() != 0) {
                throw http_client_error(
                    http_client_error::code_type::invalid_request, "HTTP upload trailer or final length rejected");
            }
            struct completion_guard final {
                http_client_upload_state& upload_;
                explicit completion_guard(http_client_upload_state& value)
                    : upload_(value) {
                    upload_.output_.completion_pending_ = true;
                }
                ~completion_guard() {
                    upload_.output_.completion_pending_ = false;
                    upload_.output_.space_.notify();
                }
            } completion(upload);
            if (const auto bytes = std::get<0>(ending); !bytes.empty()) {
                co_await write(single_part(bytes), timeout);
            }
            writer.commit_finish();
            const auto completed = parser.complete_request_content();
            if (completed == http1_client_request_content_completion_status::exchange_terminal && !state_value.head_ready_) {
                // The receiver already failed this exchange after the parser
                // left await_response; it owns and reports that failure.
                co_return;
            }
            upload.output_.finish();
            co_return;
        }
    } catch (...) {
        const auto upload_failure = std::current_exception();
        // TCP directions are independent: a peer can reject more request
        // bytes while a complete final response is already on its way. Keep
        // reading after a plain transport write failure; framing decides
        // whether that response is valid. Timeout, cancellation, local
        // request errors and explicit abandonment still terminate both lanes.
        const bool preserve_response = !state_value.abandoned_ &&
                                       connection_.transport_.abort_reason() == client_abort_reason::none &&
                                       is_upload_io_failure(upload_failure);
        if (preserve_response) {
            upload.failure_ = upload_failure;
            if (!state_value.complete_ && !upload.output_.stopped_) {
                failure = upload_failure;
            }
        } else if (!state_value.complete_ && !upload.output_.stopped_) {
            failure = upload_failure;
            // The caller's pending write() wakes on stop() below and reports
            // the published exchange failure instead of a bare cancellation.
            if (state_value.failure_ == nullptr) {
                state_value.failure_ = failure;
            }
        }
        upload.output_.stop();
        if (!preserve_response) {
            connection_.close();
        }
    }
}

}  // namespace ruvia::detail
