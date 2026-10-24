#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

// The single writer of every HTTP/1 final head that hands the connection to
// another protocol: a WebSocket or CONNECT-UDP 101 and a CONNECT 2xx. RFC 9110
// section 7.8: when the head is 101 (Switching Protocols) and the request
// carried Expect: 100-continue, the HTTP-owned
// http_request_expectations::continue_before_switching_protocols() decision
// puts a 100 (Continue) head before it in the same write, even when no content
// follows. Any other status is written alone. `head` is the complete encoded
// final head; it must stay valid until the returned task completes.
template <typename stream_type>
[[nodiscard]] task<std::error_code> write_http1_upgrade_response_head(stream_type& stream,
    const http_request_expectations& expectations, http_status_code status, std::string_view head) {
    constexpr std::size_t continue_head_capacity = 64;
    std::array<char, continue_head_capacity> continue_bytes;
    std::string_view continue_head;
    if (status == http_status::switching_protocols && expectations.continue_before_switching_protocols()) {
        const auto prepared = prepare_http1_interim_response_head(
            http_interim_response_head(http_status::continue_value), std::span<char>(continue_bytes));
        const auto* prepared_head = std::get_if<prepared_http1_interim_response>(&prepared);
        if (prepared_head == nullptr) {
            throw std::logic_error("cannot prepare HTTP/1 100 Continue head");
        }
        continue_head = prepared_head->head();
    }
    const std::array<asio::const_buffer, 2> buffers{
        asio::buffer(continue_head.data(), continue_head.size()), asio::buffer(head.data(), head.size())};
    const auto written = co_await ruvia::async_asio<std::size_t>([&stream, &buffers](auto handler) {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return written.error_code();
}

}  // namespace ruvia::detail
