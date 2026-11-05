#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/task.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/websocket_handshake.h"

#include "server/http1_upgrade_response_head.h"

namespace ruvia::detail {

// Flush the exact HTTP-owned 101 handshake prepared by the route. Negotiation is
// not recomputed and no compression bool is returned through a side channel.
// The shared HTTP/1 upgrade-head writer applies the request `expectations`
// (100 Continue before 101, RFC 9110 section 7.8).
template <typename stream_type>
task<std::error_code> write_websocket_handshake(stream_type& stream,
    const websocket_server_handshake& handshake, const http_request_expectations& expectations,
    std::pmr::memory_resource& resource) {
    static constexpr std::size_t inline_head_capacity = 512;
    std::array<char, inline_head_capacity> inline_head;
    std::pmr::string overflow_head(&resource);
    std::size_t inline_size = 0;
    bool using_overflow = false;
    handshake.for_each_response_part([&](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!using_overflow) {
            if (part.size() <= inline_head.size() - inline_size) {
                std::char_traits<char>::copy(inline_head.data() + inline_size, part.data(), part.size());
                inline_size += part.size();
                return;
            }
            overflow_head.reserve((std::max)(inline_size + part.size(), inline_head.size() * 2));
            overflow_head.append(inline_head.data(), inline_size);
            using_overflow = true;
        }
        overflow_head.append(part);
    });
    const auto head_view = using_overflow ? std::string_view(overflow_head)
                                          : std::string_view(inline_head.data(), inline_size);
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, head_view);
}

}  // namespace ruvia::detail
