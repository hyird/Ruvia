#include "client/client_happy_eyeballs.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <ranges>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {
namespace {

using namespace std::chrono_literals;

constexpr auto connection_attempt_delay = 250ms;

void append_unique(std::pmr::vector<asio::ip::tcp::endpoint>& output,
    const asio::ip::tcp::endpoint& endpoint) {
    if (std::ranges::find(output, endpoint) == output.end()) {
        output.push_back(endpoint);
    }
}

std::pmr::vector<asio::ip::tcp::endpoint> interleave_addresses(
    const asio::ip::tcp::resolver::results_type& addresses) {
    auto* const resource = process_resource();
    std::pmr::vector<asio::ip::tcp::endpoint> first_family(resource);
    std::pmr::vector<asio::ip::tcp::endpoint> other_family(resource);
    if (addresses.empty()) {
        return first_family;
    }
    const bool first_is_v6 = addresses.begin()->endpoint().address().is_v6();
    for (const auto& entry : addresses) {
        auto& family = entry.endpoint().address().is_v6() == first_is_v6 ? first_family : other_family;
        append_unique(family, entry.endpoint());
    }
    std::pmr::vector<asio::ip::tcp::endpoint> ordered(resource);
    ordered.reserve(first_family.size() + other_family.size());
    const auto count = std::max(first_family.size(), other_family.size());
    for (std::size_t index = 0; index != count; ++index) {
        if (index < first_family.size()) {
            ordered.push_back(first_family[index]);
        }
        if (index < other_family.size()) {
            ordered.push_back(other_family[index]);
        }
    }
    return ordered;
}

}  // namespace

client_happy_eyeballs::client_happy_eyeballs(const worker_handle& worker,
    const asio::ip::tcp::socket::executor_type& executor,
    const asio::ip::tcp::resolver::results_type& addresses)
    : worker_(worker),
      changed_(worker),
      attempts_(process_resource()) {
    const auto ordered = interleave_addresses(addresses);
    attempts_.reserve(ordered.size());
    for (const auto& endpoint : ordered) {
        attempts_.emplace_back(executor, endpoint);
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
        const auto status = worker_.schedule_timer(delay_, worker_timer_deadline_after(connection_attempt_delay),
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
