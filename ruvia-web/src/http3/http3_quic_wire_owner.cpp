#include "http3/http3_quic_wire_owner.h"

#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/post.hpp>

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {

http3_quic_wire_owner::timer_wait_handler_type::timer_wait_handler_type(
    http3_quic_wire_owner& owner_value, std::uint64_t generation) noexcept
    : owner_(&owner_value),
      generation_(generation),
      allocator_(owner_value.timer_handler_allocator_) {}

http3_quic_wire_owner::timer_wait_handler_type::timer_wait_handler_type(
    timer_wait_handler_type&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      generation_(other.generation_),
      allocator_(other.allocator_) {}

http3_quic_wire_owner::timer_wait_handler_type::~timer_wait_handler_type() {
    if (owner_ != nullptr) {
        owner_->on_timer_handler_retired(generation_);
    }
}

void http3_quic_wire_owner::timer_wait_handler_type::operator()(
    const asio::error_code& error) noexcept {
    if (owner_ != nullptr) {
        try {
            owner_->on_timer_completion(generation_, error);
        } catch (...) {
            owner_->record_current_failure();
        }
    }
}

http3_quic_wire_owner::timer_retirement_handler_type::timer_retirement_handler_type(
    http3_quic_wire_owner& owner_value) noexcept
    : owner_(&owner_value),
      allocator_(owner_value.timer_handler_allocator_) {}

http3_quic_wire_owner::timer_retirement_handler_type::timer_retirement_handler_type(
    timer_retirement_handler_type&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      allocator_(other.allocator_) {}

http3_quic_wire_owner::timer_retirement_handler_type::~timer_retirement_handler_type() {
    if (owner_ != nullptr) {
        owner_->on_timer_retirement_handler_retired();
    }
}

void http3_quic_wire_owner::timer_retirement_handler_type::operator()() noexcept {
    if (owner_ != nullptr) {
        try {
            owner_->on_timer_retirement_wake();
        } catch (...) {
            owner_->record_current_failure();
        }
    }
}

http3_quic_wire_owner::http3_quic_wire_owner(asio::io_context& worker_io,
    http3_datagram_channel& channel, http3_worker_datagram_endpoint::udp::endpoint local_endpoint,
    http3_quic_tls_context& tls, ruvia::quic_server_config transport_config,
    std::pmr::memory_resource* timer_handler_resource, protocol_pump_type protocol_pump)
    : owner_thread_(std::this_thread::get_id()),
      network_io_(worker_io),
      tls_(tls),
      transport_config_(transport_config),
      endpoint_(channel, std::move(local_endpoint),
          http3_worker_datagram_endpoint::notification{this, &endpoint_notification}),
      timer_(network_io_),
      timer_handler_allocator_(timer_handler_resource != nullptr
                                   ? timer_handler_resource
                                   : ruvia::detail::process_resource()),
      protocol_pump_(protocol_pump) {
    if (protocol_pump_.context_ == nullptr || protocol_pump_.drive_ == nullptr) {
        throw std::invalid_argument("HTTP/3 wire protocol pump must be complete");
    }
}

http3_quic_wire_owner::~http3_quic_wire_owner() {
    require_owner_thread();
    if (!stopping_) {
        request_stop();
    }
    if (!stop_status().complete()) {
        std::terminate();
    }
}

void http3_quic_wire_owner::prepare() {
    require_owner_thread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 server network QUIC wire owner cannot prepare in this state");
    }
    try {
        endpoint_.prepare();
        transport_.emplace(tls_, transport_config_, timer_handler_allocator_.resource());
        transport_destroyed_ = false;
        prepared_ = true;
    } catch (...) {
        record_current_failure();
        throw;
    }
}

void http3_quic_wire_owner::start() {
    require_owner_thread();
    if (!prepared_ || started_ || stopping_) {
        throw std::logic_error("HTTP/3 server network QUIC wire owner cannot start in this state");
    }
    try {
        started_ = true;
        const auto receive = endpoint_.start();
        if (receive == http3_worker_datagram_endpoint::pump_result::error) {
            capture_endpoint_error();
        } else if (receive == http3_worker_datagram_endpoint::pump_result::stopped) {
            begin_stop();
        }
        if (!stopping_) {
            drive();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    } catch (...) {
        record_current_failure();
        throw;
    }
}

void http3_quic_wire_owner::request_stop() noexcept {
    require_owner_thread();
    capture_endpoint_error();
    begin_stop();
}

void http3_quic_wire_owner::request_drive() noexcept {
    require_owner_thread();
    if (!stopping_) {
        drive_requested_ = true;
        drive();
    }
}

void http3_quic_wire_owner::poll_datagrams() noexcept {
    require_owner_thread();
    endpoint_.poll_channel();
}

void http3_quic_wire_owner::defer_transport_retirement() noexcept {
    require_owner_thread();
    if (started_ || stopping_ || !prepared_) {
        std::terminate();
    }
    transport_retirement_released_ = false;
}

void http3_quic_wire_owner::release_transport_retirement() noexcept {
    require_owner_thread();
    transport_retirement_released_ = true;
}

void http3_quic_wire_owner::poll_stop() noexcept {
    require_owner_thread();
    if (transport_destroyed_) {
        return;
    }
    if (!stopping_ || driving_ || !timer_handlers_retired_ || !endpoint_.endpoint_retired() ||
        endpoint_.outbound_pending() || !transport_retirement_released_) {
        return;
    }
    transport_.reset();
    capture_endpoint_error();
    endpoint_.retire_channel();
    transport_destroyed_ = true;
}

http3_quic_wire_owner::stop_status_type http3_quic_wire_owner::stop_status() const noexcept {
    require_owner_thread();
    return stop_status_type{
        .stopping_ = stopping_,
        .endpoint_retired_ = endpoint_.endpoint_retired(),
        .timer_handlers_retired_ = timer_handlers_retired_,
        .transport_destroyed_ = transport_destroyed_,
        .outbound_pending_ = endpoint_.outbound_pending(),
        .failed_ = static_cast<bool>(failure_),
    };
}

std::uint16_t http3_quic_wire_owner::bound_port() const noexcept {
    require_owner_thread();
    return endpoint_.bound_port();
}

std::size_t http3_quic_wire_owner::timer_expirations() const noexcept {
    require_owner_thread();
    return timer_expirations_;
}

std::exception_ptr http3_quic_wire_owner::failure() const noexcept {
    require_owner_thread();
    return failure_;
}

void http3_quic_wire_owner::rethrow_failure() const {
    require_owner_thread();
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

void http3_quic_wire_owner::endpoint_notification(void* context_value,
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    auto& owner_value = *static_cast<http3_quic_wire_owner*>(context_value);
    try {
        owner_value.on_endpoint_notification(kind);
    } catch (...) {
        owner_value.record_current_failure();
    }
}

void http3_quic_wire_owner::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

void http3_quic_wire_owner::on_endpoint_notification(
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    require_owner_thread();
    if (kind == http3_worker_datagram_endpoint::notification_kind::stopping) {
        capture_endpoint_error();
        begin_stop();
        return;
    }
    if (!stopping_) {
        transport_activity_ = true;
        drive();
    }
}

void http3_quic_wire_owner::drive() noexcept {
    require_owner_thread();
    if (driving_) {
        drive_requested_ = true;
        return;
    }
    if (!started_ || stopping_ || !transport_) {
        return;
    }
    driving_ = true;
    constexpr unsigned maximum_drives_per_turn = 2;
    for (unsigned pass = 0; pass < maximum_drives_per_turn; ++pass) {
        drive_requested_ = false;
        if (stopping_ || !transport_) {
            break;
        }
        try {
            const auto result_value = protocol_pump_.drive_(protocol_pump_.context_,
                *transport_, endpoint_);
            if (result_value == protocol_pump_result_type::fatal) {
                record_failure(std::make_exception_ptr(
                    std::runtime_error("HTTP/3 server network protocol pump failed")));
                break;
            }
            drive_requested_ = drive_requested_ || result_value == protocol_pump_result_type::progress;
            update_deadline();
        } catch (...) {
            record_current_failure();
            break;
        }
        if (!drive_requested_) {
            break;
        }
    }
    driving_ = false;
}

void http3_quic_wire_owner::update_deadline() {
    if (stopping_ || !transport_) {
        desired_deadline_.reset();
    } else {
        desired_deadline_ = transport_->server().next_expiry();
    }
    reconcile_timer();
}

void http3_quic_wire_owner::reconcile_timer() noexcept {
    if (stopping_) {
        desired_deadline_.reset();
    }

    if (timer_wait_outstanding_) {
        if (timer_completion_delivered_) {
            return;
        }
        if (!stopping_ && desired_deadline_ && armed_deadline_ == desired_deadline_ &&
            !timer_cancel_requested_) {
            return;
        }
        if (!timer_cancel_requested_) {
            if (timer_generation_ == std::numeric_limits<std::uint64_t>::max()) {
                std::terminate();
            }
            ++timer_generation_;
            timer_cancel_requested_ = true;
            asio::error_code error;
            timer_.cancel();
            if (error) {
                try {
                    throw std::system_error(error, "cancel HTTP/3 server network QUIC timer");
                } catch (...) {
                    if (!failure_) {
                        failure_ = std::current_exception();
                    }
                    begin_stop();
                }
            }
        }
        return;
    }

    if (!stopping_ && desired_deadline_) {
        arm_timer(*desired_deadline_);
    } else {
        timer_handlers_retired_ = !timer_wake_outstanding_;
    }
}

void http3_quic_wire_owner::arm_timer(clock_type::time_point deadline_value) noexcept {
    if (timer_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++timer_generation_;
    const auto generation = timer_generation_;
    armed_generation_ = generation;
    armed_deadline_ = deadline_value;
    timer_wait_outstanding_ = true;
    timer_cancel_requested_ = false;
    timer_completion_delivered_ = false;
    timer_handlers_retired_ = false;
    try {
        timer_.expires_at(deadline_value);
        timer_submitting_ = true;
        timer_.async_wait(timer_wait_handler_type(*this, generation));
        timer_submitting_ = false;
    } catch (...) {
        // An async initiation failure can destroy its handler while unwinding,
        // without ever delivering a completion. This is not a retired wait.
        failed_timer_submission_generation_ = generation;
        timer_submitting_ = false;
        if (timer_wait_outstanding_ && armed_generation_ == generation) {
            timer_wait_outstanding_ = false;
            timer_completion_delivered_ = false;
            timer_cancel_requested_ = false;
            armed_deadline_.reset();
            timer_handlers_retired_ = !timer_wake_outstanding_;
        }
        record_current_failure();
    }
}

void http3_quic_wire_owner::on_timer_completion(std::uint64_t generation,
    const asio::error_code& error) noexcept {
    require_owner_thread();
    if (!timer_wait_outstanding_ || generation != armed_generation_ || timer_completion_delivered_) {
        std::terminate();
    }
    timer_completion_delivered_ = true;
    if (error) {
        if (error != asio::error::operation_aborted) {
            record_error(error, "HTTP/3 server network QUIC timer");
        }
        return;
    }
    ++timer_expirations_;
    transport_activity_ = true;
    if (generation != timer_generation_ || stopping_) {
        return;
    }
    drive();
}

void http3_quic_wire_owner::on_timer_handler_retired(std::uint64_t generation) noexcept {
    require_owner_thread();
    if (!timer_completion_delivered_ &&
        ((timer_submitting_ && generation == armed_generation_) ||
            generation == failed_timer_submission_generation_)) {
        return;
    }
    if (!timer_wait_outstanding_ || generation != armed_generation_ ||
        !timer_completion_delivered_) {
        std::terminate();
    }
    timer_wait_outstanding_ = false;
    timer_cancel_requested_ = false;
    timer_completion_delivered_ = false;
    armed_deadline_.reset();
    if (stopping_ || !desired_deadline_) {
        timer_handlers_retired_ = !timer_wake_outstanding_;
        return;
    }
    if (timer_wake_outstanding_) {
        std::terminate();
    }
    timer_wake_outstanding_ = true;
    timer_handlers_retired_ = false;
    try {
        asio::post(network_io_, timer_retirement_handler_type(*this));
    } catch (...) {
        timer_wake_outstanding_ = false;
        timer_handlers_retired_ = true;
        record_current_failure();
    }
}

void http3_quic_wire_owner::on_timer_retirement_wake() noexcept {
    require_owner_thread();
    if (!timer_wake_outstanding_) {
        std::terminate();
    }
    if (!stopping_) {
        reconcile_timer();
    }
}

void http3_quic_wire_owner::on_timer_retirement_handler_retired() noexcept {
    require_owner_thread();
    if (!timer_wake_outstanding_) {
        std::terminate();
    }
    timer_wake_outstanding_ = false;
    timer_handlers_retired_ = !timer_wait_outstanding_;
}

void http3_quic_wire_owner::begin_stop() noexcept {
    require_owner_thread();
    if (!stopping_) {
        stopping_ = true;
        desired_deadline_.reset();
        if (timer_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++timer_generation_;
        if (timer_wait_outstanding_ && !timer_cancel_requested_ &&
            !timer_completion_delivered_) {
            timer_cancel_requested_ = true;
            asio::error_code error;
            timer_.cancel();
            if (error) {
                try {
                    throw std::system_error(error, "cancel HTTP/3 server network QUIC timer");
                } catch (...) {
                    if (!failure_) {
                        failure_ = std::current_exception();
                    }
                }
            }
        }
    }
    endpoint_.request_stop();
    capture_endpoint_error();
}

void http3_quic_wire_owner::record_failure(std::exception_ptr failure) noexcept {
    if (!failure_) {
        failure_ = failure;
    }
    begin_stop();
}

void http3_quic_wire_owner::record_current_failure() noexcept {
    record_failure(std::current_exception());
}

void http3_quic_wire_owner::record_error(std::error_code error,
    const char* operation) noexcept {
    try {
        throw std::system_error(error, operation);
    } catch (...) {
        record_failure(std::current_exception());
    }
}

void http3_quic_wire_owner::capture_endpoint_error() noexcept {
    const auto error = endpoint_.error();
    if (error && !failure_) {
        try {
            throw std::system_error(error, "HTTP/3 server network datagram endpoint");
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
}

}  // namespace ruvia::detail
