#pragma once

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"

#include "http/http_stream_read_result.h"
#include "http/tls_tunnel_output.h"
#include "tls/tls_stream_end.h"

namespace ruvia::detail {

template <typename stream_type>
class http_socket_tunnel_transport final {
public:
    explicit http_socket_tunnel_transport(stream_type& stream, tls_tunnel_output* tls_output = nullptr)
        : stream_(stream),
          tls_output_(tls_output) {
        if constexpr (requires { stream_.next_layer(); }) {
            if (tls_output_ == nullptr) {
                throw std::logic_error("TLS tunnel requires its ciphertext writer");
            }
        }
    }
    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& bytes_value) {
        const auto old_size = bytes_value.size();
        resize_pmr_string_for_overwrite(bytes_value, old_size + 4096);
        const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
            stream_.async_read_some(asio::buffer(bytes_value.data() + old_size, 4096), std::move(handler));
        });
        bytes_value.resize(old_size + result_value.result());
        // A TLS peer that closed TCP without close_notify ends the tunnel byte
        // stream exactly like an orderly FIN or close_notify.
        if (is_stream_read_end(result_value.error_code())) {
            co_return http_stream_read_result::make_end();
        }
        if (result_value.error_code()) {
            co_return http_stream_read_result::make_failure(result_value.error_code());
        }
        co_return result_value.result() == 0 ? http_stream_read_result::make_end() : http_stream_read_result::make_data();
    }
    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value, http_stream_end end) {
        while (!bytes_value.empty()) {
            const auto chunk = bytes_value.substr(0, 4096);
            const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
                asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
            });
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
    void abort() noexcept {
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
