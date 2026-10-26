#include "client/client_host_resolver.h"

#include <array>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_number_format.h"

namespace ruvia::detail {
struct client_host_resolver::query final {
    std::pmr::string host_;
    std::pmr::string service_;

    [[nodiscard]] client_resolve_result operator()() const {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        client_resolve_result result;
        result.addresses_ = resolver.resolve(host_, service_, result.error_);
        return result;
    }
};

namespace {
task<client_resolve_result> ready_result(client_resolve_result result) {
    co_return std::move(result);
}
}  // namespace

client_host_resolver::client_host_resolver(client_host_resolver&& other) noexcept
    : worker_(other.worker_) {
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

task<client_resolve_result> client_host_resolver::resolve(std::string_view host, std::uint16_t port,
    const operation_timeout& timeout) {
    std::error_code error;
    const auto address = asio::ip::make_address(host, error);
    std::array<char, http_max_unsigned_decimal_size> digits;
    const auto service = http_format_unsigned(digits, port);
    if (!error) {
        return ready_result(client_resolve_result{asio::ip::tcp::resolver::results_type::create(
                                                      asio::ip::tcp::endpoint(address, port), std::string(host), std::string(service)),
            {}});
    }
    return resolve_query(query{std::pmr::string(host, process_resource()), std::pmr::string(service, process_resource())}, timeout);
}

task<client_resolve_result> client_host_resolver::resolve_query(query input, operation_timeout timeout) {
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
    auto result = co_await run_blocking(*worker_,
        {.timeout_ = timeout.remaining(), .stop_token_ = stop_->token()}, std::move(input));
    switch (result.status()) {
        case blocking_status::completed:
            co_return std::move(result).value();
        case blocking_status::timed_out:
            co_return client_resolve_result{{}, asio::error::timed_out};
        case blocking_status::queue_full:
            co_return client_resolve_result{{}, asio::error::no_buffer_space};
        default:
            co_return client_resolve_result{{}, asio::error::operation_aborted};
    }
}

void client_host_resolver::cancel() noexcept {
    if (stop_) {
        stop_->request_stop();
    }
}

}  // namespace ruvia::detail
