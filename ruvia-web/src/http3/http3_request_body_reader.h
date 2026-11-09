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
class http3_request_body_reader final : public multiplexed_request_body_reader_base<http3_request_body_reader> {
public:
    static constexpr const char* terminated_error_message = "HTTP/3 connection ended during body read";
    static constexpr const char* reset_error_message = "HTTP/3 request stream reset during body read";

    // continue_output is non-null when the request carries a pending
    // 100-continue expectation: the first read sends it, as the HTTP/1 reader
    // does, so content the handler never reads is never solicited.
    http3_request_body_reader(http3_session& session, http3_stream& stream, multiplexed_stream_signal& signal,
        http_interim_response_output* continue_output) noexcept
        : multiplexed_request_body_reader_base(signal, continue_output),
          session_(session),
          stream_(stream) {}

private:
    friend class multiplexed_request_body_reader_base<http3_request_body_reader>;

    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const noexcept {
        return stream_.body_failure();
    }

    [[nodiscard]] bool stream_aborted() const noexcept {
        return stream_.aborted();
    }

    [[nodiscard]] std::string_view pop_chunk() noexcept {
        const auto [chunk, consumed] = stream_.pop();
        if (consumed != 0) {
            session_.consume_stream_data(stream_.id(), consumed);
        }
        return chunk;
    }

    [[nodiscard]] bool body_ended() const noexcept {
        return stream_.request_ended();
    }

    http3_session& session_;
    http3_stream& stream_;
};

}  // namespace ruvia::detail
