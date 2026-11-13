#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/read.hpp>

#include "ruvia/http/http2_framing.h"

namespace ruvia::test {

inline void append_frame(std::string& wire, http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, http2_frame_header_bytes> header{};
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        !encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

struct received_frame final {
    http2_frame_header header_;
    std::string payload_{};
};

template <typename stream_type>
[[nodiscard]] inline received_frame read_frame(stream_type& stream,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    auto& socket = stream.lowest_layer();
    const auto was_non_blocking = socket.non_blocking();
    struct restore_mode final {
        decltype(socket) socket_;
        bool mode_;
        ~restore_mode() {
            asio::error_code ignored;
            socket_.non_blocking(mode_, ignored);
        }
    } restore{socket, was_non_blocking};
    if (deadline) {
        socket.non_blocking(true);
    }
    const auto read_exact = [&](char* bytes, std::size_t count) {
        if (!deadline) {
            asio::read(stream, asio::buffer(bytes, count));
            return;
        }
        while (count != 0) {
            if (std::chrono::steady_clock::now() >= *deadline) {
                throw std::runtime_error("HTTP/2 test frame timed out");
            }
            asio::error_code error;
            const auto received = stream.read_some(asio::buffer(bytes, count), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (error) {
                throw std::system_error(error);
            }
            bytes += received;
            count -= received;
        }
    };
    std::array<char, http2_frame_header_bytes> bytes{};
    read_exact(bytes.data(), bytes.size());
    const auto header = parse_http2_frame_header(bytes);
    if (!header) {
        throw std::runtime_error("invalid HTTP/2 response frame");
    }
    received_frame result{*header, std::string(header->length_, '\0')};
    read_exact(result.payload_.data(), result.payload_.size());
    return result;
}

}  // namespace ruvia::test
