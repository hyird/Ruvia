#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/error.hpp>

namespace ruvia::detail {
namespace {

bool is_concrete_unicast(const asio::ip::address& address) noexcept {
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

bool matches_bound_destination(const asio::ip::udp::endpoint& destination,
    const asio::ip::udp::endpoint& bound) noexcept {
    return destination.port() == bound.port() &&
           destination.address().is_v4() == bound.address().is_v4() &&
           is_concrete_unicast(destination.address()) &&
           (bound.address().is_unspecified() || destination.address() == bound.address());
}

std::error_code invalid_datagram() noexcept {
    return std::make_error_code(std::errc::bad_message);
}

std::error_code operation_not_permitted() noexcept {
    return std::make_error_code(std::errc::operation_not_permitted);
}

std::error_code io_failure() noexcept {
    return std::make_error_code(std::errc::io_error);
}

}  // namespace

Http3DatagramEndpoint::Http3DatagramEndpoint(asio::io_context& network_io,
    udp::endpoint bind_endpoint, notification callback)
    : bind_endpoint_(checked_bind_endpoint(std::move(bind_endpoint))),
      owner_thread_(std::this_thread::get_id()),
      socket_(network_io, bind_endpoint_),
      notification_(checked_notification(callback)) {}

Http3DatagramEndpoint::~Http3DatagramEndpoint() {
    require_owner_thread();
    if (!stopping_) {
        request_stop();
    }
    if (callback_depth_ != 0 || !socket_.done()) {
        std::terminate();
    }
}

Http3DatagramEndpoint::udp::endpoint
Http3DatagramEndpoint::checked_bind_endpoint(udp::endpoint endpoint) {
    if (!(endpoint.address().is_unspecified() || is_concrete_unicast(endpoint.address()))) {
        throw std::invalid_argument(
            "HTTP/3 UDP bind requires wildcard or unicast IPv4/IPv6 without IPv6 scope");
    }
    return endpoint;
}

Http3DatagramEndpoint::notification
Http3DatagramEndpoint::checked_notification(notification callback) {
    if (callback.notify == nullptr) {
        throw std::invalid_argument("HTTP/3 UDP notification is required");
    }
    return callback;
}

void Http3DatagramEndpoint::prepare() {
    require_owner_thread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 UDP endpoint cannot prepare in this state");
    }
    try {
        socket_.prepare();
        bound_endpoint_ = udp::endpoint(bind_endpoint_.address(), socket_.boundPort());
        prepared_ = true;
    } catch (...) {
        request_stop();
        throw;
    }
}

std::uint16_t Http3DatagramEndpoint::bound_port() const noexcept {
    require_owner_thread();
    return prepared_ ? bound_endpoint_.port() : 0;
}

Http3DatagramEndpoint::pump_result Http3DatagramEndpoint::start() noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (!prepared_ || started_) {
        fail(operation_not_permitted());
        return pump_result::error;
    }
    started_ = true;
    return arm_receive() ? pump_result::pending : pump_result::error;
}

std::optional<Http3DatagramEndpoint::received_datagram>
Http3DatagramEndpoint::receive_slot() const noexcept {
    require_owner_thread();
    if (!has_held_receive_) {
        return std::nullopt;
    }
    return received_datagram{held_receive_.bytes, held_receive_.peer,
        held_receive_.localDestination};
}

Http3DatagramEndpoint::pump_result Http3DatagramEndpoint::consume_receive() noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (!has_held_receive_) {
        return pump_result::idle;
    }
    held_receive_ = {};
    has_held_receive_ = false;
    return arm_receive() ? pump_result::pending : pump_result::error;
}

Http3DatagramEndpoint::pump_result Http3DatagramEndpoint::send_datagram(
    std::span<const std::byte> bytes, const udp::endpoint& source,
    const udp::endpoint& peer) noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    const std::size_t max_payload = bound_endpoint_.address().is_v4() ? 65507 : 65527;
    if (!prepared_ || !started_ || bytes.empty() ||
        bytes.size() > max_payload || send_in_flight_ ||
        !matches_bound_destination(source, bound_endpoint_) ||
        !is_concrete_unicast(peer.address()) || peer.port() == 0 ||
        peer.address().is_v4() != bound_endpoint_.address().is_v4()) {
        if (send_in_flight_) {
            return pump_result::pending;
        }
        fail(bytes.size() > max_payload ? std::make_error_code(std::errc::message_size)
                                                : invalid_datagram());
        return pump_result::error;
    }
    std::ranges::copy(bytes, send_buffer_.begin());
    send_size_ = bytes.size();
    send_in_flight_ = true;
    const auto accepted = socket_.asyncSend(
        Http3UdpSocket::SendView{source, peer, std::span<const std::byte>(send_buffer_).first(send_size_)},
        this, &Http3DatagramEndpoint::send_completion);
    if (!accepted) {
        send_in_flight_ = false;
        send_size_ = 0;
        fail(io_failure());
        return pump_result::error;
    }
    return pump_result::pending;
}

bool Http3DatagramEndpoint::send_in_flight() const noexcept {
    require_owner_thread();
    return send_in_flight_;
}

bool Http3DatagramEndpoint::outbound_quiescent() const noexcept {
    require_owner_thread();
    return !send_in_flight_;
}

void Http3DatagramEndpoint::request_stop() noexcept {
    require_owner_thread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    held_receive_ = {};
    has_held_receive_ = false;
    socket_.requestStop();
    if (!stopping_notified_) {
        stopping_notified_ = true;
        notify(notification_kind::stopping);
    }
}

bool Http3DatagramEndpoint::socket_done() const noexcept {
    require_owner_thread();
    return socket_.done();
}

Http3DatagramEndpoint::stop_status Http3DatagramEndpoint::status() const noexcept {
    require_owner_thread();
    if (!stopping_ || callback_depth_ != 0 || send_in_flight_ || !socket_.done()) {
        return stop_status::pending;
    }
    return error_ ? stop_status::error : stop_status::done;
}

std::error_code Http3DatagramEndpoint::error() const noexcept {
    require_owner_thread();
    return error_;
}

void Http3DatagramEndpoint::receive_completion(void* context, std::error_code error,
    Http3UdpSocket::ReceiveView view) noexcept {
    auto& self = *static_cast<Http3DatagramEndpoint*>(context);
    ++self.callback_depth_;
    self.receive_armed_ = false;
    self.handle_receive(error, std::move(view));
    --self.callback_depth_;
}

void Http3DatagramEndpoint::send_completion(void* context, std::error_code error,
    std::size_t size) noexcept {
    auto& self = *static_cast<Http3DatagramEndpoint*>(context);
    ++self.callback_depth_;
    self.handle_send(error, size);
    --self.callback_depth_;
}

void Http3DatagramEndpoint::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

bool Http3DatagramEndpoint::arm_receive() noexcept {
    if (stopping_ || !started_ || receive_armed_ || has_held_receive_) {
        return false;
    }
    receive_armed_ = true;
    if (!socket_.asyncReceive(this, &Http3DatagramEndpoint::receive_completion)) {
        receive_armed_ = false;
        fail(io_failure());
        return false;
    }
    return true;
}

void Http3DatagramEndpoint::handle_receive(std::error_code error,
    Http3UdpSocket::ReceiveView view) noexcept {
    if (error) {
        if (stopping_) {
            return;
        }
        if (error == invalid_datagram()) {
            (void)arm_receive();
        } else {
            fail(error);
        }
        return;
    }
    if (stopping_) {
        return;
    }
    if (view.bytes.empty()) {
        (void)arm_receive();
        return;
    }
    if (has_held_receive_) {
        std::terminate();
    }
    if (!is_concrete_unicast(view.peer.address()) || view.peer.port() == 0 ||
        !matches_bound_destination(view.localDestination, bound_endpoint_)) {
        (void)arm_receive();
        return;
    }
    held_receive_ = std::move(view);
    has_held_receive_ = true;
    notify(notification_kind::input_available);
}

void Http3DatagramEndpoint::handle_send(std::error_code error,
    std::size_t size) noexcept {
    if (!send_in_flight_) {
        std::terminate();
    }
    send_in_flight_ = false;
    const auto expected_size = send_size_;
    send_size_ = 0;
    if (stopping_) {
        return;
    }
    if (error) {
        fail(error);
    } else if (size != expected_size) {
        fail(io_failure());
    } else {
        notify(notification_kind::output_drained);
    }
}

void Http3DatagramEndpoint::notify(notification_kind kind) noexcept {
    ++callback_depth_;
    notification_.notify(notification_.context, kind);
    --callback_depth_;
}

void Http3DatagramEndpoint::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error ? error : io_failure();
    }
    request_stop();
}

}  // namespace ruvia::detail
