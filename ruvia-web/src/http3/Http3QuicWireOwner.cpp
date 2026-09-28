#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"

#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>
#include <asio/post.hpp>

namespace ruvia::detail {
namespace {

using Duration = Http3QuicWireOwner::Clock::duration;

Duration positiveTimerDelay(Duration delay) noexcept {
    const auto minimum = std::chrono::duration_cast<Duration>(std::chrono::milliseconds(1));
    const auto floor = minimum > Duration::zero() ? minimum : Duration{1};
    return delay < floor ? floor : delay;
}

Http3QuicWireOwner::Clock::time_point deadlineAfter(Duration delay) noexcept {
    const auto now = Http3QuicWireOwner::Clock::now();
    delay = positiveTimerDelay(delay);
    const auto available = Http3QuicWireOwner::Clock::time_point::max() - now;
    return delay >= available ? Http3QuicWireOwner::Clock::time_point::max()
                              : now + delay;
}

}  // namespace

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

Http3QuicWireOwner::Http3QuicWireOwner(asio::io_context& networkIo,
    Http3DatagramEndpoint::Udp::endpoint bindEndpoint, Http3QuicTlsContext& tls,
    Http3QuicServerTransportConfig transportConfig,
    std::pmr::memory_resource* timerHandlerResource)
    : Http3QuicWireOwner(networkIo, std::move(bindEndpoint), tls, transportConfig,
          timerHandlerResource, {}) {}

Http3QuicWireOwner::Http3QuicWireOwner(asio::io_context& networkIo,
    Http3DatagramEndpoint::Udp::endpoint bindEndpoint, Http3QuicTlsContext& tls,
    Http3QuicServerTransportConfig transportConfig,
    std::pmr::memory_resource* timerHandlerResource, ProtocolPump protocolPump)
    : ownerThread_(std::this_thread::get_id()),
      networkIo_(networkIo),
      tls_(tls),
      transportConfig_(transportConfig),
      endpoint_(networkIo_, std::move(bindEndpoint),
          Http3DatagramEndpoint::Notification{this, &endpointNotification}),
      timer_(networkIo_),
      timerHandlerAllocator_(timerHandlerResource != nullptr
                                 ? timerHandlerResource
                                 : std::pmr::get_default_resource()),
      protocolPump_(protocolPump) {
    if ((protocolPump_.context == nullptr) != (protocolPump_.drive == nullptr)) {
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
        bridgeLease_.emplace(endpoint_.acquireBridge());
        bridgeLeaseReleased_ = false;
        transport_.emplace(tls_, bridgeLease_->bridge(), transportConfig_);
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
        if (receive == Http3DatagramEndpoint::PumpResult::kError) {
            captureEndpointError();
        } else if (receive == Http3DatagramEndpoint::PumpResult::kStopped) {
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

void Http3QuicWireOwner::pollStop() noexcept {
    requireOwnerThread();
    if (!stopping_ || driving_ || !timerHandlersRetired_ || !endpoint_.socketDone() ||
        endpoint_.sendInFlight()) {
        return;
    }

    transport_.reset();
    transportDestroyed_ = true;
    bridgeLease_.reset();
    bridgeLeaseReleased_ = true;
    captureEndpointError();
}

Http3QuicWireOwner::StopStatus
Http3QuicWireOwner::stopStatus() const noexcept {
    requireOwnerThread();
    return StopStatus{
        .stopping = stopping_,
        .socketDone = endpoint_.socketDone(),
        .timerHandlersRetired = timerHandlersRetired_,
        .transportDestroyed = transportDestroyed_,
        .bridgeLeaseReleased = bridgeLeaseReleased_,
        .sendInFlight = endpoint_.sendInFlight(),
        .failed = static_cast<bool>(failure_),
    };
}

std::uint16_t Http3QuicWireOwner::boundPort() const noexcept {
    requireOwnerThread();
    return endpoint_.boundPort();
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
    Http3DatagramEndpoint::NotificationKind kind) noexcept {
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
    Http3DatagramEndpoint::NotificationKind kind) noexcept {
    requireOwnerThread();
    if (kind == Http3DatagramEndpoint::NotificationKind::kStopping) {
        captureEndpointError();
        beginStop();
        return;
    }
    if (!stopping_) {
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
    constexpr unsigned kMaximumDrivesPerTurn = 2;
    for (unsigned pass = 0; pass < kMaximumDrivesPerTurn; ++pass) {
        driveRequested_ = false;
        if (stopping_ || !transport_) {
            break;
        }
        if (endpoint_.sendInFlight()) {
            driveRequested_ = true;
            break;
        }

        try {
            if (transport_->handleEvents() == Http3QuicServerTransport::EventResult::kFatal) {
                recordFailure(std::make_exception_ptr(
                    std::runtime_error("OpenSSL QUIC event handling failed on server network")));
                break;
            }

            const auto receive = endpoint_.retryHeldReceive();
            if (receive == Http3DatagramEndpoint::PumpResult::kError) {
                captureEndpointError();
                break;
            }
            if (receive == Http3DatagramEndpoint::PumpResult::kStopped || stopping_) {
                beginStop();
                break;
            }

            if (protocolPump_.drive != nullptr) {
                const auto protocol = protocolPump_.drive(protocolPump_.context, *transport_);
                if (protocol == ProtocolPumpResult::kFatal) {
                    recordFailure(std::make_exception_ptr(
                        std::runtime_error("HTTP/3 server network protocol pump failed")));
                    break;
                }
                driveRequested_ =
                    driveRequested_ || protocol == ProtocolPumpResult::kProgress;
            }

            const auto send = endpoint_.sendPending();
            if (send == Http3DatagramEndpoint::PumpResult::kError) {
                captureEndpointError();
                break;
            }
            if (send == Http3DatagramEndpoint::PumpResult::kStopped || stopping_) {
                beginStop();
                break;
            }

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
        const auto timeout = transport_->eventTimeout();
        desiredDeadline_ = timeout ? std::optional<Clock::time_point>(deadlineAfter(*timeout))
                                   : std::nullopt;
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
            timer_.cancel(error);
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
    if (generation != timerGeneration_ || stopping_) {
        return;
    }
    if (endpoint_.sendInFlight()) {
        // The UDP completion owns the bridge's outbound bytes. Only its real
        // completion notification may resume QUIC progress.
        desiredDeadline_.reset();
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
            timer_.cancel(error);
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
    endpoint_.requestStop();
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
