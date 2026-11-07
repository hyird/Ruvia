#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"

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
          first_is_v6_(!addresses.empty() && addresses.begin()->endpoint().address().is_v6()) {}

    [[nodiscard]] std::optional<endpoint_type> next() {
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

private:
    const results_type& addresses_;
    decltype(std::declval<const results_type&>().begin()) first_;
    decltype(std::declval<const results_type&>().begin()) other_;
    bool first_is_v6_;
    bool next_first_{true};
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
    client_happy_eyeballs(const worker_handle& worker, const asio::ip::tcp::socket::executor_type& executor,
        const asio::ip::tcp::resolver::results_type& addresses,
        std::pmr::memory_resource* resource = nullptr);
    client_happy_eyeballs(worker_handle&&, const asio::ip::tcp::socket::executor_type&,
        const asio::ip::tcp::resolver::results_type&, std::pmr::memory_resource* = nullptr) = delete;
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

    const worker_handle& worker_;
    worker_signal changed_;
    worker_timer_registration delay_;
    std::pmr::vector<attempt> attempts_;
    std::size_t next_{0};
    std::size_t active_{0};
    std::optional<std::size_t> winner_;
    std::exception_ptr failure_;
    std::error_code last_error_;
    bool launch_ready_{false};
    bool cancelled_{false};
};

}  // namespace ruvia::detail
