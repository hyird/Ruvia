#pragma once

#include "ruvia/web/http_tunnel.h"

namespace ruvia::detail {
struct http_tunnel_access final {
    [[nodiscard]] static http_tunnel make(std::pmr::memory_resource& resource,
        const worker_handle& worker_value, void* target, http_tunnel::read_type read,
        http_tunnel::write_type write, http_tunnel::finish_type finish_value, http_tunnel::abort_type abort) noexcept {
        return http_tunnel(resource, worker_value, target, read, write, finish_value, abort);
    }
    static void bind_datagrams(http_tunnel& tunnel, http_tunnel::read_datagram_input_type read,
        http_tunnel::send_datagram_type send, http_tunnel::datagram_config_type config) noexcept {
        tunnel.read_datagram_input_ = read;
        tunnel.send_datagram_ = send;
        tunnel.datagram_config_ = config;
    }
    [[nodiscard]] static bool has_running_operations(const http_tunnel& tunnel) noexcept {
        return tunnel.running_operations_ != 0;
    }
    // The transport must be stopped before joining if an operation remains
    // suspended. Cold operations expire while their borrowed owner is alive.
    [[nodiscard]] static task<void> close_and_join(http_tunnel& tunnel) {
        return tunnel.operations_.close_and_join();
    }
};
}  // namespace ruvia::detail
