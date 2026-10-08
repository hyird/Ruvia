#include "http3/http3_datagram_endpoint.h"

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
std::error_code io_failure() noexcept {
    return std::make_error_code(std::errc::io_error);
}

asio::ip::udp::endpoint checked_bind_endpoint(asio::ip::udp::endpoint endpoint) {
    if (!(endpoint.address().is_unspecified() || is_concrete_unicast(endpoint.address()))) {
        throw std::invalid_argument("HTTP/3 UDP bind requires wildcard or unicast IPv4/IPv6 without IPv6 scope");
    }
    return endpoint;
}

http3_datagram_endpoint_types::notification checked_notification(http3_datagram_endpoint_types::notification callback) {
    if (!callback.notify) {
        throw std::invalid_argument("HTTP/3 UDP notification is required");
    }
    return callback;
}

std::error_code packet_error(std::span<const std::byte> bytes, const asio::ip::udp::endpoint& source,
    const asio::ip::udp::endpoint& peer, const asio::ip::udp::endpoint& bound) noexcept {
    const auto max_payload = bound.address().is_v4() ? 65507U : 65527U;
    if (bytes.size() > max_payload) {
        return std::make_error_code(std::errc::message_size);
    }
    if (bytes.empty() || !matches_bound_destination(source, bound) ||
        !is_concrete_unicast(peer.address()) || peer.port() == 0 ||
        peer.address().is_v4() != bound.address().is_v4()) {
        return invalid_datagram();
    }
    return {};
}

}  // namespace

http3_acceptor_datagram_endpoint::http3_acceptor_datagram_endpoint(asio::io_context& network_io,
    udp::endpoint bind_endpoint, notification callback, buffer_pool& pool)
    : bind_endpoint_(checked_bind_endpoint(std::move(bind_endpoint))),
      owner_thread_(std::this_thread::get_id()),
      pool_(pool),
      socket_(network_io, bind_endpoint_),
      notification_(checked_notification(callback)) {}

std::uint16_t http3_acceptor_datagram_endpoint::bound_port() const noexcept {
    require_owner_thread();
    return prepared_ ? bound_endpoint_.port() : 0;
}

bool http3_acceptor_datagram_endpoint::outbound_pending() const noexcept {
    return outbound_count() != 0;
}

http3_acceptor_datagram_endpoint::stop_status http3_acceptor_datagram_endpoint::status() const noexcept {
    require_owner_thread();
    if (!stopping_ || callback_depth_ != 0 || !outbound_quiescent() || !endpoint_retired()) {
        return stop_status::pending;
    }
    return error_ ? stop_status::error : stop_status::done;
}

std::error_code http3_acceptor_datagram_endpoint::error() const noexcept {
    require_owner_thread();
    return error_;
}

void http3_acceptor_datagram_endpoint::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

void http3_acceptor_datagram_endpoint::notify(notification_kind kind) noexcept {
    ++callback_depth_;
    notification_.notify(notification_.context, kind);
    --callback_depth_;
}

void http3_acceptor_datagram_endpoint::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error ? error : io_failure();
    }
    request_stop();
}

bool http3_acceptor_datagram_endpoint::valid_packet(std::span<const std::byte> bytes,
    const udp::endpoint& source, const udp::endpoint& peer) noexcept {
    const auto error = packet_error(bytes, source, peer, bound_endpoint_);
    if (!prepared_ || !started_ || error) {
        fail(error ? error : invalid_datagram());
        return false;
    }
    return true;
}

http3_acceptor_datagram_endpoint::~http3_acceptor_datagram_endpoint() {
    require_owner_thread();
    if (!stopping_) {
        request_stop();
    }
    if (callback_depth_ != 0 || !endpoint_retired()) {
        std::terminate();
    }
}

void http3_acceptor_datagram_endpoint::prepare() {
    require_owner_thread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 native endpoint cannot prepare in this state");
    }
    try {
        socket_.prepare();
        bound_endpoint_ = udp::endpoint(bind_endpoint_.address(), socket_.bound_port());
        prepared_ = true;
    } catch (...) {
        request_stop();
        throw;
    }
}

http3_acceptor_datagram_endpoint::pump_result http3_acceptor_datagram_endpoint::start() noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (!prepared_ || started_) {
        fail(std::make_error_code(std::errc::operation_not_permitted));
        return pump_result::error;
    }
    started_ = true;
    (void)arm_receive();
    return error_ ? pump_result::error : stopping_ ? pump_result::stopped
                                                   : pump_result::pending;
}

std::optional<http3_acceptor_datagram_endpoint::datagram> http3_acceptor_datagram_endpoint::take_receive() noexcept {
    require_owner_thread();
    if (stopping_ || !held_receive_) {
        return std::nullopt;
    }
    auto packet = std::move(held_receive_);
    held_receive_.reset();
    return packet;
}

void http3_acceptor_datagram_endpoint::poll_receive() noexcept {
    require_owner_thread();
    (void)arm_receive();
}

http3_acceptor_datagram_endpoint::pump_result http3_acceptor_datagram_endpoint::send_owned_datagram(datagram&& packet) noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (!packet.storage || packet.size > packet.storage.bytes().size()) {
        fail(invalid_datagram());
        return pump_result::error;
    }
    if (!valid_packet(packet.view().bytes, packet.local_destination, packet.peer)) {
        return pump_result::error;
    }
    if (pending_send_) {
        return pump_result::pending;
    }
    pending_send_.emplace(std::move(packet));
    const auto view = pending_send_->view();
    if (!socket_.async_send({view.local_destination, view.peer, view.bytes}, this, send_completion)) {
        pending_send_.reset();
        fail(io_failure());
        return pump_result::error;
    }
    return pump_result::pending;
}

bool http3_acceptor_datagram_endpoint::outbound_capacity() const noexcept {
    require_owner_thread();
    return !stopping_ && started_ && !pending_send_;
}

std::size_t http3_acceptor_datagram_endpoint::outbound_count() const noexcept {
    require_owner_thread();
    return pending_send_.has_value();
}

bool http3_acceptor_datagram_endpoint::outbound_quiescent() const noexcept {
    require_owner_thread();
    return !pending_send_;
}

void http3_acceptor_datagram_endpoint::request_stop() noexcept {
    require_owner_thread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    held_receive_.reset();
    // Armed RX and native TX retain their leases until completion callbacks.
    socket_.request_stop();
    if (!stopping_notified_) {
        stopping_notified_ = true;
        notify(notification_kind::stopping);
    }
}

bool http3_acceptor_datagram_endpoint::endpoint_retired() const noexcept {
    require_owner_thread();
    return socket_.done();
}

void http3_acceptor_datagram_endpoint::receive_completion(void* context, std::error_code error,
    http3_udp_socket::receive_view view) noexcept {
    auto& self = *static_cast<http3_acceptor_datagram_endpoint*>(context);
    ++self.callback_depth_;
    self.receive_armed_ = false;
    self.handle_receive(error, std::move(view));
    if (self.stopping_) {
        self.notify(notification_kind::endpoint_retired);
    }
    --self.callback_depth_;
}

void http3_acceptor_datagram_endpoint::send_completion(void* context, std::error_code error, std::size_t size) noexcept {
    auto& self = *static_cast<http3_acceptor_datagram_endpoint*>(context);
    ++self.callback_depth_;
    self.handle_send(error, size);
    if (self.stopping_) {
        self.notify(notification_kind::endpoint_retired);
    }
    --self.callback_depth_;
}

bool http3_acceptor_datagram_endpoint::arm_receive() noexcept {
    if (stopping_ || !started_ || receive_armed_ || held_receive_) {
        return false;
    }
    receive_lease_ = pool_.try_acquire();
    if (!receive_lease_) {
        return false;
    }
    receive_armed_ = true;
    if (!socket_.async_receive(receive_lease_->bytes(), this, receive_completion)) {
        receive_armed_ = false;
        receive_lease_.reset();
        fail(io_failure());
        return false;
    }
    return true;
}

void http3_acceptor_datagram_endpoint::handle_receive(std::error_code error, http3_udp_socket::receive_view view) noexcept {
    if (stopping_) {
        receive_lease_.reset();
        return;
    }
    if (error) {
        receive_lease_.reset();
        if (error == invalid_datagram()) {
            (void)arm_receive();
        } else {
            fail(error);
        }
        return;
    }
    if (view.bytes.empty() || !is_concrete_unicast(view.peer.address()) || view.peer.port() == 0 ||
        !matches_bound_destination(view.local_destination, bound_endpoint_)) {
        receive_lease_.reset();
        (void)arm_receive();
        return;
    }
    if (held_receive_ || !receive_lease_ || view.bytes.data() != receive_lease_->bytes().data()) {
        std::terminate();
    }
    held_receive_.emplace(datagram{std::move(*receive_lease_), view.bytes.size(),
        view.local_destination, view.peer});
    receive_lease_.reset();
    notify(notification_kind::input_available);
}

void http3_acceptor_datagram_endpoint::handle_send(std::error_code error, std::size_t size) noexcept {
    if (!pending_send_) {
        std::terminate();
    }
    const auto expected_size = pending_send_->size;
    pending_send_.reset();
    if (stopping_) {
        return;
    }
    if (error) {
        fail(error);
    } else if (size != expected_size) {
        fail(io_failure());
    } else {
        notify(notification_kind::output_drained);
        (void)arm_receive();
    }
}

http3_worker_datagram_endpoint::http3_worker_datagram_endpoint(http3_datagram_channel& channel,
    udp::endpoint local_endpoint, notification callback)
    : bind_endpoint_(checked_bind_endpoint(std::move(local_endpoint))),
      owner_thread_(std::this_thread::get_id()),
      channel_(&channel),
      notification_(checked_notification(callback)) {}

std::uint16_t http3_worker_datagram_endpoint::bound_port() const noexcept {
    require_owner_thread();
    return prepared_ ? bound_endpoint_.port() : 0;
}

bool http3_worker_datagram_endpoint::outbound_pending() const noexcept {
    return outbound_count() != 0;
}

http3_worker_datagram_endpoint::stop_status http3_worker_datagram_endpoint::status() const noexcept {
    require_owner_thread();
    if (!stopping_ || callback_depth_ != 0 || !outbound_quiescent() || !endpoint_retired()) {
        return stop_status::pending;
    }
    return error_ ? stop_status::error : stop_status::done;
}

std::error_code http3_worker_datagram_endpoint::error() const noexcept {
    require_owner_thread();
    return error_;
}

void http3_worker_datagram_endpoint::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

void http3_worker_datagram_endpoint::notify(notification_kind kind) noexcept {
    ++callback_depth_;
    notification_.notify(notification_.context, kind);
    --callback_depth_;
}

void http3_worker_datagram_endpoint::fail(std::error_code error) noexcept {
    if (!error_) {
        error_ = error ? error : io_failure();
    }
    request_stop();
}

bool http3_worker_datagram_endpoint::valid_packet(std::span<const std::byte> bytes,
    const udp::endpoint& source, const udp::endpoint& peer) noexcept {
    const auto error = packet_error(bytes, source, peer, bound_endpoint_);
    if (!prepared_ || !started_ || error) {
        fail(error ? error : invalid_datagram());
        return false;
    }
    return true;
}

http3_worker_datagram_endpoint::~http3_worker_datagram_endpoint() {
    require_owner_thread();
    if (!stopping_) {
        request_stop();
    }
    if (callback_depth_ != 0 || !endpoint_retired()) {
        std::terminate();
    }
}

void http3_worker_datagram_endpoint::prepare() {
    require_owner_thread();
    if (prepared_ || stopping_) {
        throw std::logic_error("HTTP/3 worker endpoint cannot prepare in this state");
    }
    try {
        if (bind_endpoint_.port() == 0) {
            throw std::invalid_argument("HTTP/3 worker endpoint requires the bound acceptor port");
        }
        bound_endpoint_ = bind_endpoint_;
        prepared_ = true;
    } catch (...) {
        request_stop();
        throw;
    }
}

http3_worker_datagram_endpoint::pump_result http3_worker_datagram_endpoint::start() noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (!prepared_ || started_) {
        fail(std::make_error_code(std::errc::operation_not_permitted));
        return pump_result::error;
    }
    started_ = true;
    poll_channel();
    return error_ ? pump_result::error : stopping_ ? pump_result::stopped
                                                   : pump_result::pending;
}

std::optional<http3_worker_datagram_endpoint::received_datagram>
http3_worker_datagram_endpoint::receive_slot() const noexcept {
    require_owner_thread();
    if (stopping_) {
        return std::nullopt;
    }
    const auto received = channel_->worker_input();
    return received ? std::optional<received_datagram>{{received->bytes, received->peer, received->local_destination}} : std::nullopt;
}

http3_worker_datagram_endpoint::pump_result http3_worker_datagram_endpoint::consume_receive() noexcept {
    require_owner_thread();
    if (stopping_) {
        return pump_result::stopped;
    }
    if (channel_->worker_input()) {
        channel_->worker_consume_input();
    }
    if (channel_->worker_input()) {
        notify(notification_kind::input_available);
    }
    return pump_result::pending;
}

std::span<std::byte> http3_worker_datagram_endpoint::packet_buffer() noexcept {
    require_owner_thread();
    return stopping_ || !prepared_ || !started_ ? std::span<std::byte>{} : channel_->worker_output_buffer();
}

void http3_worker_datagram_endpoint::cancel_packet() noexcept {
    require_owner_thread();
    if (channel_) {
        channel_->worker_cancel_output();
    }
}

http3_worker_datagram_endpoint::pump_result http3_worker_datagram_endpoint::send_datagram(
    std::span<const std::byte> bytes, const udp::endpoint& source, const udp::endpoint& peer) noexcept {
    require_owner_thread();
    if (stopping_) {
        cancel_packet();
        return pump_result::stopped;
    }
    if (!valid_packet(bytes, source, peer)) {
        return pump_result::error;
    }
    if (channel_->worker_send(bytes, source, peer)) {
        ++observed_output_count_;
    }
    return pump_result::pending;
}

bool http3_worker_datagram_endpoint::outbound_capacity() const noexcept {
    require_owner_thread();
    return !stopping_ && started_ && channel_->worker_outbound_capacity();
}

std::size_t http3_worker_datagram_endpoint::outbound_count() const noexcept {
    require_owner_thread();
    return channel_ ? channel_->worker_outbound_count() : 0;
}

bool http3_worker_datagram_endpoint::outbound_quiescent() const noexcept {
    require_owner_thread();
    return !channel_ || channel_->worker_outbound_quiescent();
}

void http3_worker_datagram_endpoint::poll_channel() noexcept {
    require_owner_thread();
    if (!channel_) {
        return;
    }
    const auto count = channel_->worker_outbound_count();
    if (count < observed_output_count_) {
        observed_output_count_ = count;
        notify(notification_kind::output_drained);
    }
    if (!stopping_ && channel_->acceptor_closed()) {
        error_ = channel_->error();
        request_stop();
    }
    if (stopping_) {
        channel_->worker_stop();
        if (endpoint_retired()) {
            notify(notification_kind::endpoint_retired);
        }
    } else if (channel_->worker_input()) {
        notify(notification_kind::input_available);
    }
}

void http3_worker_datagram_endpoint::request_stop() noexcept {
    require_owner_thread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    channel_->worker_stop();
    if (!stopping_notified_) {
        stopping_notified_ = true;
        notify(notification_kind::stopping);
    }
}

bool http3_worker_datagram_endpoint::endpoint_retired() const noexcept {
    require_owner_thread();
    return stopping_ && (!channel_ || (channel_->acceptor_closed() && channel_->worker_outbound_quiescent()));
}

void http3_worker_datagram_endpoint::retire_channel() noexcept {
    require_owner_thread();
    if (!channel_) {
        return;
    }
    if (status() == stop_status::pending) {
        std::terminate();
    }
    if (!error_) {
        error_ = channel_->error();
    }
    channel_ = nullptr;
}

}  // namespace ruvia::detail
