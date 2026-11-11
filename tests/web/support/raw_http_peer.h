#pragma once

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

namespace ruvia::test {

template <typename stream_type>
[[nodiscard]] inline std::string read_to_end(stream_type& stream, asio::error_code& terminal_error) {
    std::string result;
    std::array<char, 4096> bytes;
    terminal_error.clear();
    while (!terminal_error) {
        const auto count = stream.read_some(asio::buffer(bytes), terminal_error);
        result.append(bytes.data(), count);
    }
    return result;
}

template <typename stream_type>
[[nodiscard]] inline std::string read_to_end(stream_type& stream) {
    asio::error_code error;
    auto result = read_to_end(stream, error);
    if (error != asio::error::eof && error != asio::ssl::error::stream_truncated) {
        throw std::system_error(error);
    }
    return result;
}

[[nodiscard]] inline std::string exchange(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    return read_to_end(socket);
}

template <typename predicate_type, typename representation_type, typename period_type>
inline void wait_until(predicate_type predicate,
    std::chrono::duration<representation_type, period_type> timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("test observation timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace ruvia::test
