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

#include "asio_ip_address.h"
#include "blocking_pool.h"
#include "decimal_text.h"
#include "operation_options.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia::detail {

struct client_host_resolver::query final {
    std::string host_;
    std::uint16_t port_;

    [[nodiscard]] client_resolve_result operator()() const {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        std::array<char, http_max_unsigned_decimal_size> digits;
        const auto service = http_format_unsigned(digits, port_);
        client_resolve_result result;
        result.addresses_ = resolver.resolve(host_, service, result.error_);
        return result;
    }
};

bool client_host_needs_lookup(std::string_view host) {
    return !parse_ip_address(host).has_value();
}

void require_client_host_lookup(std::string_view host, const worker_ref& worker) {
    if (worker.blocking_pool() == nullptr && client_host_needs_lookup(host)) {
        throw std::invalid_argument("a DNS host requires an event loop with a blocking pool");
    }
}

client_host_resolver::client_host_resolver(client_host_resolver&& other) noexcept :
    worker_(other.worker_) {
    if (other.stop_) {
        std::terminate();
    }
}

client_host_resolver& client_host_resolver::operator=(client_host_resolver&& other) noexcept {
    if (stop_ || other.stop_) {
        std::terminate();
    }
    worker_ = other.worker_;
    return *this;
}

client_resolve_operation client_host_resolver::resolve(std::string_view host, std::uint16_t port, const operation_deadline& timeout, stop_token stop) {
    if (stop.stop_requested()) {
        return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
    if (const auto address = parse_ip_address(host)) {
        return client_resolve_result{.literal_ = asio::ip::tcp::endpoint(*address, port)};
    }
    return resolve_query(query{std::string(host), port}, timeout, std::move(stop));
}

task<client_resolve_result> client_host_resolver::resolve_query(query input, operation_deadline timeout, stop_token stop) {
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
    stop_group cancellation(stop_->token(), stop);
    auto result = co_await run_blocking(*worker_, {.timeout_ = timeout.timeout(), .stop_token_ = cancellation.token()}, std::move(input));
    switch (result.status()) {
        case blocking_status::completed:
            co_return std::move(result).take_value();
        case blocking_status::timed_out:
            co_return client_resolve_result{.error_ = asio::error::timed_out};
        case blocking_status::queue_full:
            co_return client_resolve_result{.error_ = asio::error::no_buffer_space};
        default:
            co_return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
}

void client_host_resolver::cancel() noexcept {
    if (stop_) {
        stop_->request_stop();
    }
}

}  // namespace ruvia::detail
