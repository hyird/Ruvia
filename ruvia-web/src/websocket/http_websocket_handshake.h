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
    std::pmr::memory_resource* resource = nullptr) {
    static constexpr std::size_t inline_head_capacity = 512;
    std::array<char, inline_head_capacity> inline_head;
    std::pmr::string overflow_head(resource != nullptr ? resource : std::pmr::get_default_resource());
    std::size_t total_size = 0;
    handshake.for_each_response_part([&total_size](std::string_view part) noexcept {
        total_size += part.size();
    });
    char* target = inline_head.data();
    if (total_size > inline_head.size()) {
        overflow_head.resize(total_size);
        target = overflow_head.data();
    }
    std::size_t offset = 0;
    handshake.for_each_response_part([target, &offset](std::string_view part) noexcept {
        if (!part.empty()) {
            std::char_traits<char>::copy(target + offset, part.data(), part.size());
            offset += part.size();
        }
    });
    co_return co_await write_http1_upgrade_response_head(
        stream, expectations, http_status::switching_protocols, std::string_view(target, total_size));
}

}  // namespace ruvia::detail
