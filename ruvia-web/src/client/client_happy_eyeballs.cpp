#include "client/client_happy_eyeballs.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/socket.h"
#include "ruvia/http/http_number_format.h"

namespace ruvia::detail {

client_happy_eyeballs::client_happy_eyeballs(const worker_ref& worker,
    const asio::ip::tcp::socket::executor_type& executor,
    const asio::ip::tcp::resolver::results_type& addresses)
    : worker_(worker),
      changed_(worker),
      attempts_() {
    if (addresses.empty()) {
        return;
    }
    attempts_.reserve(addresses.size());
    client_address_sequence sequence(addresses);
    while (auto endpoint = sequence.next()) {
        attempts_.emplace_back(executor, *endpoint);
    }
}

client_happy_eyeballs::~client_happy_eyeballs() {
    delay_.cancel();
    if (active_ != 0) {
        std::terminate();
    }
}

void client_happy_eyeballs::close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    (void)socket.cancel(ignored);
    (void)socket.close(ignored);
}

void client_happy_eyeballs::close_active() noexcept {
    for (auto& current : attempts_) {
        if (current.active_) {
            close_socket(current.socket_);
        }
    }
}

void client_happy_eyeballs::cancel() noexcept {
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    if (winner_) {
        close_socket(attempts_[*winner_].socket_);
    }
    winner_.reset();
    delay_.cancel();
    close_active();
    changed_.notify();
}

void client_happy_eyeballs::schedule_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    try {
        const auto status = worker_.schedule_timer(delay_, deadline_after(client_connection_attempt_delay),
            [this](worker_timer_outcome outcome) noexcept { on_delay(outcome); });
        if (status != worker_timer_schedule_status::worker_stopping) {
            return;
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
    cancel();
}

void client_happy_eyeballs::start_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    const auto index = next_++;
    auto& current = attempts_[index];
    current.active_ = true;
    ++active_;
    try {
        current.socket_.async_connect(current.endpoint_,
            [this, index](const std::error_code& error) noexcept { on_connect(index, error); });
    } catch (...) {
        current.active_ = false;
        --active_;
        close_socket(current.socket_);
        failure_ = std::current_exception();
        cancel();
        return;
    }
    schedule_next();
}

void client_happy_eyeballs::on_delay(worker_timer_outcome outcome) noexcept {
    if (outcome == worker_timer_outcome::cancelled) {
        cancel();
        return;
    }
    launch_ready_ = true;
    changed_.notify();
}

void client_happy_eyeballs::on_connect(std::size_t index, const std::error_code& error) noexcept {
    auto& current = attempts_[index];
    if (!current.active_ || active_ == 0) {
        std::terminate();
    }
    current.active_ = false;
    --active_;
    if (!error && !cancelled_ && !winner_) {
        winner_ = index;
        delay_.cancel();
        close_active();
    } else {
        if (error && error != asio::error::operation_aborted) {
            last_error_ = error;
            if (!cancelled_ && !winner_) {
                // RFC 8305 section 5: a fast failure must not leave the next
                // address waiting for the remainder of the attempt delay.
                delay_.cancel();
                launch_ready_ = true;
            }
        }
        close_socket(current.socket_);
    }
    changed_.notify();
}

task<client_happy_eyeballs_result> client_happy_eyeballs::connect() & {
    if (cancelled_) {
        co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
    }
    if (attempts_.empty()) {
        co_return client_happy_eyeballs_result{{}, asio::error::host_not_found};
    }
    start_next();
    for (;;) {
        if (launch_ready_) {
            launch_ready_ = false;
            start_next();
        }
        if (cancelled_) {
            close_active();
            if (active_ == 0) {
                if (failure_) {
                    std::rethrow_exception(failure_);
                }
                co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
            }
        } else if (winner_) {
            if (active_ == 0) {
                auto socket = std::move(attempts_[*winner_].socket_);
                co_return client_happy_eyeballs_result{std::move(socket), {}};
            }
        } else if (active_ == 0 && next_ == attempts_.size()) {
            co_return client_happy_eyeballs_result{{},
                last_error_ ? last_error_ : std::error_code(asio::error::host_unreachable)};
        }
        co_await changed_.wait();
    }
}

}  // namespace ruvia::detail

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

client_resolve_operation client_host_resolver::resolve(std::string_view host, std::uint16_t port,
    const operation_deadline& timeout, stop_token stop) {
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
    auto result = co_await run_blocking(
        *worker_, {.timeout_ = timeout.timeout(), .stop_token_ = cancellation.token()}, std::move(input));
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
