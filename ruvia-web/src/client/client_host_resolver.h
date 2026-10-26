#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia::detail {

struct client_resolve_result final {
    asio::ip::tcp::resolver::results_type addresses_;
    std::error_code error_;
};

// HTTP、WebSocket、Redis 和数据库共用的系统名称查询。系统 getaddrinfo 不可
// 抢占；core 的有界卸载只携带自有输入，取消或停机先释放等待者，再丢弃迟到结果。
class client_host_resolver final {
public:
    explicit client_host_resolver(const worker_handle& worker) noexcept
        : worker_(&worker) {}
    client_host_resolver(const client_host_resolver&) = delete;
    client_host_resolver& operator=(const client_host_resolver&) = delete;
    client_host_resolver(client_host_resolver&& other) noexcept;
    client_host_resolver& operator=(client_host_resolver&& other) noexcept;

    [[nodiscard]] task<client_resolve_result> resolve(std::string_view host, std::uint16_t port,
        const operation_timeout& timeout);
    void cancel() noexcept;

private:
    struct query;
    [[nodiscard]] task<client_resolve_result> resolve_query(query input, operation_timeout timeout);
    const worker_handle* worker_;
    std::optional<stop_source> stop_;
};

}  // namespace ruvia::detail
