#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/http_tunnel.h"

#include "http3/http3_session.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"
#include "util/steady_clock.h"

namespace ruvia::detail {

// One duplex HTTP/3 request stream. The session remains its sole QUIC reader
// and writer; this adapter returns flow credit only after consuming DATA.
class http3_tunnel_transport final {
public:
    http3_tunnel_transport(http3_session& session, http3_stream& stream, http3_stream_signal& signal) noexcept
        : session_(session),
          stream_(stream),
          signal_(signal) {}

    template <typename buffer_type>
    [[nodiscard]] task<bool> read_more(buffer_type& buffer) {
        read_deadline deadline(stream_);
        for (;;) {
            require_live();
            auto chunk = stream_.take_tunnel_bytes();
            if (!chunk.empty()) {
                const auto consumed = chunk.size();
                buffer.append(chunk);
                std::pmr::string(chunk.get_allocator()).swap(chunk);
                session_.consume_stream_data(stream_.id(), consumed);
                co_return true;
            }
            if (stream_.request_ended()) {
                co_return false;
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<std::optional<http_tunnel_input>> read_datagram() {
        read_deadline deadline(stream_);
        for (;;) {
            require_live();
            // Reliable input is not starved by a native datagram flood.
            auto chunk = stream_.take_tunnel_bytes();
            if (!chunk.empty()) {
                session_.consume_stream_data(stream_.id(), chunk.size());
                co_return http_tunnel_input{std::move(chunk), false};
            }
            if (auto payload = stream_.pop_datagram()) {
                co_return http_tunnel_input{std::move(*payload), true};
            }
            if (stream_.request_ended()) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<bool> write_datagram(std::string_view payload) {
        // The QUIC owner checks the negotiated frame size and current path MTU;
        // invalid_state selects reliable capsule fallback without consuming it.
        output_deadline deadline(signal_);
        for (;;) {
            require_live();
            const auto result = session_.submit_datagram(stream_.id(), std::as_bytes(std::span(payload.data(), payload.size())));
            if (result == http3_quic_status::accepted) {
                co_return true;
            }
            if (result == http3_quic_status::invalid_state) {
                co_return false;
            }
            if (result != http3_quic_status::would_block) {
                throw inbound_transport_error(std::make_error_code(std::errc::connection_reset), "HTTP/3 datagram write");
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes, http_stream_end disposition) {
        const auto result = co_await write_http3_stream_data(session_, stream_.id(), signal_, bytes);
        if (result != http3_data_write_status::completed || signal_.stopped() || stream_.aborted()) {
            co_return signal_.terminated() ? signal_.terminal_error() : std::make_error_code(std::errc::connection_reset);
        }
        if (disposition == http_stream_end::end && session_.codec().finish_stream(stream_.id()) != http3_status::accepted) {
            co_return std::make_error_code(std::errc::protocol_error);
        }
        session_.wake_writer();
        co_return std::error_code{};
    }

    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes, websocket_transport_disposition disposition) {
        return write_bytes(bytes, disposition == websocket_transport_disposition::end_transport
                                      ? http_stream_end::end
                                      : http_stream_end::keep_open);
    }

    void abort(bool malformed = false) noexcept {
        session_.reset_stream(stream_.id(), malformed ? http3_error_code::message_error : http3_error_code::request_cancelled);
    }

private:
    struct read_deadline final {
        http3_stream& stream_;
        explicit read_deadline(http3_stream& stream)
            : stream_(stream) {
            if (!stream_.request_ended()) {
                stream_.mark_request_body_open(steady_now_ms());
            }
        }
        ~read_deadline() {
            stream_.close_request_body_timing();
        }
    };
    struct output_deadline final {
        http3_stream_signal& signal_;
        explicit output_deadline(http3_stream_signal& signal)
            : signal_(signal) {
            signal_.set_waiting_output(true);
        }
        ~output_deadline() {
            signal_.set_waiting_output(false);
        }
    };
    void require_live() const {
        if (signal_.stopped() || stream_.aborted()) {
            throw inbound_transport_error(signal_.terminated() ? signal_.terminal_error()
                                                               : std::make_error_code(std::errc::connection_reset),
                "HTTP/3 tunnel read");
        }
    }

    http3_session& session_;
    http3_stream& stream_;
    http3_stream_signal& signal_;
};

}  // namespace ruvia::detail
