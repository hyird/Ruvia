#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ruvia/web/server_config.h"

namespace ruvia::detail {

// Checked startup dimensions shared by application, standalone workers and Acceptor.
struct http3_capacity final {
    static constexpr std::size_t packet_bytes = 65536;
    std::uint32_t stream_slots_;
    std::size_t input_slots_;
    std::size_t output_credits_;
    std::size_t packet_slots_;
};

[[nodiscard]] inline http3_capacity normalize_http3_capacity(
    const http3_listen_config& config, std::size_t worker_count) {
    if (worker_count == 0 || worker_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HTTP/3 worker count is not representable");
    }
    // UINT32_MAX is the stream buffer free-list sentinel.
    if (config.stream_buffer_capacity_ == 0 ||
        config.stream_buffer_capacity_ >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HTTP/3 stream buffer capacity must be positive and below the 32-bit node limit");
    }
    const auto input = config.datagram_input_capacity_;
    const auto output = config.datagram_output_capacity_;
    constexpr auto max_slots = static_cast<std::size_t>(
                                   std::numeric_limits<std::ptrdiff_t>::max()) /
                               http3_capacity::packet_bytes;
    if (input == 0 || output == 0 || input >= max_slots || output > max_slots - input) {
        throw std::invalid_argument("HTTP/3 datagram capacities must be positive and representable");
    }
    const auto per_worker = input + output;
    // One receive lease plus every worker's input leases and output credits.
    if (worker_count > (max_slots - 1) / per_worker) {
        throw std::invalid_argument("HTTP/3 aggregate datagram storage is not representable");
    }
    return {static_cast<std::uint32_t>(config.stream_buffer_capacity_), input, output,
        1 + worker_count * per_worker};
}

}  // namespace ruvia::detail
