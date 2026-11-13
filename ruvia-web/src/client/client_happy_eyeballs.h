#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"

namespace ruvia::detail {

struct client_resolve_result final {
    asio::ip::tcp::resolver::results_type addresses_{};
    // Numeric hosts do not need a heap-backed resolver result.
    std::optional<asio::ip::tcp::endpoint> literal_{};
    std::error_code error_{};
};

using client_resolve_operation = std::variant<client_resolve_result, task<client_resolve_result>>;

// False for an IP literal, which connects without a lookup.
[[nodiscard]] bool client_host_needs_lookup(std::string_view host);

// A DNS host is looked up on the blocking pool of the client's event loop;
// without one the client cannot connect, so it is rejected at construction.
void require_client_host_lookup(std::string_view host, const worker_ref& worker);

// HTTP、WebSocket、Redis 和数据库共用的系统名称查询。系统 getaddrinfo 不可
// 抢占；查询卸载到客户端事件循环借用的有界阻塞池，只携带自有输入，取消或停机先
// 释放等待者，再丢弃迟到结果。IP 字面量不查询，也不需要池。
class client_host_resolver final {
public:
    explicit client_host_resolver(const worker_ref& worker) noexcept
        : worker_(&worker) {}
    client_host_resolver(const client_host_resolver&) = delete;
    client_host_resolver& operator=(const client_host_resolver&) = delete;
    client_host_resolver(client_host_resolver&& other) noexcept;
    client_host_resolver& operator=(client_host_resolver&& other) noexcept;

    [[nodiscard]] client_resolve_operation resolve(std::string_view host, std::uint16_t port,
        const operation_deadline& timeout, stop_token stop = {});
    void cancel() noexcept;

private:
    struct query;
    [[nodiscard]] task<client_resolve_result> resolve_query(query input, operation_deadline timeout, stop_token stop);
    const worker_ref* worker_;
    std::optional<stop_source> stop_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
inline constexpr std::chrono::milliseconds client_connection_attempt_delay{250};

// Traverse RFC 6724 resolver order while alternating families (RFC 8305 §4).
// The resolver owns the addresses; traversal and duplicate suppression allocate
// nothing, allowing a bounded transport race to construct candidates lazily.
template <typename results_type>
class client_address_sequence final {
public:
    using endpoint_type = std::remove_cvref_t<decltype(std::declval<const results_type&>().begin()->endpoint())>;
    explicit client_address_sequence(const results_type& addresses)
        : addresses_(addresses),
          first_(addresses.begin()),
          other_(addresses.begin()),
          first_is_v6_(!addresses.empty() && addresses.begin()->endpoint().address().is_v6()),
          pending_(advance()) {}
    client_address_sequence(results_type&&) = delete;

    [[nodiscard]] bool has_next() const noexcept {
        return pending_.has_value();
    }

    [[nodiscard]] std::optional<endpoint_type> next() {
        auto result = std::move(pending_);
        if (result.has_value()) {
            pending_ = advance();
        }
        return result;
    }

private:
    [[nodiscard]] std::optional<endpoint_type> advance() {
        for (unsigned family = 0; family != 2; ++family) {
            auto& iterator = next_first_ ? first_ : other_;
            const bool is_v6 = next_first_ ? first_is_v6_ : !first_is_v6_;
            next_first_ = !next_first_;
            while (iterator != addresses_.end()) {
                const auto current = iterator++;
                const auto endpoint = current->endpoint();
                if (endpoint.address().is_v6() != is_v6) {
                    continue;
                }
                bool repeated = false;
                for (auto previous = addresses_.begin(); previous != current; ++previous) {
                    if (previous->endpoint() == endpoint) {
                        repeated = true;
                        break;
                    }
                }
                if (!repeated) {
                    return endpoint;
                }
            }
        }
        return std::nullopt;
    }

    const results_type& addresses_;
    decltype(std::declval<const results_type&>().begin()) first_;
    decltype(std::declval<const results_type&>().begin()) other_;
    bool first_is_v6_;
    bool next_first_{true};
    std::optional<endpoint_type> pending_;
};

struct client_happy_eyeballs_result final {
    std::optional<asio::ip::tcp::socket> socket_;
    std::error_code error_;
};

// RFC 8305 §4-5 connection-attempt phase. The platform resolver supplies the
// RFC 6724 sorted list; this operation interleaves address families, staggers
// attempts, keeps earlier attempts alive, and joins every loser after the first
// success or cancellation.
class client_happy_eyeballs final {
public:
    client_happy_eyeballs(const worker_ref& worker, const asio::ip::tcp::socket::executor_type& executor,
        const asio::ip::tcp::resolver::results_type& addresses);
    client_happy_eyeballs(worker_ref&&, const asio::ip::tcp::socket::executor_type&,
        const asio::ip::tcp::resolver::results_type&) = delete;
    ~client_happy_eyeballs();
    client_happy_eyeballs(const client_happy_eyeballs&) = delete;
    client_happy_eyeballs& operator=(const client_happy_eyeballs&) = delete;

    [[nodiscard]] task<client_happy_eyeballs_result> connect() &;
    task<client_happy_eyeballs_result> connect() && = delete;
    void cancel() noexcept;

private:
    struct attempt final {
        attempt(const asio::ip::tcp::socket::executor_type& executor, asio::ip::tcp::endpoint endpoint)
            : socket_(executor),
              endpoint_(std::move(endpoint)) {}
        attempt(const attempt&) = delete;
        attempt& operator=(const attempt&) = delete;
        attempt(attempt&&) noexcept = default;
        attempt& operator=(attempt&&) noexcept = default;

        asio::ip::tcp::socket socket_;
        asio::ip::tcp::endpoint endpoint_;
        bool active_{false};
    };

    void start_next() noexcept;
    void schedule_next() noexcept;
    void on_delay(worker_timer_outcome outcome) noexcept;
    void on_connect(std::size_t index, const std::error_code& error) noexcept;
    void close_active() noexcept;
    static void close_socket(asio::ip::tcp::socket& socket) noexcept;

    const worker_ref& worker_;
    worker_signal changed_;
    std::vector<attempt> attempts_;
    std::size_t next_{0};
    std::size_t active_{0};
    std::optional<std::size_t> winner_;
    std::exception_ptr failure_;
    std::error_code last_error_;
    bool launch_ready_{false};
    bool cancelled_{false};
    worker_timer_registration delay_;
};

}  // namespace ruvia::detail
