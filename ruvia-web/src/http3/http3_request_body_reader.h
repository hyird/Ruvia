#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"

#include "context/http_interim_response_output.h"
#include "http3/http3_session.h"
#include "http3/http3_stream.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

// Streaming request-body reader of one HTTP/3 request stream; the body_reader
// facade wraps it for the handler. Each read hands out every byte queued
// since the previous read and returns the previous chunk's receive
// flow-control credit (RFC 9000 §4.1): the peer can send more only once the
// handler has consumed what it sent.
class http3_request_body_reader final {
public:
    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so content the handler never reads is never solicited.
    http3_request_body_reader(http3_session& session, http3_stream& stream, http3_stream_signal& signal,
        http_interim_response_output* continue_output) noexcept
        : session_(session),
          stream_(stream),
          signal_(signal),
          continue_output_(continue_output) {}

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() {
        if (const auto& failure = stream_.body_failure(); failure.has_value()) {
            throw http_request_body_protocol_error(*failure);
        }
        if (auto* continue_output = std::exchange(continue_output_, nullptr)) {
            co_await continue_output->send_continue();
        }
        for (;;) {
            if (const auto& failure = stream_.body_failure(); failure.has_value()) {
                throw http_request_body_protocol_error(*failure);
            }
            if (signal_.terminated()) {
                throw inbound_transport_error(signal_.terminal_error(), "HTTP/3 connection ended during body read");
            }
            if (stream_.aborted()) {
                throw inbound_transport_error(
                    std::make_error_code(std::errc::connection_reset), "HTTP/3 request stream reset during body read");
            }
            const auto [chunk, consumed] = stream_.pop();
            if (consumed != 0) {
                session_.consume_stream_data(stream_.id(), consumed);
            }
            if (!chunk.empty()) {
                co_return ::ruvia::as_bytes(chunk);
            }
            if (stream_.request_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

private:
    http3_session& session_;
    http3_stream& stream_;
    http3_stream_signal& signal_;
    http_interim_response_output* continue_output_;
};

}  // namespace ruvia::detail
