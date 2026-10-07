#pragma once

#include "ruvia/web/HttpTunnel.h"

namespace ruvia::detail {
struct HttpTunnelAccess final {
    [[nodiscard]] static HttpTunnel make(std::pmr::memory_resource& resource,
        const WorkerHandle& worker, void* target, HttpTunnel::Read read,
        HttpTunnel::Write write, HttpTunnel::Finish finish, HttpTunnel::Abort abort) noexcept {
        return HttpTunnel(resource, worker, target, read, write, finish, abort);
    }
    static void bindDatagrams(HttpTunnel& tunnel, HttpTunnel::ReadDatagramInput read,
        HttpTunnel::SendDatagram send, HttpTunnel::DatagramConfig config) noexcept {
        tunnel.readDatagramInput_ = read;
        tunnel.sendDatagram_ = send;
        tunnel.datagramConfig_ = config;
    }
    [[nodiscard]] static bool hasRunningOperations(const HttpTunnel& tunnel) noexcept {
        return tunnel.runningOperations_ != 0;
    }
    // The transport must be stopped before joining if an operation remains
    // suspended. Cold operations expire while their borrowed owner is alive.
    [[nodiscard]] static Task<void> closeAndJoin(HttpTunnel& tunnel) {
        return tunnel.operations_.close_and_join();
    }
};
}  // namespace ruvia::detail
