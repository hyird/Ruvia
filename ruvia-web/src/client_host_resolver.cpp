#include "client_host_resolver.h"

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "blocking_pool.h"
#include "decimal_text.h"
#include "operation_options.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia {

struct dns_resolver::query final {
    std::string host_;
    std::uint16_t port_;

    [[nodiscard]] dns_resolve_result operator()() const {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        std::array<char, max_decimal_size> digits;
        const auto service = format_decimal(digits, port_);
        dns_resolve_result result;
        result.addresses_ = resolver.resolve(host_, service, result.error_);
        return result;
    }
};

dns_resolver::dns_resolver(dns_resolver&& other) noexcept :
    worker_(other.worker_) {
    if (other.stop_) {
        std::terminate();
    }
}

dns_resolver& dns_resolver::operator=(dns_resolver&& other) noexcept {
    if (stop_ || other.stop_) {
        std::terminate();
    }
    worker_ = other.worker_;
    return *this;
}

task<dns_resolve_result> dns_resolver::resolve_query(std::string host, std::uint16_t port, operation_deadline timeout, stop_token stop) {
    if (stop_) {
        throw std::logic_error("host resolver already has a pending query");
    }
    stop_.emplace();
    struct reset_stop final {
        std::optional<stop_source>& stop_;
        ~reset_stop() {
            stop_.reset();
        }
    } guard{stop_};
    const linked_operation_options linked({.timeout_ = timeout.timeout(), .stop_token_ = std::move(stop)}, stop_->token());
    auto result = co_await run_blocking(*worker_, query{std::move(host), port}, linked.options());
    switch (result.status()) {
        case blocking_status::completed:
        case blocking_status::failed:
            co_return std::move(result).take_value();
        case blocking_status::timed_out:
            co_return dns_resolve_result{.error_ = asio::error::timed_out};
        case blocking_status::queue_full:
            co_return dns_resolve_result{.error_ = asio::error::no_buffer_space};
        default:
            co_return dns_resolve_result{.error_ = asio::error::operation_aborted};
    }
}

void dns_resolver::cancel() noexcept {
    if (stop_) {
        stop_->request_stop();
    }
}

namespace detail {

bool client_host_needs_lookup(std::string_view host) {
    return !try_parse_asio_ip_address(host).has_value();
}

void require_client_host_lookup(std::string_view host, const worker_ref& worker) {
    if (worker.blocking_pool() == nullptr && client_host_needs_lookup(host)) {
        throw std::invalid_argument("a DNS host requires an event loop with a blocking pool");
    }
}

}  // namespace detail
}  // namespace ruvia
