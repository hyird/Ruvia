#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio/ip/tcp.hpp>

#include "http_authority.h"
#include "operation_options.h"
#include "startup_config.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia::detail {

// Lexical checks on a transport host: IP literal or ASCII DNS name.
[[nodiscard]] inline bool is_ascii_dns_alnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool is_ascii_dns_digit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// An unbracketed IP literal peer, verified against an iPAddress SAN and never
// sent as SNI (RFC 6066 §3); any other host is a DNS name.
[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

// Validates an ASCII transport hostname with an optional root dot. This is
// intentionally narrower than a DNS owner name or URI reg-name; the
// four-numeric-label exclusion is lexical and does not parse IPv4 addresses.
[[nodiscard]] inline bool is_valid_dns_host(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    if (host.empty() || host.ends_with('.') || host.size() > 253) {
        return false;
    }

    std::size_t labels = 0;
    bool saw_dot = false;
    bool all_labels_numeric = true;
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const auto label_end = host.find('.', label_start);
        const auto end = label_end == std::string_view::npos ? host.size() : label_end;
        const auto label_length = end - label_start;
        if (label_length == 0 || label_length > 63) {
            return false;
        }
        const auto first = static_cast<unsigned char>(host[label_start]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!is_ascii_dns_alnum(first) || !is_ascii_dns_alnum(last)) {
            return false;
        }

        bool label_numeric = true;
        for (std::size_t i = label_start; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!is_ascii_dns_alnum(byte) && byte != '-') {
                return false;
            }
            label_numeric = label_numeric && is_ascii_dns_digit(byte);
        }
        all_labels_numeric = all_labels_numeric && label_numeric;
        ++labels;
        if (label_end == std::string_view::npos) {
            break;
        }
        saw_dot = true;
        label_start = end + 1;
    }
    return !(saw_dot && labels == 4 && all_labels_numeric);
}

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
    explicit client_host_resolver(const worker_ref& worker) noexcept :
        worker_(&worker) {}
    client_host_resolver(const client_host_resolver&) = delete;
    client_host_resolver& operator=(const client_host_resolver&) = delete;
    client_host_resolver(client_host_resolver&& other) noexcept;
    client_host_resolver& operator=(client_host_resolver&& other) noexcept;

    [[nodiscard]] client_resolve_operation resolve(std::string_view host, std::uint16_t port, const operation_deadline& timeout, stop_token stop = {});
    void cancel() noexcept;

private:
    struct query;
    [[nodiscard]] task<client_resolve_result> resolve_query(query input, operation_deadline timeout, stop_token stop);
    const worker_ref* worker_;
    std::optional<stop_source> stop_;
};

// A client transport host, also used for TLS SNI and peer verification: an
// unbracketed IP literal or an ASCII DNS host name.
inline void validate_client_origin_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    ensure_config_host(host, empty_message, invalid_message);
    if (!is_client_ip_address(host) && !is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

}  // namespace ruvia::detail
