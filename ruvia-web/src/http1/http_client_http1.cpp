#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/core/async.h"
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
#include "client/client_transport.h"
#include "client/http_client_config_validation.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "http/http_socket_tunnel_transport.h"
#include "http/tls_tunnel_output.h"

namespace ruvia::detail {
task<void> http_client_pool::execute_http1(connection_type& connection,
    const http_client_request_storage& request, const ruvia::operation_timeout& timeout,
    http_client_response& response) {
    response.state_->transport_ = http_client_response_transport::http1;
    auto* response_resource = response.state_->resource_;
    std::pmr::vector<http_header_view> headers(resource_);
    auto source_value = http_client_request_storage_access::view(request, headers);
    std::pmr::string cookie_header(resource_);
    policy_.append_headers(request, headers, cookie_header);
    source_value.headers_ = std::span<const http_header_view>(headers);

    connection.write_buffer_.resize(max_http_header_bytes + 1024);
    const auto wire_host = client_uri_host(config_.host_, resource_);
    auto origin =
        config_.scheme_ == http_scheme::https
            ? http_origin_view::https({.host_ = wire_host, .port_ = http_client_port(config_)})
            : http_origin_view::http({.host_ = wire_host, .port_ = http_client_port(config_)});
    if (request.is_tunnel() && request.tunnel_protocol() == "connect-udp") {
        const auto authority = parse_http_authority(borrowed_text(request.tunnel_authority()));
        if (!authority) {
            throw http_client_error(http_client_error::code_type::invalid_request, "invalid CONNECT-UDP authority");
        }
        origin = config_.scheme_ == http_scheme::https ? http_origin_view::https({.host_ = authority->host_, .port_ = authority->port_})
                                                       : http_origin_view::http({.host_ = authority->host_, .port_ = authority->port_});
    }
    if (request.is_tunnel() && !request.tunnel_protocol().empty() && request.tunnel_protocol() != "connect-udp") {
        throw http_client_error(http_client_error::code_type::protocol_unavailable, "Extended CONNECT requires HTTP/2 or HTTP/3");
    }
    auto prepared_result = request.is_tunnel() && request.tunnel_protocol() == "connect-udp"
                               ? http1_client_request_writer({.resource_ = response_resource}).prepare_connect_udp(origin, source_value.target_, headers, std::span<char>(connection.write_buffer_))
                           : request.is_tunnel()
                               ? http1_client_request_writer({.resource_ = response_resource}).prepare_connect(borrowed_text(request.tunnel_authority()), headers, std::span<char>(connection.write_buffer_))
                           : request.upload() != nullptr
                               ? http1_client_request_writer({.resource_ = response_resource}).prepare_streaming(origin, {.method_ = source_value.method_, .target_ = source_value.target_, .headers_ = headers, .content_length_ = request.upload()->config_.content_length_}, std::span<char>(connection.write_buffer_), {.expectation_ = request.upload()->config_.expectation_})
                               : http1_client_request_writer({.resource_ = response_resource}).prepare(origin, source_value, std::span<char>(connection.write_buffer_));
    const auto* prepared = prepared_result.prepared();
    if (!prepared) {
        throw http_client_error(http_client_error::code_type::invalid_request,
            prepared_result.failure() ? std::string(http1_client_request_prepare_error_message(
                                            prepared_result.failure()->error()))
                                      : "HTTP request head is too large");
    }
    http1_client_response_parser parser(prepared->exchange_state(), {.resource_ = response_resource});
    co_await connection.transport_.write(prepared->head(), timeout);
    if (const auto* content = prepared->content_plan().immediate()) {
        co_await connection.transport_.write(content->bytes(), timeout);
        if (!content->bytes().empty() &&
            parser.complete_request_content() !=
                http1_client_request_content_completion_status::completed) {
            std::terminate();
        }
    }

    if (auto* upload = request.upload()) {
        task_scope writers(worker_, {.resource_ = resource_});
        std::exception_ptr writer_failure;
        writers.spawn(write_http1_upload(connection, *response.state_, timeout,
            http1_request_content_writer(*prepared->content_plan().streaming()), parser, writer_failure));
        std::exception_ptr receive_failure;
        try {
            co_await execute_http1_response(connection, request, timeout, response, parser);
        } catch (...) {
            receive_failure = std::current_exception();
        }
        upload->output_.stop();
        if (receive_failure || !upload->output_.ended_) {
            close(connection);
        }
        co_await writers.join();
        if (writer_failure) {
            std::rethrow_exception(writer_failure);
        }
        if (receive_failure) {
            std::rethrow_exception(receive_failure);
        }
        co_return;
    }
    co_await execute_http1_response(connection, request, timeout, response, parser);
}

task<void> http_client_pool::execute_http1_response(connection_type& connection,
    const http_client_request_storage& request, const operation_timeout& timeout,
    http_client_response& response, http1_client_response_parser& parser) {
    auto* response_resource = response.state_->resource_;
    std::array<char, 16384> input{};
    for (;;) {
        auto parse_result = parser.parse(connection.read_buffer_);
        if (parse_result.failure()) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                std::string(http1_client_response_parse_error_message(parse_result.failure()->error())));
        }
        if (parse_result.need_more()) {
            if (connection.read_buffer_.size() >= max_http_header_bytes) {
                throw http_client_error(
                    http_client_error::code_type::protocol_error, "HTTP response head is too large");
            }
            const auto bytes_value = co_await connection.transport_.read_some(input, timeout);
            if (bytes_value == 0) {
                throw http_client_error(http_client_error::code_type::io_error,
                    "upstream closed before the HTTP response head");
            }
            connection.read_buffer_.append(input.data(), bytes_value);
            continue;
        }
        auto* parsed_value = parse_result.parsed();
        if (!parsed_value) {
            std::terminate();
        }
        const auto consumed_head = parsed_value->consumed_bytes();
        if (parsed_value->plan().informational()) {
            std::pmr::vector<http_header_view> interim_headers(response_resource);
            for (const auto& field : parsed_value->head().headers()) {
                interim_headers.emplace_back(field.name(), field.value());
            }
            response.state_->retain_informational(parsed_value->head().status(), interim_headers);
            if (request.upload() != nullptr && parsed_value->plan().request_content_signal() == http_client_request_content_signal::continue_value) {
                request.upload()->content_released_ = true;
                request.upload()->output_.notify_data();
            }
            const bool closes_exchange = parsed_value->plan().informational()->persistence() ==
                                         http1_close_policy::close_after_response;
            connection.read_buffer_.erase(0, consumed_head);
            if (closes_exchange) {
                close(connection);
                throw http_client_error(http_client_error::code_type::protocol_error,
                    "upstream closed the HTTP exchange after an informational response");
            }
            continue;
        }
        if (request.upload() != nullptr && !request.upload()->output_.ended_) {
            request.upload()->output_.stop();
        }
        response.state_->status_ = parsed_value->head().status();
        response.state_->protocol_version_ = parsed_value->head().protocol_version();
        response.state_->request_method_ = classify_http_method(request.method());
        response.state_->response_body_plan_ =
            plan_http_response_body(response.state_->request_method_, response.state_->status_);
        response.state_->headers_.reserve(parsed_value->head().headers().size());
        for (const auto& header : parsed_value->head().headers()) {
            response.state_->headers_.push_back(
                http_header::copy_of(header.name(), header.value(), response_resource));
        }
        connection.read_buffer_.erase(0, consumed_head);
        if (request.is_tunnel() && (parsed_value->plan().connect_tunnel() != nullptr ||
                                       (request.tunnel_protocol() == "connect-udp" && parsed_value->plan().protocol_upgrade() != nullptr))) {
            if (request.tunnel_protocol() == "connect-udp") {
                std::pmr::vector<http_header_view> fields(response_resource);
                for (const auto& field : response.state_->headers_) {
                    fields.emplace_back(field.name(), field.value());
                }
                if ((validate_http_connect_udp_response(response.state_->protocol_version_, response.state_->status_.value(), fields).index() != 0)) {
                    throw http_client_error(http_client_error::code_type::protocol_error, "invalid CONNECT-UDP response head");
                }
            }
            response.state_->tunnel_->accepted_ = true;
            response.state_->head_ready_ = true;
            response.state_->head_signal_.notify();
            co_await execute_http1_tunnel(connection, *response.state_, timeout);
            co_return;
        }
        if (request.tunnel() != nullptr) {
            request.tunnel()->output_.stop();
        }
        if (parsed_value->plan().connect_tunnel() != nullptr ||
            parsed_value->plan().protocol_upgrade() != nullptr) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP tunnel and protocol upgrade responses require a dedicated API");
        }
        http1_client_response_body_decoder body_decoder(parsed_value->plan(), response_resource);
        const bool content_semantics_present = parsed_value->plan().without_content() == nullptr;
        configure_http_client_response_decoding(*response.state_);
        response.state_->head_ready_ = true;
        response.state_->head_signal_.notify();

        const auto append_output = [&](std::string_view bytes_value) {
            const auto retained = response.state_->buffered_.size() - response.state_->offset_ +
                                  response.state_->pending_.size();
            if (response.state_->collect_all_ &&
                bytes_value.size() >
                    config_.max_response_bytes_ - std::min(retained, config_.max_response_bytes_)) {
                throw http_client_error(http_client_error::code_type::response_too_large,
                    "HTTP response exceeds configured byte limit");
            }
            // The consumer's last borrowed view lives in buffered, not pending.
            // Appending producer output must never relocate that view.
            response.state_->pending_.append(bytes_value);
            response.state_->data_signal_.notify();
        };
        const auto retain_trailers = [&](std::string_view trailer_block) {
            const auto ok = visit_http_response_trailers(
                trailer_block, [&](std::string_view name, std::string_view value) {
                    response.state_->trailers_.push_back(
                        http_header::copy_of(name, value, response_resource));
                    return true;
                });
            if (!ok) {
                throw std::logic_error("HTTP decoder published invalid response trailers");
            }
        };
        const auto wait_for_buffer_space = [&]() -> task<void> {
            while (!response.state_->collect_all_ &&
                   response.state_->pending_.size() >= config_.max_response_bytes_) {
                connection.transport_.throw_if_aborted();
                if (!connection.transport_.arm_deadline(timeout, client_deadline_kind::response_buffer)) {
                    throw http_client_error(
                        http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
                }
                try {
                    co_await response.state_->space_signal_.wait();
                } catch (...) {
                    (void)connection.transport_.clear_deadline();
                    throw;
                }
                const bool timed_out = connection.transport_.clear_deadline() || timeout.expired();
                connection.transport_.throw_if_aborted();
                if (timed_out) {
                    throw http_client_error(
                        http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
                }
                if (response.state_->abandoned_) {
                    throw http_client_error(
                        http_client_error::code_type::cancelled, "HTTP response body was abandoned");
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
            connection.transport_.throw_if_aborted();
            const auto capacity = response.state_->collect_all_
                                      ? output.size()
                                      : std::min(output.size(),
                                            config_.max_response_bytes_ - response.state_->pending_.size());
            const auto scratch = std::span<char>(output).first(capacity);
            const auto decoded = eof
                                     ? body_decoder.finish_input(connection.read_buffer_, scratch)
                                     : body_decoder.decode(connection.read_buffer_, scratch);
            const auto consumed = decoded.consumed_bytes();
            if (const auto* body = decoded.output()) {
                append_output(body->bytes());
            } else if (const auto* trailers = decoded.trailers()) {
                retain_trailers(trailers->bytes());
            } else if (const auto* failure = decoded.protocol_failure()) {
                throw http_client_error(http_client_error::code_type::protocol_error,
                    std::string(http1_client_response_body_error_message(failure->error())));
            } else if (decoded.decoder_failure() != nullptr) {
                throw http_client_error(http_client_error::code_type::protocol_error,
                    "HTTP/1 response body decoder failed");
            }
            // Output/trailers have been owned before their source is compacted.
            connection.read_buffer_.erase(0, consumed);
            if (const auto* complete = decoded.complete()) {
                persistence = complete->persistence();
                break;
            }
            if (decoded.need_input() == nullptr ||
                (consumed != 0 && !connection.read_buffer_.empty())) {
                continue;
            }
            if (eof) {
                throw std::logic_error("HTTP body decoder requested input after EOF");
            }
            const auto bytes_value = co_await connection.transport_.read_some(input, timeout, true);
            if (bytes_value == 0) {
                eof = true;
            } else {
                connection.read_buffer_.append(input.data(), bytes_value);
            }
        }
        if (!connection.read_buffer_.empty()) {
            throw http_client_error(
                http_client_error::code_type::protocol_error, "unexpected bytes after HTTP response");
        }
        if (timeout.expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
        }
        decode_http_client_response_content_encoding(
            *response.state_, content_semantics_present, config_.max_response_bytes_);
        if (timeout.expired()) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP/1 response body decoding timed out");
        }
        if (persistence == http1_close_policy::close_after_response) {
            close(connection);
        }
        co_return;
    }
}

task<void> http_client_pool::execute_http1_tunnel(connection_type& connection, http_client_response_state& state_value, const operation_timeout& timeout) {
    auto& output = state_value.tunnel_->output_;
    std::optional<tls_tunnel_output> tls;
    if (config_.scheme_ == http_scheme::https) {
        tls.emplace(*connection.transport_.stream().native_handle(), connection.transport_.stream().next_layer(), worker_, *resource_);
        tls->start();
    }
    worker_timer_registration lifetime_timer;
    if (const auto remaining = timeout.remaining()) {
        (worker_).schedule_timer(lifetime_timer, worker_timer_deadline_after(*remaining), [this, &connection](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                connection.transport_.abort_output(client_abort_reason::timeout);
                close(connection);
            }
        });
    }
    task_scope writers(worker_, {.resource_ = resource_});
    std::exception_ptr writer_failure;
    const auto send = [&]() -> task<void> {
        try {
            while (!output.ended_ && !output.stopped_) {
                if (!output.chunk_ready_ && !output.end_requested_) {
                    co_await output.data_.wait();
                    continue;
                }
                worker_timer_registration write_timer;
                const auto write_timeout = timeout.constrained_by(config_.write_timeout_);
                if (const auto remaining = write_timeout.remaining()) {
                    (worker_).schedule_timer(write_timer, worker_timer_deadline_after(*remaining), [this, &connection](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            connection.transport_.abort_output(client_abort_reason::timeout);
                            close(connection);
                        }
                    });
                }
                const auto ending = output.end_requested_ && !output.chunk_ready_ ? http_stream_end::end : http_stream_end::keep_open;
                const auto bytes_value = output.chunk_ready_ ? std::string_view(output.chunk_) : std::string_view{};
                std::error_code error;
                if (tls) {
                    http_socket_tunnel_transport transport(connection.transport_.stream(), &*tls);
                    error = co_await transport.write_bytes(bytes_value, ending);
                } else {
                    http_socket_tunnel_transport transport(connection.transport_.stream().next_layer());
                    error = co_await transport.write_bytes(bytes_value, ending);
                }
                write_timer.cancel();
                connection.transport_.throw_if_aborted();
                if (error) {
                    throw http_client_error(connection.transport_.error_code(error), error.message());
                }
                wire_counters_.sent_ += bytes_value.size();
                if (ending == http_stream_end::end) {
                    output.finish();
                } else {
                    output.acknowledge_chunk();
                }
            }
        } catch (...) {
            writer_failure = std::current_exception();
            close(connection);
            output.stop();
        }
    };
    writers.spawn(send());
    std::exception_ptr receive_failure;
    try {
        std::array<char, 4096> input{};
        for (;;) {
            while (state_value.pending_.size() >= config_.max_response_bytes_ && !state_value.abandoned_) {
                co_await state_value.space_signal_.wait();
                connection.transport_.throw_if_aborted();
            }
            if (state_value.abandoned_) {
                throw http_client_error(http_client_error::code_type::cancelled, "CONNECT tunnel abandoned");
            }
            if (!connection.read_buffer_.empty()) {
                const auto count = std::min(connection.read_buffer_.size(), config_.max_response_bytes_ - state_value.pending_.size());
                state_value.pending_.append(connection.read_buffer_.data(), count);
                connection.read_buffer_.erase(0, count);
                state_value.data_signal_.notify();
                continue;
            }
            const auto count = co_await connection.transport_.read_some(std::span<char>(input).first(std::min(input.size(), config_.max_response_bytes_ - state_value.pending_.size())), timeout, true);
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
        close(connection);
        output.stop();
    }
    co_await writers.join();
    if (tls) {
        co_await tls->join();
    }
    lifetime_timer.cancel();
    close(connection);
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
}

task<void> http_client_pool::write_upload_bytes(connection_type& connection, std::string_view bytes_value, const operation_timeout& timeout) {
    if (bytes_value.empty()) {
        co_return;
    }
    const auto write_timeout = timeout.constrained_by(config_.write_timeout_);
    worker_timer_registration timer;
    if (const auto remaining = write_timeout.remaining()) {
        if (remaining->count() == 0) {
            throw http_client_error(http_client_error::code_type::timeout, "HTTP upload write timed out");
        }
        (worker_).schedule_timer(timer, worker_timer_deadline_after(*remaining), [&connection](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                connection.transport_.abort_output(client_abort_reason::timeout);
                if (connection.transport_.response() != nullptr) {
                    if (auto* output = connection.transport_.response()->output()) {
                        output->stop();
                    }
                }
                std::error_code ignored;
                connection.transport_.stream().lowest_layer().cancel(ignored);
            }
        });
    }
    const auto completion = config_.scheme_ == http_scheme::https
                                ? co_await async_asio<std::size_t>([&connection, bytes_value](auto handler) { asio::async_write(connection.transport_.stream(), asio::buffer(bytes_value), std::move(handler)); })
                                : co_await async_asio<std::size_t>([&connection, bytes_value](auto handler) { asio::async_write(connection.transport_.stream().next_layer(), asio::buffer(bytes_value), std::move(handler)); });
    timer.cancel();
    connection.transport_.throw_if_aborted();
    if (completion.error_code()) {
        throw http_client_error(connection.transport_.error_code(completion.error_code()), completion.error_code().message());
    }
    wire_counters_.sent_ += completion.result();
}

task<void> http_client_pool::write_http1_upload(connection_type& connection, http_client_response_state& state_value,
    const operation_timeout& timeout, http1_request_content_writer writer, http1_client_response_parser& parser,
    std::exception_ptr& failure) {
    auto& upload = *state_value.upload_;
    worker_timer_registration continue_timer;
    if (!upload.content_released_) {
        (worker_).schedule_timer(continue_timer, worker_timer_deadline_after(upload.config_.continue_timeout_), [&upload](worker_timer_outcome outcome) noexcept {
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
            connection.transport_.throw_if_aborted();
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
                if ((chunk.index() != 0)) {
                    throw http_client_error(http_client_error::code_type::invalid_request, "HTTP upload content length mismatch");
                }
                co_await write_upload_bytes(connection, std::string_view(std::get<0>(chunk).prefix_.data(), std::get<0>(chunk).prefix_size_), timeout);
                if (upload.output_.stopped_) {
                    writer.abort();
                    co_return;
                }
                co_await write_upload_bytes(connection, std::string_view(std::get<0>(chunk).payload_.data(), std::get<0>(chunk).payload_.size()), timeout);
                co_await write_upload_bytes(connection, std::get<0>(chunk).suffix_, timeout);
                if ((writer.commit_chunk(std::get<0>(chunk).payload_.size()).index() != 0)) {
                    std::terminate();
                }
                upload.output_.acknowledge_chunk();
                continue;
            }
            std::pmr::vector<http_header_view> trailers(state_value.resource_);
            for (const auto& field : upload.trailers_) {
                trailers.emplace_back(field.name(), field.value());
            }
            std::pmr::vector<char> scratch(max_http_header_bytes + 1024, state_value.resource_);
            const auto ending = writer.plan_finish(scratch, trailers);
            if ((ending.index() != 0)) {
                throw http_client_error(http_client_error::code_type::invalid_request, "HTTP upload trailer or final length rejected");
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
            co_await write_upload_bytes(connection, std::get<0>(ending), timeout);
            if ((writer.commit_finish().index() != 0)) {
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
        close(connection);
    }
}

}  // namespace ruvia::detail
