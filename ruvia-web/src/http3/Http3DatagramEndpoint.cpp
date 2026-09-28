#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

namespace ruvia::detail {
namespace {

bool isConcreteUnicast(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        return bytes[0] != 0 && bytes[0] < 224;
    }
    if (!address.is_v6()) {
        return false;
    }
    const auto ipv6 = address.to_v6();
    return !ipv6.is_unspecified() && !ipv6.is_multicast() && !ipv6.is_v4_mapped() &&
           !ipv6.is_link_local() && ipv6.scope_id() == 0;
}

bool isSupportedBindAddress(const asio::ip::address& address) noexcept {
    return address.is_unspecified() || isConcreteUnicast(address);
}

bool matchesBoundDestination(
    const asio::ip::udp::endpoint& destination, const asio::ip::udp::endpoint& bound) noexcept {
    return destination.port() == bound.port() &&
           destination.address().is_v4() == bound.address().is_v4() &&
           isConcreteUnicast(destination.address()) &&
           (bound.address().is_unspecified() || destination.address() == bound.address());
}

std::error_code invalidDatagram() noexcept {
    return std::make_error_code(std::errc::bad_message);
}

std::error_code operationNotPermitted() noexcept {
    return std::make_error_code(std::errc::operation_not_permitted);
}

std::error_code ioFailure() noexcept {
    return std::make_error_code(std::errc::io_error);
}

}  // namespace

Http3DatagramEndpoint::BridgeLease::BridgeLease(
    Http3DatagramEndpoint& owner) noexcept
    : owner_(&owner) {}

Http3DatagramEndpoint::BridgeLease::BridgeLease(BridgeLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

Http3DatagramEndpoint::BridgeLease&
Http3DatagramEndpoint::BridgeLease::operator=(BridgeLease&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

Http3DatagramEndpoint::BridgeLease::~BridgeLease() {
    reset();
}

Http3QuicDatagramBridge& Http3DatagramEndpoint::BridgeLease::bridge() const noexcept {
    if (owner_ == nullptr || !owner_->bridge_) {
        std::terminate();
    }
    owner_->requireOwnerThread();
    return *owner_->bridge_;
}

void Http3DatagramEndpoint::BridgeLease::reset() noexcept {
    if (owner_ != nullptr) {
        auto* const owner = std::exchange(owner_, nullptr);
        owner->releaseBridgeLease();
    }
}

Http3DatagramEndpoint::Http3DatagramEndpoint(asio::io_context& networkIo,
    Udp::endpoint bindEndpoint, Notification notification)
    : bindEndpoint_(checkedBindEndpoint(std::move(bindEndpoint))),
      ownerThread_(std::this_thread::get_id()),
      socket_(networkIo, bindEndpoint_),
      notification_(checkedNotification(notification)) {}

Http3DatagramEndpoint::~Http3DatagramEndpoint() {
    requireOwnerThread();
    if (!stopping_) {
        requestStop();
    }
    if (callbackDepth_ != 0 || bridgeLeaseActive_ || !socket_.done()) {
        std::terminate();
    }
}

Http3DatagramEndpoint::Udp::endpoint
Http3DatagramEndpoint::checkedBindEndpoint(Udp::endpoint endpoint) {
    if (!isSupportedBindAddress(endpoint.address())) {
        throw std::invalid_argument(
            "HTTP/3 server network datagram bind requires a wildcard or unicast IPv4/IPv6 address without IPv6 scope");
    }
    return endpoint;
}

Http3DatagramEndpoint::Notification
Http3DatagramEndpoint::checkedNotification(Notification notification) {
    if (notification.notify == nullptr) {
        throw std::invalid_argument("HTTP/3 server network datagram notification is required");
    }
    return notification;
}

void Http3DatagramEndpoint::prepare() {
    requireOwnerThread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 server network datagram endpoint cannot be prepared in this state");
    }

    try {
        socket_.prepare();
        boundEndpoint_ = Udp::endpoint(bindEndpoint_.address(), socket_.boundPort());
        const auto boundAddress = toHttp3QuicBindAddress(boundEndpoint_);
        if (!boundAddress) {
            throw std::invalid_argument("HTTP/3 server network UDP bind address cannot be represented by QUIC BIO");
        }
        bridge_.emplace(*boundAddress);
        prepared_ = true;
    } catch (...) {
        requestStop();
        throw;
    }
}

std::uint16_t Http3DatagramEndpoint::boundPort() const noexcept {
    requireOwnerThread();
    return prepared_ ? boundEndpoint_.port() : 0;
}

Http3DatagramEndpoint::BridgeLease
Http3DatagramEndpoint::acquireBridge() {
    requireOwnerThread();
    if (!prepared_ || stopping_ || !bridge_ || bridgeLeaseActive_) {
        throw std::logic_error("HTTP/3 server network datagram bridge is not available for leasing");
    }
    bridgeLeaseActive_ = true;
    return BridgeLease(*this);
}

Http3DatagramEndpoint::PumpResult Http3DatagramEndpoint::start() noexcept {
    requireOwnerThread();
    if (stopping_) {
        return PumpResult::kStopped;
    }
    if (!prepared_ || started_) {
        fail(operationNotPermitted());
        return PumpResult::kError;
    }
    started_ = true;
    return armReceive() ? PumpResult::kPending : PumpResult::kError;
}

Http3DatagramEndpoint::PumpResult
Http3DatagramEndpoint::retryHeldReceive() noexcept {
    requireOwnerThread();
    if (stopping_) {
        return PumpResult::kStopped;
    }
    if (!hasHeldReceive_) {
        return PumpResult::kIdle;
    }
    return injectHeldReceive();
}

Http3DatagramEndpoint::PumpResult
Http3DatagramEndpoint::sendPending() noexcept {
    requireOwnerThread();
    return sendPendingImpl(false);
}

Http3DatagramEndpoint::PumpResult
Http3DatagramEndpoint::sendPendingImpl(bool notifyWhenDrained) noexcept {
    if (stopping_) {
        return PumpResult::kStopped;
    }
    if (!prepared_ || !bridge_) {
        fail(operationNotPermitted());
        return PumpResult::kError;
    }
    if (sendInFlight_) {
        return PumpResult::kPending;
    }

    Http3QuicOutboundDatagram outbound;
    switch (bridge_->takeOutbound(outbound)) {
        case Http3QuicDatagramBridge::OutboundResult::kEmpty:
            if (notifyWhenDrained) {
                notify(NotificationKind::kOutputDrained);
            }
            return stopping_ ? PumpResult::kStopped : PumpResult::kIdle;
        case Http3QuicDatagramBridge::OutboundResult::kBusy:
            return PumpResult::kPending;
        case Http3QuicDatagramBridge::OutboundResult::kFatal:
            fail(ioFailure());
            return PumpResult::kError;
        case Http3QuicDatagramBridge::OutboundResult::kReady:
            break;
    }

    const auto destination = toHttp3UdpEndpoint(outbound.destination);
    if (!destination) {
        bridge_->completeOutbound();
        fail(invalidDatagram());
        return PumpResult::kError;
    }

    Udp::endpoint source = boundEndpoint_;
    if (outbound.hasSource) {
        const auto explicitSource = toHttp3UdpEndpoint(outbound.source);
        if (!explicitSource || !matchesBoundDestination(*explicitSource, boundEndpoint_)) {
            bridge_->completeOutbound();
            fail(invalidDatagram());
            return PumpResult::kError;
        }
        source = *explicitSource;
    } else if (boundEndpoint_.address().is_unspecified()) {
        // A wildcard socket needs the concrete pktinfo-derived source address
        // OpenSSL attached to this datagram; sending from the wildcard would
        // let the kernel choose a different local address than the peer used.
        bridge_->completeOutbound();
        fail(invalidDatagram());
        return PumpResult::kError;
    }

    sendSize_ = outbound.bytes.size();
    sendInFlight_ = true;
    if (!socket_.asyncSend(Http3UdpSocket::SendView{source, *destination, outbound.bytes},
            this, &Http3DatagramEndpoint::sendCompletion)) {
        sendInFlight_ = false;
        sendSize_ = 0;
        bridge_->completeOutbound();
        fail(ioFailure());
        return PumpResult::kError;
    }
    return PumpResult::kPending;
}

void Http3DatagramEndpoint::requestStop() noexcept {
    requireOwnerThread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    heldReceive_ = {};
    hasHeldReceive_ = false;
    heldReceiveBackpressureNotified_ = false;
    socket_.requestStop();
    if (!stoppingNotified_) {
        stoppingNotified_ = true;
        notify(NotificationKind::kStopping);
    }
}

bool Http3DatagramEndpoint::socketDone() const noexcept {
    requireOwnerThread();
    return socket_.done();
}

Http3DatagramEndpoint::StopStatus
Http3DatagramEndpoint::stopStatus() const noexcept {
    requireOwnerThread();
    if (!stopping_ || callbackDepth_ != 0 || bridgeLeaseActive_ || sendInFlight_ ||
        !socket_.done()) {
        return StopStatus::kPending;
    }
    return error_ ? StopStatus::kError : StopStatus::kDone;
}

bool Http3DatagramEndpoint::sendInFlight() const noexcept {
    requireOwnerThread();
    return sendInFlight_;
}

bool Http3DatagramEndpoint::outboundQuiescent() const noexcept {
    requireOwnerThread();
    return bridge_.has_value() && bridge_->outboundQuiescent();
}

std::error_code Http3DatagramEndpoint::error() const noexcept {
    requireOwnerThread();
    return error_;
}

void Http3DatagramEndpoint::receiveCompletion(void* context, std::error_code error,
    Http3UdpSocket::ReceiveView view) noexcept {
    auto& self = *static_cast<Http3DatagramEndpoint*>(context);
    ++self.callbackDepth_;
    self.receiveArmed_ = false;
    self.handleReceive(error, std::move(view));
    --self.callbackDepth_;
}

void Http3DatagramEndpoint::sendCompletion(void* context, std::error_code error,
    std::size_t size) noexcept {
    auto& self = *static_cast<Http3DatagramEndpoint*>(context);
    ++self.callbackDepth_;
    self.handleSend(error, size);
    --self.callbackDepth_;
}

void Http3DatagramEndpoint::requireOwnerThread() const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
}

void Http3DatagramEndpoint::releaseBridgeLease() noexcept {
    requireOwnerThread();
    if (!bridgeLeaseActive_) {
        std::terminate();
    }
    bridgeLeaseActive_ = false;
}

bool Http3DatagramEndpoint::armReceive() noexcept {
    if (stopping_ || !started_ || receiveArmed_ || hasHeldReceive_) {
        return false;
    }
    receiveArmed_ = true;
    if (!socket_.asyncReceive(this, &Http3DatagramEndpoint::receiveCompletion)) {
        receiveArmed_ = false;
        fail(ioFailure());
        return false;
    }
    return true;
}

Http3DatagramEndpoint::PumpResult
Http3DatagramEndpoint::injectHeldReceive() noexcept {
    if (!hasHeldReceive_ || !bridge_) {
        fail(operationNotPermitted());
        return PumpResult::kError;
    }

    const auto peer = toHttp3QuicDatagramAddress(heldReceive_.peer);
    const auto local = toHttp3QuicDatagramAddress(heldReceive_.localDestination);
    if (!peer || !local ||
        !matchesBoundDestination(heldReceive_.localDestination, boundEndpoint_)) {
        // A malformed or misdirected peer packet must not close the shared UDP socket.
        heldReceive_ = {};
        hasHeldReceive_ = false;
        heldReceiveBackpressureNotified_ = false;
        return armReceive() ? PumpResult::kPending : PumpResult::kError;
    }

    switch (bridge_->inject(heldReceive_.bytes, *peer, *local)) {
        case Http3QuicDatagramBridge::InjectResult::kFull:
            if (!heldReceiveBackpressureNotified_) {
                heldReceiveBackpressureNotified_ = true;
                notify(NotificationKind::kInputAvailable);
            }
            return stopping_ ? PumpResult::kStopped : PumpResult::kBackpressured;
        case Http3QuicDatagramBridge::InjectResult::kFatal:
            fail(invalidDatagram());
            return PumpResult::kError;
        case Http3QuicDatagramBridge::InjectResult::kAccepted:
            heldReceive_ = {};
            hasHeldReceive_ = false;
            heldReceiveBackpressureNotified_ = false;
            notify(NotificationKind::kInputAvailable);
            if (!stopping_) {
                (void)armReceive();
            }
            return error_      ? PumpResult::kError
                   : stopping_ ? PumpResult::kStopped
                               : PumpResult::kPending;
    }
    std::terminate();
}

void Http3DatagramEndpoint::handleReceive(std::error_code error,
    Http3UdpSocket::ReceiveView view) noexcept {
    if (error) {
        if (!stopping_) {
            // Truncated datagrams and malformed pktinfo are packet-local errors.
            // Continue receiving rather than letting one datagram stop the server network runtime.
            if (error == invalidDatagram()) {
                (void)armReceive();
            } else {
                fail(error);
            }
        }
        return;
    }
    if (stopping_) {
        return;
    }
    if (view.bytes.empty()) {
        (void)armReceive();
        return;
    }
    if (hasHeldReceive_) {
        std::terminate();
    }
    heldReceive_ = std::move(view);
    hasHeldReceive_ = true;
    (void)injectHeldReceive();
}

void Http3DatagramEndpoint::handleSend(std::error_code error,
    std::size_t size) noexcept {
    if (!sendInFlight_ || !bridge_) {
        std::terminate();
    }
    sendInFlight_ = false;
    const std::size_t expectedSize = sendSize_;
    sendSize_ = 0;
    bridge_->completeOutbound();

    if (stopping_) {
        return;
    }
    if (error) {
        fail(error);
    } else if (size != expectedSize) {
        fail(ioFailure());
    } else {
        (void)sendPendingImpl(true);
    }
}

void Http3DatagramEndpoint::notify(NotificationKind kind) noexcept {
    ++callbackDepth_;
    notification_.notify(notification_.context, kind);
    --callbackDepth_;
}

void Http3DatagramEndpoint::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error ? error : ioFailure();
    }
    requestStop();
}

}  // namespace ruvia::detail
