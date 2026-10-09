#pragma once

#include <cstddef>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"

#include "websocket/http_websocket_connection.h"

namespace ruvia::detail {

// HTTP/1.1 websocket transport: reads and writes the framed bytes directly on
// the upgraded byte stream (plain TCP socket or TLS stream).
template <typename stream_type>
class websocket_socket_transport final {
public:
    explicit websocket_socket_transport(stream_type& stream) noexcept
        : stream_(stream) {}

    [[nodiscard]] auto executor() const noexcept {
        return stream_.get_executor();
    }

    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        const auto old_size = buffer.size();
        ::ruvia::resize_pmr_string_for_overwrite(buffer, old_size + 4096);
        auto read_completion = co_await ruvia::async_asio<std::size_t>([this, old_size, &buffer](
                                                                           auto handler) mutable {
            stream_.async_read_some(
                asio::buffer(buffer.data() + old_size, buffer.size() - old_size), std::move(handler));
        });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        if (ec) {
            buffer.resize(old_size);
            co_return http_stream_read_result::make_failure(ec);
        }
        if (bytes_read == 0) {
            buffer.resize(old_size);
            co_return http_stream_read_result::make_end();
        }
        buffer.resize(old_size + bytes_read);
        co_return http_stream_read_result::make_data();
    }

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, websocket_transport_disposition /*disposition*/) {
        if (bytes_value.empty()) {
            co_return std::error_code{};
        }
        const auto buffer = asio::buffer(bytes_value.data(), bytes_value.size());
        const auto write_completion = co_await ruvia::async_asio([this, buffer](auto handler) mutable {
            asio::async_write(stream_, buffer, std::move(handler));
        });
        co_return write_completion.error_code();
    }

    void abort() noexcept {
        if constexpr (requires(stream_type& value) { value.next_layer(); }) {
            ruvia::close_socket(stream_.next_layer());
        } else {
            ruvia::close_socket(stream_);
        }
    }

private:
    stream_type& stream_;
};

template <typename stream_type>
using socket_websocket_connection_type = websocket_connection<websocket_socket_transport<stream_type>>;

}  // namespace ruvia::detail
