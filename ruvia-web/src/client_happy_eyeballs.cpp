#include "client_happy_eyeballs.h"

#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>

#include "asio_bridge.h"
#include "task.h"
#include "timeout_duration.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia {

happy_eyeballs_dialer::happy_eyeballs_dialer(const worker_ref& worker, const asio::ip::tcp::socket::executor_type& executor, const asio::ip::tcp::resolver::results_type& addresses) :
    worker_(worker),
    changed_(worker),
    attempts_() {
    if (addresses.empty()) {
        return;
    }
    attempts_.reserve(addresses.size());
    address_family_interleaver sequence(addresses);
    while (auto endpoint = sequence.next()) {
        attempts_.emplace_back(executor, *endpoint);
    }
}

happy_eyeballs_dialer::~happy_eyeballs_dialer() {
    delay_.cancel();
    if (active_ != 0) {
        std::terminate();
    }
}

void happy_eyeballs_dialer::close_active() noexcept {
    for (auto& current : attempts_) {
        if (current.active_) {
            close_socket(current.socket_);
        }
    }
}

void happy_eyeballs_dialer::cancel() noexcept {
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

void happy_eyeballs_dialer::schedule_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    try {
        if (delay_.schedule(worker_, deadline_after(happy_eyeballs_attempt_delay), [this](worker_timer_status outcome) noexcept {
            on_delay(outcome);
        })) {
            return;
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
    cancel();
}

void happy_eyeballs_dialer::start_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    const auto index = next_++;
    auto& current = attempts_[index];
    current.active_ = true;
    ++active_;
    try {
        current.socket_.async_connect(current.endpoint_, [this, index](const std::error_code& error) noexcept {
            on_connect(index, error);
        });
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

void happy_eyeballs_dialer::on_delay(worker_timer_status outcome) noexcept {
    if (outcome == worker_timer_status::cancelled) {
        cancel();
        return;
    }
    launch_ready_ = true;
    changed_.notify();
}

void happy_eyeballs_dialer::on_connect(std::size_t index, const std::error_code& error) noexcept {
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
                delay_.cancel();
                launch_ready_ = true;
            }
        }
        close_socket(current.socket_);
    }
    changed_.notify();
}

task<happy_eyeballs_result> happy_eyeballs_dialer::connect() & {
    if (cancelled_) {
        co_return happy_eyeballs_result{{}, asio::error::operation_aborted};
    }
    if (attempts_.empty()) {
        co_return happy_eyeballs_result{{}, asio::error::host_not_found};
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
                co_return happy_eyeballs_result{{}, asio::error::operation_aborted};
            }
        } else if (winner_) {
            if (active_ == 0) {
                auto socket = std::move(attempts_[*winner_].socket_);
                co_return happy_eyeballs_result{std::move(socket), {}};
            }
        } else if (active_ == 0 && next_ == attempts_.size()) {
            co_return happy_eyeballs_result{{}, last_error_ ? last_error_ : std::error_code(asio::error::host_unreachable)};
        }
        co_await changed_.wait();
    }
}

}  // namespace ruvia
