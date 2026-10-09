#include "http3/Http3QuicWireOwner.h"

#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/post.hpp>

#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {

Http3QuicWireOwner::TimerWaitHandler::TimerWaitHandler(
    Http3QuicWireOwner& owner, std::uint64_t generation) noexcept
    : owner_(&owner),
      generation_(generation),
      allocator_(owner.timerHandlerAllocator_) {}

Http3QuicWireOwner::TimerWaitHandler::TimerWaitHandler(
    TimerWaitHandler&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      generation_(other.generation_),
      allocator_(other.allocator_) {}

Http3QuicWireOwner::TimerWaitHandler::~TimerWaitHandler() {
    if (owner_ != nullptr) {
        owner_->onTimerHandlerRetired(generation_);
    }
}

void Http3QuicWireOwner::TimerWaitHandler::operator()(
    const asio::error_code& error) noexcept {
    if (owner_ != nullptr) {
        try {
            owner_->onTimerCompletion(generation_, error);
        } catch (...) {
            owner_->recordCurrentFailure();
        }
    }
}

Http3QuicWireOwner::TimerRetirementHandler::TimerRetirementHandler(
    Http3QuicWireOwner& owner) noexcept
    : owner_(&owner),
      allocator_(owner.timerHandlerAllocator_) {}

Http3QuicWireOwner::TimerRetirementHandler::TimerRetirementHandler(
    TimerRetirementHandler&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      allocator_(other.allocator_) {}

Http3QuicWireOwner::TimerRetirementHandler::~TimerRetirementHandler() {
    if (owner_ != nullptr) {
        owner_->onTimerRetirementHandlerRetired();
    }
}

void Http3QuicWireOwner::TimerRetirementHandler::operator()() noexcept {
    if (owner_ != nullptr) {
        try {
            owner_->onTimerRetirementWake();
        } catch (...) {
            owner_->recordCurrentFailure();
        }
    }
}

Http3QuicWireOwner::Http3QuicWireOwner(asio::io_context& worker_io,
    http3_datagram_channel& channel, http3_worker_datagram_endpoint::udp::endpoint local_endpoint,
    http3_quic_tls_context& tls, ruvia::quic_server_config transport_config,
    std::pmr::memory_resource* timer_handler_resource, ProtocolPump protocol_pump)
    : ownerThread_(std::this_thread::get_id()),
      networkIo_(worker_io),
      tls_(tls),
      transportConfig_(transport_config),
      endpoint_(channel, std::move(local_endpoint),
          http3_worker_datagram_endpoint::notification{this, &endpointNotification}),
      timer_(networkIo_),
      timerHandlerAllocator_(timer_handler_resource != nullptr
                                 ? timer_handler_resource
                                 : ruvia::detail::processResource()),
      protocolPump_(protocol_pump) {
    if (protocolPump_.context == nullptr || protocolPump_.drive == nullptr) {
        throw std::invalid_argument("HTTP/3 wire protocol pump must be complete");
    }
}

Http3QuicWireOwner::~Http3QuicWireOwner() {
    requireOwnerThread();
    if (!stopping_) {
        requestStop();
    }
    if (!stopStatus().complete()) {
        std::terminate();
    }
}

void Http3QuicWireOwner::prepare() {
    requireOwnerThread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 server network QUIC wire owner cannot prepare in this state");
    }
    try {
        endpoint_.prepare();
        transport_.emplace(tls_, transportConfig_, timerHandlerAllocator_.resource());
        transportDestroyed_ = false;
        prepared_ = true;
    } catch (...) {
        recordCurrentFailure();
        throw;
    }
}

void Http3QuicWireOwner::start() {
    requireOwnerThread();
    if (!prepared_ || started_ || stopping_) {
        throw std::logic_error("HTTP/3 server network QUIC wire owner cannot start in this state");
    }
    try {
        started_ = true;
        const auto receive = endpoint_.start();
        if (receive == http3_worker_datagram_endpoint::pump_result::error) {
            captureEndpointError();
        } else if (receive == http3_worker_datagram_endpoint::pump_result::stopped) {
            beginStop();
        }
        if (!stopping_) {
            drive();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    } catch (...) {
        recordCurrentFailure();
        throw;
    }
}

void Http3QuicWireOwner::requestStop() noexcept {
    requireOwnerThread();
    captureEndpointError();
    beginStop();
}

void Http3QuicWireOwner::requestDrive() noexcept {
    requireOwnerThread();
    if (!stopping_) {
        driveRequested_ = true;
        drive();
    }
}

void Http3QuicWireOwner::poll_datagrams() noexcept {
    requireOwnerThread();
    endpoint_.poll_channel();
}

void Http3QuicWireOwner::deferTransportRetirement() noexcept {
    requireOwnerThread();
    if (started_ || stopping_ || !prepared_) {
        std::terminate();
    }
    transportRetirementReleased_ = false;
}

void Http3QuicWireOwner::releaseTransportRetirement() noexcept {
    requireOwnerThread();
    transportRetirementReleased_ = true;
}

void Http3QuicWireOwner::pollStop() noexcept {
    requireOwnerThread();
    if (transportDestroyed_) {
        return;
    }
    if (!stopping_ || driving_ || !timerHandlersRetired_ || !endpoint_.endpoint_retired() ||
        endpoint_.outbound_pending() || !transportRetirementReleased_) {
        return;
    }
    transport_.reset();
    captureEndpointError();
    endpoint_.retire_channel();
    transportDestroyed_ = true;
}

Http3QuicWireOwner::StopStatus Http3QuicWireOwner::stopStatus() const noexcept {
    requireOwnerThread();
    return StopStatus{
        .stopping = stopping_,
        .endpoint_retired = endpoint_.endpoint_retired(),
        .timerHandlersRetired = timerHandlersRetired_,
        .transportDestroyed = transportDestroyed_,
        .outbound_pending = endpoint_.outbound_pending(),
        .failed = static_cast<bool>(failure_),
    };
}

std::uint16_t Http3QuicWireOwner::boundPort() const noexcept {
    requireOwnerThread();
    return endpoint_.bound_port();
}

std::size_t Http3QuicWireOwner::timerExpirations() const noexcept {
    requireOwnerThread();
    return timerExpirations_;
}

std::exception_ptr Http3QuicWireOwner::failure() const noexcept {
    requireOwnerThread();
    return failure_;
}

void Http3QuicWireOwner::rethrowFailure() const {
    requireOwnerThread();
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

void Http3QuicWireOwner::endpointNotification(void* context,
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    auto& owner = *static_cast<Http3QuicWireOwner*>(context);
    try {
        owner.onEndpointNotification(kind);
    } catch (...) {
        owner.recordCurrentFailure();
    }
}

void Http3QuicWireOwner::requireOwnerThread() const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
}

void Http3QuicWireOwner::onEndpointNotification(
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    requireOwnerThread();
    if (kind == http3_worker_datagram_endpoint::notification_kind::stopping) {
        captureEndpointError();
        beginStop();
        return;
    }
    if (!stopping_) {
        transportActivity_ = true;
        drive();
    }
}

void Http3QuicWireOwner::drive() noexcept {
    requireOwnerThread();
    if (driving_) {
        driveRequested_ = true;
        return;
    }
    if (!started_ || stopping_ || !transport_) {
        return;
    }
    driving_ = true;
    constexpr unsigned maximum_drives_per_turn = 2;
    for (unsigned pass = 0; pass < maximum_drives_per_turn; ++pass) {
        driveRequested_ = false;
        if (stopping_ || !transport_) {
            break;
        }
        try {
            const auto result = protocolPump_.drive(protocolPump_.context,
                *transport_, endpoint_);
            if (result == ProtocolPumpResult::kFatal) {
                recordFailure(std::make_exception_ptr(
                    std::runtime_error("HTTP/3 server network protocol pump failed")));
                break;
            }
            driveRequested_ = driveRequested_ || result == ProtocolPumpResult::kProgress;
            updateDeadline();
        } catch (...) {
            recordCurrentFailure();
            break;
        }
        if (!driveRequested_) {
            break;
        }
    }
    driving_ = false;
}

void Http3QuicWireOwner::updateDeadline() {
    if (stopping_ || !transport_) {
        desiredDeadline_.reset();
    } else {
        desiredDeadline_ = transport_->server().next_expiry();
    }
    reconcileTimer();
}

void Http3QuicWireOwner::reconcileTimer() noexcept {
    if (stopping_) {
        desiredDeadline_.reset();
    }

    if (timerWaitOutstanding_) {
        if (timerCompletionDelivered_) {
            return;
        }
        if (!stopping_ && desiredDeadline_ && armedDeadline_ == desiredDeadline_ &&
            !timerCancelRequested_) {
            return;
        }
        if (!timerCancelRequested_) {
            if (timerGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
                std::terminate();
            }
            ++timerGeneration_;
            timerCancelRequested_ = true;
            asio::error_code error;
            timer_.cancel();
            if (error) {
                try {
                    throw std::system_error(error, "cancel HTTP/3 server network QUIC timer");
                } catch (...) {
                    if (!failure_) {
                        failure_ = std::current_exception();
                    }
                    beginStop();
                }
            }
        }
        return;
    }

    if (!stopping_ && desiredDeadline_) {
        armTimer(*desiredDeadline_);
    } else {
        timerHandlersRetired_ = !timerWakeOutstanding_;
    }
}

void Http3QuicWireOwner::armTimer(Clock::time_point deadline) noexcept {
    if (timerGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++timerGeneration_;
    const auto generation = timerGeneration_;
    armedGeneration_ = generation;
    armedDeadline_ = deadline;
    timerWaitOutstanding_ = true;
    timerCancelRequested_ = false;
    timerCompletionDelivered_ = false;
    timerHandlersRetired_ = false;
    try {
        timer_.expires_at(deadline);
        timerSubmitting_ = true;
        timer_.async_wait(TimerWaitHandler(*this, generation));
        timerSubmitting_ = false;
    } catch (...) {
        // An async initiation failure can destroy its handler while unwinding,
        // without ever delivering a completion. This is not a retired wait.
        failedTimerSubmissionGeneration_ = generation;
        timerSubmitting_ = false;
        if (timerWaitOutstanding_ && armedGeneration_ == generation) {
            timerWaitOutstanding_ = false;
            timerCompletionDelivered_ = false;
            timerCancelRequested_ = false;
            armedDeadline_.reset();
            timerHandlersRetired_ = !timerWakeOutstanding_;
        }
        recordCurrentFailure();
    }
}

void Http3QuicWireOwner::onTimerCompletion(std::uint64_t generation,
    const asio::error_code& error) noexcept {
    requireOwnerThread();
    if (!timerWaitOutstanding_ || generation != armedGeneration_ || timerCompletionDelivered_) {
        std::terminate();
    }
    timerCompletionDelivered_ = true;
    if (error) {
        if (error != asio::error::operation_aborted) {
            recordError(error, "HTTP/3 server network QUIC timer");
        }
        return;
    }
    ++timerExpirations_;
    transportActivity_ = true;
    if (generation != timerGeneration_ || stopping_) {
        return;
    }
    drive();
}

void Http3QuicWireOwner::onTimerHandlerRetired(std::uint64_t generation) noexcept {
    requireOwnerThread();
    if (!timerCompletionDelivered_ &&
        ((timerSubmitting_ && generation == armedGeneration_) ||
            generation == failedTimerSubmissionGeneration_)) {
        return;
    }
    if (!timerWaitOutstanding_ || generation != armedGeneration_ ||
        !timerCompletionDelivered_) {
        std::terminate();
    }
    timerWaitOutstanding_ = false;
    timerCancelRequested_ = false;
    timerCompletionDelivered_ = false;
    armedDeadline_.reset();
    if (stopping_ || !desiredDeadline_) {
        timerHandlersRetired_ = !timerWakeOutstanding_;
        return;
    }
    if (timerWakeOutstanding_) {
        std::terminate();
    }
    timerWakeOutstanding_ = true;
    timerHandlersRetired_ = false;
    try {
        asio::post(networkIo_, TimerRetirementHandler(*this));
    } catch (...) {
        timerWakeOutstanding_ = false;
        timerHandlersRetired_ = true;
        recordCurrentFailure();
    }
}

void Http3QuicWireOwner::onTimerRetirementWake() noexcept {
    requireOwnerThread();
    if (!timerWakeOutstanding_) {
        std::terminate();
    }
    if (!stopping_) {
        reconcileTimer();
    }
}

void Http3QuicWireOwner::onTimerRetirementHandlerRetired() noexcept {
    requireOwnerThread();
    if (!timerWakeOutstanding_) {
        std::terminate();
    }
    timerWakeOutstanding_ = false;
    timerHandlersRetired_ = !timerWaitOutstanding_;
}

void Http3QuicWireOwner::beginStop() noexcept {
    requireOwnerThread();
    if (!stopping_) {
        stopping_ = true;
        desiredDeadline_.reset();
        if (timerGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++timerGeneration_;
        if (timerWaitOutstanding_ && !timerCancelRequested_ &&
            !timerCompletionDelivered_) {
            timerCancelRequested_ = true;
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
    captureEndpointError();
}

void Http3QuicWireOwner::recordFailure(std::exception_ptr failure) noexcept {
    if (!failure_) {
        failure_ = failure;
    }
    beginStop();
}

void Http3QuicWireOwner::recordCurrentFailure() noexcept {
    recordFailure(std::current_exception());
}

void Http3QuicWireOwner::recordError(std::error_code error,
    const char* operation) noexcept {
    try {
        throw std::system_error(error, operation);
    } catch (...) {
        recordFailure(std::current_exception());
    }
}

void Http3QuicWireOwner::captureEndpointError() noexcept {
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
