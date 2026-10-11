#include "http1/http1_client_exchange.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/ssl/error.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_body_decoder.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_request_content_writer.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_body_decoding.h"

#include "body/http_body_buffer.h"
#include "client/http_client_connection.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"
#include "http/http_socket_tunnel_transport.h"
#include "http/tls_tunnel_output.h"

namespace ruvia::detail {

task<void> http1_client_exchange::run(const http_client_request_storage& request, const operation_timeout& timeout,
    stop_token stop_token_value, http_client_response_state& state_value) {
    auto& connection = connection_;
    connection.transport_.reset_abort();
    connection.generation_ = connection.generation_ + 1 == 0 ? 1 : connection.generation_ + 1;
    struct exchange_generation final {
        http_client_connection& connection_;
        ~exchange_generation() {
            ++connection_.generation_;
            connection_.http1_response_ = nullptr;
        }
    } exchange_generation_value{connection};
    worker_cancellation_registration cancellation(pool_.cancellation_target(), connection.cancellation_id_);
    state_value.cancellation_id_ = cancellation.id();
    cancellation.arm(stop_token_value);
    connection.http1_response_ = &state_value;
    co_await exchange(request, timeout, state_value);
}

task<void> http1_client_exchange::write(std::string_view bytes_value, const operation_timeout& timeout) {
    const auto written =
        co_await connection_.transport_.write(bytes_value, timeout.constrained_by(pool_.config().write_timeout_));
    if (!written.ok()) {
        throw_http_client_transport_error(written, "upstream closed while receiving the HTTP request");
    }
}

task<std::size_t> http1_client_exchange::read_some(
    std::span<char> bytes_value, const operation_timeout& timeout, bool allow_eof) {
    const auto read = co_await connection_.transport_.read_some(bytes_value, timeout);
    if (read.ok()) {
        co_return read.bytes_;
    }
    if (allow_eof && read.status_ == client_transport_status::eof) {
        co_return 0;
    }
    throw_http_client_transport_error(read, "upstream closed the HTTP connection");
}

task<void> http1_client_exchange::exchange(
    const http_client_request_storage& request, const operation_timeout& timeout, http_client_response_state& state_value) {
    state_value.transport_ = http_client_response_transport::http1;
    auto* const response_resource = state_value.resource_;
    std::pmr::vector<http_header_view> headers(pool_.resource());
    auto source_value = http_client_request_storage_access::view(request, headers);
    std::pmr::string cookie_header(pool_.resource());
    pool_.policy().append_headers(request, headers, cookie_header);
    source_value.headers_ = std::span<const http_header_view>(headers);

    auto& write_buffer = connection_.write_buffer_;
    write_buffer.resize(max_http_header_bytes + 1024);
    auto origin = pool_.origin();
    const bool connect_udp = request.is_tunnel() && request.tunnel_protocol() == "connect-udp";
    if (connect_udp) {
        const auto authority = parse_http_authority(borrowed_text(request.tunnel_authority()));
        if (!authority) {
            throw http_client_error(http_client_error::code_type::invalid_request, "invalid CONNECT-UDP authority");
        }
        const http_origin_options options{.host_ = authority->host(), .port_ = authority->port()};
        origin = pool_.config().scheme_ == http_scheme::https ? http_origin_view::https(options)
                                                              : http_origin_view::http(options);
    }
    if (request.is_tunnel() && !request.tunnel_protocol().empty() && !connect_udp) {
        throw http_client_error(
            http_client_error::code_type::protocol_unavailable, "Extended CONNECT requires HTTP/2 or HTTP/3");
    }
    http1_client_request_writer writer({.resource_ = response_resource});
    const auto buffer = std::span<char>(write_buffer);
    auto prepared_result =
        connect_udp           ? writer.prepare_connect_udp(origin, source_value.target_, headers, buffer)
        : request.is_tunnel() ? writer.prepare_connect(borrowed_text(request.tunnel_authority()), headers, buffer)
        : request.upload() != nullptr
            ? writer.prepare_streaming(origin,
                  {.method_ = source_value.method_,
                      .target_ = source_value.target_,
                      .headers_ = headers,
                      .content_length_ = request.upload()->config_.content_length_},
                  buffer, {.expectation_ = request.upload()->config_.expectation_})
            : writer.prepare(origin, source_value, buffer);
    const auto* prepared = prepared_result.prepared();
    if (!prepared) {
        throw http_client_error(http_client_error::code_type::invalid_request,
            prepared_result.failure()
                ? std::string(http1_client_request_prepare_error_message(prepared_result.failure()->error()))
                : "HTTP request head is too large");
    }
    http1_client_response_parser parser(prepared->exchange_state(), {.resource_ = response_resource});
    co_await write(prepared->head(), timeout);
    if (const auto* content = prepared->content_plan().immediate()) {
        co_await write(content->bytes(), timeout);
        if (!content->bytes().empty() &&
            parser.complete_request_content() != http1_client_request_content_completion_status::completed) {
            std::terminate();
        }
    }

    auto* upload = request.upload();
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
    upload->output_.stop();
    if (receive_failure || !upload->output_.ended_) {
        connection_.close();
    }
    co_await writers.join();
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
}

task<void> http1_client_exchange::receive(const http_client_request_storage& request, const operation_timeout& timeout,
    http_client_response_state& state_value, http1_client_response_parser& parser) {
    auto* const response_resource = state_value.resource_;
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    auto& read_buffer = connection_.read_buffer_;
    std::array<char, 16384> input{};
    for (;;) {
        auto parse_result = parser.parse(read_buffer);
        if (parse_result.failure()) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                std::string(http1_client_response_parse_error_message(parse_result.failure()->error())));
        }
        if (parse_result.need_more()) {
            if (read_buffer.size() >= max_http_header_bytes) {
                throw http_client_error(http_client_error::code_type::protocol_error, "HTTP response head is too large");
            }
            const auto bytes_value = co_await read_some(input, timeout, true);
            if (bytes_value == 0) {
                throw http_client_error(
                    http_client_error::code_type::io_error, "upstream closed before the HTTP response head");
            }
            read_buffer.append(input.data(), bytes_value);
            continue;
        }
        auto* parsed_value = parse_result.parsed();
        if (!parsed_value) {
            std::terminate();
        }
        const auto consumed_head = parsed_value->consumed_bytes();
        if (parsed_value->plan().informational()) {
            state_value.retain_informational(parsed_value->head().status(), parsed_value->head().headers());
            if (request.upload() != nullptr &&
                parsed_value->plan().request_content_signal() == http_client_request_content_signal::continue_value) {
                request.upload()->content_released_ = true;
                request.upload()->output_.notify_data();
            }
            const bool closes_exchange =
                parsed_value->plan().informational()->persistence() == http1_close_policy::close_after_response;
            read_buffer.erase(0, consumed_head);
            if (closes_exchange) {
                connection_.close();
                throw http_client_error(http_client_error::code_type::protocol_error,
                    "upstream closed the HTTP exchange after an informational response");
            }
            continue;
        }
        if (request.upload() != nullptr && !request.upload()->output_.ended_) {
            request.upload()->output_.stop();
        }
        state_value.status_ = parsed_value->head().status();
        state_value.protocol_version_ = parsed_value->head().protocol_version();
        state_value.request_method_ = classify_http_method(request.method());
        state_value.response_body_plan_ = plan_http_response_body(state_value.request_method_, state_value.status_);
        state_value.headers_.reserve(parsed_value->head().headers().size());
        for (const auto& header : parsed_value->head().headers()) {
            state_value.headers_.push_back(http_header::copy_of(header.name(), header.value(), response_resource));
        }
        read_buffer.erase(0, consumed_head);
        if (request.is_tunnel() && (parsed_value->plan().connect_tunnel() != nullptr ||
                                       (request.tunnel_protocol() == "connect-udp" &&
                                           parsed_value->plan().protocol_upgrade() != nullptr))) {
            if (request.tunnel_protocol() == "connect-udp") {
                std::pmr::vector<http_header_view> fields(response_resource);
                fields.reserve(state_value.headers_.size());
                for (const auto& field : state_value.headers_) {
                    fields.emplace_back(field.name(), field.value());
                }
                if (validate_http_connect_udp_response(state_value.protocol_version_, state_value.status_.value(), fields)
                        .index() != 0) {
                    throw http_client_error(
                        http_client_error::code_type::protocol_error, "invalid CONNECT-UDP response head");
                }
            }
            state_value.tunnel_->accepted_ = true;
            state_value.head_ready_ = true;
            state_value.head_signal_.notify();
            co_await run_tunnel(state_value, timeout);
            co_return;
        }
        if (request.tunnel() != nullptr) {
            request.tunnel()->output_.stop();
        }
        if (parsed_value->plan().connect_tunnel() != nullptr || parsed_value->plan().protocol_upgrade() != nullptr) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP tunnel and protocol upgrade responses require a dedicated API");
        }
        http1_client_response_body_decoder body_decoder(parsed_value->plan(), response_resource);
        const bool content_semantics_present = parsed_value->plan().without_content() == nullptr;
        configure_http_client_response_decoding(state_value);
        state_value.head_ready_ = true;
        state_value.head_signal_.notify();

        const auto append_output = [&](std::string_view bytes_value) {
            const auto retained = state_value.buffered_.size() - state_value.offset_ + state_value.pending_.size();
            if (state_value.collect_all_ &&
                bytes_value.size() > max_response_bytes - std::min(retained, max_response_bytes)) {
                throw http_client_error(
                    http_client_error::code_type::response_too_large, "HTTP response exceeds configured byte limit");
            }
            // The consumer's last borrowed view lives in buffered, not pending.
            // Appending producer output must never relocate that view.
            state_value.pending_.append(bytes_value);
            state_value.data_signal_.notify();
        };
        const auto retain_trailers = [&](std::string_view trailer_block) {
            const auto ok = visit_http_response_trailers(trailer_block, [&](std::string_view name, std::string_view value) {
                state_value.trailers_.push_back(http_header::copy_of(name, value, response_resource));
                return true;
            });
            if (!ok) {
                throw std::logic_error("HTTP decoder published invalid response trailers");
            }
        };
        const auto wait_for_buffer_space = [&]() -> task<void> {
            while (!state_value.collect_all_ && state_value.pending_.size() >= max_response_bytes) {
                connection_.throw_if_aborted();
                const auto remaining = timeout.remaining();
                if (remaining && remaining->count() == 0) {
                    throw http_client_error(
                        http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
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
                    throw http_client_error(
                        http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
                }
                if (state_value.abandoned_) {
                    throw http_client_error(http_client_error::code_type::cancelled, "HTTP response body was abandoned");
                }
            }
        };

        std::array<char, http_body_buffer_bytes> output{};
        bool eof = false;
        http1_close_policy persistence = http1_close_policy::close_after_response;
        for (;;) {
            // Backpressure applies to decoding buffered compressed input too,
            // not just to the next transport read. One step cannot overfill the
            // producer queue; read_all retains its separate total byte policy.
            co_await wait_for_buffer_space();
            connection_.throw_if_aborted();
            const auto capacity = state_value.collect_all_
                                      ? output.size()
                                      : std::min(output.size(), max_response_bytes - state_value.pending_.size());
            const auto scratch = std::span<char>(output).first(capacity);
            const auto decoded =
                eof ? body_decoder.finish_input(read_buffer, scratch) : body_decoder.decode(read_buffer, scratch);
            const auto consumed = decoded.consumed_bytes();
            if (const auto* body = decoded.output()) {
                append_output(body->bytes());
            } else if (const auto* trailers = decoded.trailers()) {
                retain_trailers(trailers->bytes());
            } else if (const auto* failure = decoded.protocol_failure()) {
                throw http_client_error(http_client_error::code_type::protocol_error,
                    std::string(http1_client_response_body_error_message(failure->error())));
            } else if (decoded.decoder_failure() != nullptr) {
                throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/1 response body decoder failed");
            }
            // Output/trailers have been owned before their source is compacted.
            read_buffer.erase(0, consumed);
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
            const auto bytes_value = co_await read_some(input, timeout, true);
            if (bytes_value == 0) {
                eof = true;
            } else {
                read_buffer.append(input.data(), bytes_value);
            }
        }
        if (!read_buffer.empty()) {
            throw http_client_error(http_client_error::code_type::protocol_error, "unexpected bytes after HTTP response");
        }
        if (timeout.expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
        }
        decode_http_client_response_body(state_value, content_semantics_present, max_response_bytes);
        if (timeout.expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
        }
        if (persistence == http1_close_policy::close_after_response) {
            connection_.close();
        }
        co_return;
    }
}

task<void> http1_client_exchange::run_tunnel(http_client_response_state& state_value, const operation_timeout& timeout) {
    auto& connection = connection_;
    auto& output = state_value.tunnel_->output_;
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    const bool secure = connection.transport_.secure();
    auto& stream = connection.transport_.stream();
    std::optional<tls_tunnel_output> tls;
    if (secure) {
        tls.emplace(*stream.native_handle(), stream.next_layer(), pool_.worker(), *pool_.resource());
        tls->start();
    }
    const auto expire = [&connection](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired) {
            connection.transport_.abort(client_abort_reason::timeout);
            connection.abort_http1_response();
            connection.close();
        }
    };
    worker_timer_registration lifetime_timer;
    if (const auto remaining = timeout.remaining()) {
        (void)pool_.worker().schedule_timer(lifetime_timer, worker_timer_deadline_after(*remaining), expire);
    }
    task_scope writers(pool_.worker(), {.resource_ = pool_.resource()});
    std::exception_ptr writer_failure;
    const auto send = [&]() -> task<void> {
        try {
            while (!output.ended_ && !output.stopped_) {
                if (!output.chunk_ready_ && !output.end_requested_) {
                    co_await output.data_.wait();
                    continue;
                }
                worker_timer_registration write_timer;
                if (const auto remaining = timeout.constrained_by(pool_.config().write_timeout_).remaining()) {
                    (void)pool_.worker().schedule_timer(write_timer, worker_timer_deadline_after(*remaining), expire);
                }
                const auto ending = output.end_requested_ && !output.chunk_ready_ ? http_stream_end::end
                                                                                  : http_stream_end::keep_open;
                const auto bytes_value = output.chunk_ready_ ? std::string_view(output.chunk_) : std::string_view{};
                std::error_code error;
                if (tls) {
                    http_socket_tunnel_transport transport(stream, &*tls);
                    error = co_await transport.write_bytes(bytes_value, ending);
                } else {
                    http_socket_tunnel_transport transport(stream.next_layer());
                    error = co_await transport.write_bytes(bytes_value, ending);
                }
                write_timer.cancel();
                connection.throw_if_aborted();
                if (error) {
                    const bool tls_error = secure && (error.category() == asio::error::get_ssl_category() ||
                                                         error == asio::ssl::error::stream_truncated);
                    throw http_client_error(
                        tls_error ? http_client_error::code_type::tls_failed : http_client_error::code_type::io_error,
                        error.message());
                }
                pool_.wire_counters().sent_ += bytes_value.size();
                if (ending == http_stream_end::end) {
                    output.finish();
                } else {
                    output.acknowledge_chunk();
                }
            }
        } catch (...) {
            writer_failure = std::current_exception();
            connection.close();
            output.stop();
        }
    };
    writers.spawn(send());
    std::exception_ptr receive_failure;
    try {
        auto& read_buffer = connection.read_buffer_;
        std::array<char, 4096> input{};
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
                read_buffer.erase(0, count);
                state_value.data_signal_.notify();
                continue;
            }
            const auto count =
                co_await read_some(std::span<char>(input).first(std::min(input.size(), room)), timeout, true);
            if (count == 0) {
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
                break;
            }
            state_value.pending_.append(input.data(), count);
            state_value.data_signal_.notify();
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
            [&upload](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.output_.stopped_) {
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
                co_await write(std::string_view(planned.prefix_.data(), planned.prefix_size_), timeout);
                if (upload.output_.stopped_) {
                    writer.abort();
                    co_return;
                }
                co_await write(std::string_view(planned.payload_.data(), planned.payload_.size()), timeout);
                co_await write(planned.suffix_, timeout);
                if (writer.commit_chunk(planned.payload_.size()).index() != 0) {
                    std::terminate();
                }
                upload.output_.acknowledge_chunk();
                continue;
            }
            std::pmr::vector<http_header_view> trailers(state_value.resource_);
            trailers.reserve(upload.trailers_.size());
            for (const auto& field : upload.trailers_) {
                trailers.emplace_back(field.name(), field.value());
            }
            std::pmr::vector<char> scratch(max_http_header_bytes + 1024, state_value.resource_);
            const auto ending = writer.plan_finish(scratch, trailers);
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
            co_await write(std::get<0>(ending), timeout);
            if (writer.commit_finish().index() != 0) {
                std::terminate();
            }
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
        // A receiver, timeout, or cancellation stops output before it closes
        // the socket; the resulting write abort is a consequence, not the cause.
        if (!state_value.head_ready_ && !upload.output_.stopped_) {
            failure = std::current_exception();
        }
        upload.output_.stop();
        connection_.close();
    }
}

}  // namespace ruvia::detail
