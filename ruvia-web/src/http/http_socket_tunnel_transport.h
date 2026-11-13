#pragma once

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_io_buffer.h"

#include "http/http_stream_end.h"
#include "http/tls_tunnel_output.h"
#include "server/connection_scanner.h"
#include "server/inbound_buffer_budget.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

template <typename stream_type>
class http_socket_tunnel_transport final {
public:
    // `write_scanner_entry` is the server connection's scanner entry: every
    // write runs its write stall clock, so write_timeout closes a tunnel
    // whose peer stopped reading while the reader stays long_lived. Null for
    // an owner that bounds its writes itself (the client's operation timeout).
    http_socket_tunnel_transport(
        stream_type& stream, tls_tunnel_output* tls_output, connection_scanner::entry* write_scanner_entry)
        : stream_(stream),
          tls_output_(tls_output),
          write_scanner_entry_(write_scanner_entry) {
        if constexpr (requires { stream_.next_layer(); }) {
            if (tls_output_ == nullptr) {
                throw std::logic_error("TLS tunnel requires its ciphertext writer");
            }
        }
    }
    // Appends up to 4096 bytes read from the stream to `bytes_value`. A caller
    // that clears and reuses one string reads without allocating after its
    // first read.
    [[nodiscard]] task<bool> read_more(std::string& bytes_value, inbound_buffer_reservation* reservation = nullptr) {
        return read_more(bytes_value, []() noexcept {}, asio::cancellation_slot{}, reservation);
    }

    // A cancellation emitted on `slot` ends only this transport read and
    // appends nothing, after which `check` may throw to abandon it.
    template <typename check_type>
    [[nodiscard]] task<bool> read_more(std::string& bytes_value, check_type check, asio::cancellation_slot slot,
        inbound_buffer_reservation* reservation = nullptr) {
        constexpr std::size_t read_bytes = 4096;
        const auto old_size = bytes_value.size();
        if (reservation != nullptr) {
            inbound_resize(bytes_value, *reservation, old_size + read_bytes);
        } else {
            bytes_value.resize(old_size + read_bytes);
        }
        const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
            stream_.async_read_some(asio::buffer(bytes_value.data() + old_size, read_bytes),
                asio::bind_cancellation_slot(slot, std::move(handler)));
        });
        if (reservation != nullptr) {
            inbound_resize(bytes_value, *reservation, old_size + result_value.result());
        } else {
            bytes_value.resize(old_size + result_value.result());
        }
        // A TLS peer that closed TCP without close_notify ends the tunnel byte
        // stream exactly like an orderly FIN or close_notify.
        if (is_stream_read_end(result_value.error_code())) {
            co_return false;
        }
        if (result_value.error_code()) {
            if (result_value.error_code() == asio::error::operation_aborted) {
                check();
            }
            throw inbound_transport_error(result_value.error_code(), "HTTP tunnel read");
        }
        co_return result_value.result() != 0;
    }

    // A cancellation emitted on `slot` ends only this transport read (a TLS
    // stream shields its record writes), after which `check` may throw to
    // abandon it.
    template <typename check_type>
    [[nodiscard]] task<bool> read_more(http_io_buffer& bytes, check_type check, asio::cancellation_slot slot,
        inbound_buffer_reservation* reservation = nullptr) {
        const auto writable = reservation != nullptr ? inbound_prepare(bytes, *reservation, 4096) : bytes.prepare(4096);
        const auto result = co_await async_asio<std::size_t>([&](auto completion) {
            stream_.async_read_some(asio::buffer(writable.data(), writable.size()),
                asio::bind_cancellation_slot(slot, std::move(completion)));
        });
        bytes.commit(result.result());
        if (is_stream_read_end(result.error_code())) {
            co_return false;
        }
        if (result.error_code()) {
            if (result.error_code() == asio::error::operation_aborted) {
                check();
            }
            throw inbound_transport_error(result.error_code(), "WebSocket read");
        }
        co_return result.result() != 0;
    }
    // `completed_bytes` receives the plaintext prefix accepted by the stream,
    // including progress reported with the terminal error.
    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end end, std::size_t* completed_bytes = nullptr) {
        if (completed_bytes != nullptr) {
            *completed_bytes = 0;
        }
        std::optional<connection_scanner::entry::write_scope> write_stall_clock;
        if (write_scanner_entry_ != nullptr) {
            write_stall_clock.emplace(*write_scanner_entry_);
        }
        if constexpr (!requires { stream_.next_layer(); }) {
            if (!bytes_value.empty()) {
                const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
                    if (write_scanner_entry_ != nullptr) {
                        asio::async_write(stream_, asio::buffer(bytes_value),
                            connection_write_progress(*write_scanner_entry_), std::move(handler));
                    } else {
                        asio::async_write(stream_, asio::buffer(bytes_value), std::move(handler));
                    }
                });
                if (completed_bytes != nullptr) {
                    *completed_bytes = result_value.result();
                }
                if (result_value.error_code()) {
                    co_return result_value.error_code();
                }
            }
        } else {
            // The ciphertext writer drains each chunk before the next is
            // encrypted, so a flushed chunk is the unit of progress.
            while (!bytes_value.empty()) {
                const auto chunk = bytes_value.substr(0, 4096);
                const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
                });
                if (completed_bytes != nullptr) {
                    *completed_bytes += result_value.result();
                }
                if (result_value.error_code()) {
                    co_return result_value.error_code();
                }
                if (const auto error = co_await tls_output_->flush()) {
                    co_return error;
                }
                if (write_scanner_entry_ != nullptr) {
                    write_scanner_entry_->note_write_progress();
                }
                bytes_value.remove_prefix(chunk.size());
            }
        }
        if (end == http_stream_end::end) {
            if constexpr (requires { stream_.next_layer(); }) {
                co_return co_await tls_output_->finish();
            } else {
                std::error_code error;
                stream_.shutdown(asio::ip::tcp::socket::shutdown_send, error);
                co_return error;
            }
        }
        co_return std::error_code{};
    }
    // HTTP/1.1 has no per-message reset: malformed tunnel data closes the
    // connection like any other abort.
    void abort(bool /*malformed*/ = false) noexcept {
        if constexpr (requires { stream_.next_layer(); }) {
            tls_output_->abort();
        } else {
            close_socket(stream_.lowest_layer());
        }
    }

private:
    stream_type& stream_;
    tls_tunnel_output* tls_output_;
    connection_scanner::entry* write_scanner_entry_;
};

}  // namespace ruvia::detail
