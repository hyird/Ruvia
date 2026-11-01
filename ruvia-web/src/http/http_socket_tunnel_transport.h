#pragma once

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"

#include "http/tls_tunnel_output.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"

namespace ruvia::detail {

template <typename stream_type>
class http_socket_tunnel_transport final {
public:
    explicit http_socket_tunnel_transport(stream_type& stream, tls_tunnel_output* tls_output)
        : stream_(stream),
          tls_output_(tls_output) {
        if constexpr (requires { stream_.next_layer(); }) {
            if (tls_output_ == nullptr) {
                throw std::logic_error("TLS tunnel requires its ciphertext writer");
            }
        }
    }
    // Appends up to 4096 bytes read from the stream to `bytes_value`. A caller
    // that clears and reuses one string reads without allocating after its
    // first read.
    [[nodiscard]] task<bool> read_more(std::pmr::string& bytes_value) {
        constexpr std::size_t read_bytes = 4096;
        const auto old_size = bytes_value.size();
        bytes_value.resize(old_size + read_bytes);
        const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
            stream_.async_read_some(asio::buffer(bytes_value.data() + old_size, read_bytes), std::move(handler));
        });
        bytes_value.resize(old_size + result_value.result());
        // A TLS peer that closed TCP without close_notify ends the tunnel byte
        // stream exactly like an orderly FIN or close_notify.
        if (is_stream_read_end(result_value.error_code())) {
            co_return false;
        }
        if (result_value.error_code()) {
            throw inbound_transport_error(result_value.error_code(), "HTTP tunnel read");
        }
        co_return result_value.result() != 0;
    }
    // `completed_bytes` receives the plaintext prefix accepted by the stream,
    // including progress reported with the terminal error.
    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end end, std::size_t* completed_bytes = nullptr) {
        if (completed_bytes != nullptr) {
            *completed_bytes = 0;
        }
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
            if constexpr (requires { stream_.next_layer(); }) {
                if (const auto error = co_await tls_output_->flush()) {
                    co_return error;
                }
            }
            bytes_value.remove_prefix(chunk.size());
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
            close_socket(stream_);
        }
    }

private:
    stream_type& stream_;
    tls_tunnel_output* tls_output_;
};

}  // namespace ruvia::detail
