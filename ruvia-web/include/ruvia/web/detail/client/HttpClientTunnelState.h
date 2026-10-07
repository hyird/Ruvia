#pragma once
#include <deque>
#include <string>

#include "ruvia/http/HttpDatagram.h"
#include "ruvia/web/HttpClientTunnelConfig.h"
#include "ruvia/web/detail/client/HttpClientOutputQueue.h"

namespace ruvia::detail {
class HttpClientTunnelState final {
public:
    HttpClientTunnelState(const WorkerHandle& worker, std::pmr::memory_resource* resource, HttpClientTunnelConfig config)
        : output(worker, resource),
          config(config),
          datagrams(resource) {}
    http_client_output_queue output;
    HttpClientTunnelConfig config;
    std::pmr::deque<std::pmr::string> datagrams;
    bool udp{};
    bool accepted{};
    bool receiveEnded{};
};
}  // namespace ruvia::detail
