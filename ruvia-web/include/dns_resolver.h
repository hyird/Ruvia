#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>

#include "asio_bridge.h"
#include "http_authority.h"
#include "operation_options.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_signal.h"
#include "worker_timer.h"

namespace ruvia {

namespace detail {

[[nodiscard]] constexpr bool is_ascii_dns_alnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] constexpr bool is_ascii_dns_digit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

}  // namespace detail

// Validates an ASCII transport hostname with an optional trailing root dot.
// Rejects four-numeric-label forms lexically (IPv4 literals are not DNS names).
[[nodiscard]] constexpr bool is_valid_dns_host(std::string_view host) noexcept {
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
        if (!detail::is_ascii_dns_alnum(first) || !detail::is_ascii_dns_alnum(last)) {
            return false;
        }

        bool label_numeric = true;
        for (std::size_t i = label_start; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!detail::is_ascii_dns_alnum(byte) && byte != '-') {
                return false;
            }
            label_numeric = label_numeric && detail::is_ascii_dns_digit(byte);
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

struct dns_resolve_result final {
    asio::ip::tcp::resolver::results_type addresses_{};
    // Numeric IP literals resolve inline without allocating a resolver list.
    std::optional<asio::ip::tcp::endpoint> literal_{};
    std::error_code error_{};
};

using dns_resolve_operation = std::variant<dns_resolve_result, task<dns_resolve_result>>;

// Offloads blocking system `getaddrinfo` queries onto the worker's bounded
// blocking pool while resolving numeric IP literals synchronously without pool
// submission. Cancellation or shutdown wakes the caller immediately and drops
// the late result safely.
class dns_resolver final {
public:
    explicit dns_resolver(const worker_ref& worker) noexcept :
        worker_(&worker) {}
    dns_resolver(worker_ref&&) = delete;
    dns_resolver(const dns_resolver&) = delete;
    dns_resolver& operator=(const dns_resolver&) = delete;
    dns_resolver(dns_resolver&& other) noexcept;
    dns_resolver& operator=(dns_resolver&& other) noexcept;

    template <typename host_type>
        requires std::convertible_to<host_type&&, std::string_view>
    [[nodiscard]] dns_resolve_operation resolve(host_type&& host, std::uint16_t port, const operation_deadline& timeout, stop_token stop = {}) {
        if (stop.stop_requested()) {
            return dns_resolve_result{.error_ = asio::error::operation_aborted};
        }
        if (const auto address = try_parse_asio_ip_address(std::string_view(host))) {
            return dns_resolve_result{.literal_ = asio::ip::tcp::endpoint(*address, port)};
        }
        return resolve_query(std::string(std::forward<host_type>(host)), port, timeout, std::move(stop));
    }

    void cancel() noexcept;

private:
    struct query;
    [[nodiscard]] task<dns_resolve_result> resolve_query(std::string host, std::uint16_t port, operation_deadline timeout, stop_token stop);

    const worker_ref* worker_;
    std::optional<stop_source> stop_;
};

inline constexpr std::chrono::milliseconds happy_eyeballs_attempt_delay{250};

// Traverses RFC 6724 resolver order while alternating address families (RFC 8305 §4)
// and suppressing duplicates without heap allocation.
template <typename results_type>
class address_family_interleaver final {
public:
    using endpoint_type = std::remove_cvref_t<decltype(std::declval<const results_type&>().begin()->endpoint())>;

    explicit address_family_interleaver(const results_type& addresses) :
        addresses_(addresses),
        first_(addresses.begin()),
        other_(addresses.begin()),
        first_is_v6_(!addresses.empty() && addresses.begin()->endpoint().address().is_v6()),
        pending_(advance()) {}
    address_family_interleaver(results_type&&) = delete;

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

struct happy_eyeballs_result final {
    std::optional<asio::ip::tcp::socket> socket_;
    std::error_code error_;
};

// RFC 8305 §4-5 Happy Eyeballs v2 connection-attempt racer: interleaves IPv6/IPv4
// families, staggers attempts by 250 ms (advancing immediately on fast failure),
// and closes/joins every loser once the first socket connects or on cancellation.
class happy_eyeballs_dialer final {
public:
    happy_eyeballs_dialer(const worker_ref& worker, const asio::ip::tcp::socket::executor_type& executor, const asio::ip::tcp::resolver::results_type& addresses);
    happy_eyeballs_dialer(worker_ref&&, const asio::ip::tcp::socket::executor_type&, const asio::ip::tcp::resolver::results_type&) = delete;
    ~happy_eyeballs_dialer();
    happy_eyeballs_dialer(const happy_eyeballs_dialer&) = delete;
    happy_eyeballs_dialer& operator=(const happy_eyeballs_dialer&) = delete;

    [[nodiscard]] task<happy_eyeballs_result> connect() &;
    task<happy_eyeballs_result> connect() && = delete;
    void cancel() noexcept;

private:
    struct attempt final {
        attempt(const asio::ip::tcp::socket::executor_type& executor, asio::ip::tcp::endpoint endpoint) :
            socket_(executor),
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
    void on_delay(worker_timer_status outcome) noexcept;
    void on_connect(std::size_t index, const std::error_code& error) noexcept;
    void close_active() noexcept;

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

}  // namespace ruvia
