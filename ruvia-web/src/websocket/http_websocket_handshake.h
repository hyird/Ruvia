#pragma once

#include <array>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// `continue_first` is the request's HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision:
// the 100 (Continue) head then precedes the 101 in the same write.
template <typename stream_type>
task<std::error_code> write_websocket_handshake(
    stream_type& stream, const websocket_server_handshake& handshake, bool continue_first) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity + max_http_header_bytes> head_bytes;
    std::size_t size = 0;
    if (continue_first) {
        const auto prepared = prepare_http1_interim_response_head(
            http_interim_response_head(http_status::continue_value),
            std::span<char>(head_bytes.data(), continue_head_capacity));
        const auto* continue_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (continue_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        size = continue_head->head().size();
    }
    handshake.for_each_response_part([&head_bytes, &size](std::string_view part) {
        std::char_traits<char>::copy(head_bytes.data() + size, part.data(), part.size());
        size += part.size();
    });
    const auto active_buffer = asio::buffer(head_bytes.data(), size);

    const auto write_completion = co_await ruvia::async_asio([&stream, active_buffer](auto handler) mutable {
        asio::async_write(stream, active_buffer, std::move(handler));
    });
    co_return write_completion.error_code();
}

}  // namespace ruvia::detail
