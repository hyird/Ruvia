#include "http3/http3_quic_wire_owner.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/error.hpp>
#include <asio/post.hpp>

#include "ruvia/core/memory/process_resource.h"

#include "http3/http3_quic_socket_address.h"

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
        owner_->on_timer_completion(generation_, error);
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
        owner_->on_timer_retirement_wake();
    }
}

http3_quic_wire_owner::http3_quic_wire_owner(asio::io_context& worker_io,
    http3_datagram_channel& channel, http3_worker_datagram_endpoint::udp::endpoint local_endpoint,
    http3_quic_tls_context& tls, ruvia::quic_server_config transport_config,
    std::pmr::memory_resource* timer_handler_resource, wake_type wake)
    : owner_thread_(std::this_thread::get_id()),
      network_io_(worker_io),
      wake_(wake),
      endpoint_(channel, std::move(local_endpoint),
          http3_worker_datagram_endpoint::notification{this, &endpoint_notification}),
      timer_(network_io_),
      timer_handler_allocator_(timer_handler_resource != nullptr
                                   ? timer_handler_resource
                                   : ruvia::process_resource()) {
    try {
        if (wake_.wake_ == nullptr) {
            throw std::invalid_argument("HTTP/3 QUIC wire requires a wake target");
        }
        endpoint_.prepare();
        transport_.emplace(tls, transport_config, timer_handler_allocator_.resource());
    } catch (...) {
        // Cold rollback: nothing was started, so the stop completes in place.
        request_stop();
        poll_stop();
        if (!transport_destroyed_) {
            std::terminate();
        }
        throw;
    }
}

http3_quic_wire_owner::~http3_quic_wire_owner() {
    require_owner_thread();
    if (!stopping_) {
        request_stop();
    }
    poll_stop();
    if (!transport_destroyed_) {
        std::terminate();
    }
}

void http3_quic_wire_owner::start() noexcept {
    require_owner_thread();
    if (started_ || stopping_) {
        return;
    }
    started_ = true;
    const auto receive = endpoint_.start();
    if (receive == http3_worker_datagram_endpoint::pump_result::error ||
        receive == http3_worker_datagram_endpoint::pump_result::stopped) {
        capture_endpoint_error();
        request_stop();
    }
}

void http3_quic_wire_owner::poll_channel() noexcept {
    require_owner_thread();
    endpoint_.poll_channel();
}

http3_quic_wire_owner::received_type http3_quic_wire_owner::receive_one(clock_type::time_point now) {
    require_owner_thread();
    if (!started_ || stopping_) {
        return {};
    }
    const auto received = endpoint_.receive_slot();
    if (!received) {
        return {};
    }
    received_type result_value{.kind_ = received_type::kind_type::dropped};
    const auto local = to_quic_address(received->local_destination_);
    const auto peer = to_quic_address(received->peer_);
    if (local.index() == 0 && peer.index() == 0) {
        const ruvia::quic_datagram_view datagram{received->bytes_, std::get<0>(local), std::get<0>(peer)};
        auto& server = transport_->server();
        ruvia::quic_server_route route;
        try {
            route = server.route_datagram(datagram);
        } catch (const ruvia::quic_error& error) {
            if (error.code() != ruvia::quic_error_code::protocol_failure) {
                throw;
            }
        }
        switch (route.kind_) {
            case ruvia::quic_server_route_kind::initial_offer:
                result_value = {.kind_ = received_type::kind_type::initial_offer, .offer_ = route.offer_};
                break;
            case ruvia::quic_server_route_kind::version_negotiation: {
                const auto packet_buffer = endpoint_.packet_buffer();
                if (!packet_buffer.empty()) {
                    struct reservation_guard final {
                        http3_worker_datagram_endpoint& endpoint_;
                        ~reservation_guard() {
                            endpoint_.cancel_packet();
                        }
                    } guard_value{endpoint_};
                    const auto packet = server.write_version_negotiation(route.version_negotiation_, packet_buffer);
                    if (packet.status_ == ruvia::quic_operation_status::accepted &&
                        !send_packet(packet_buffer, packet)) {
                        throw std::runtime_error("QUIC server serialized an invalid Version Negotiation packet");
                    }
                }
                break;
            }
            case ruvia::quic_server_route_kind::existing_connection:
                result_value.connection_ = route.connection_;
                try {
                    (void)server.receive(route.connection_, datagram, now);
                    result_value.kind_ = received_type::kind_type::connection;
                } catch (const ruvia::quic_error&) {
                    result_value.kind_ = received_type::kind_type::failed_connection;
                }
                break;
            default:
                break;
        }
    }
    if (endpoint_.consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
        throw std::system_error(endpoint_.error(), "consume HTTP/3 UDP receive slot");
    }
    return result_value;
}

std::optional<std::size_t> http3_quic_wire_owner::send(ruvia::quic_connection_token token, clock_type::time_point now) {
    require_owner_thread();
    std::size_t sent = 0;
    if (!started_ || stopping_) {
        return sent;
    }
    try {
        auto& quic = transport_->server().connection(token);
        for (;;) {
            const auto packet_buffer = endpoint_.packet_buffer();
            if (packet_buffer.empty()) {
                // Production pauses; output_drained wakes the owner again.
                return sent;
            }
            struct reservation_guard final {
                http3_worker_datagram_endpoint& endpoint_;
                ~reservation_guard() {
                    endpoint_.cancel_packet();
                }
            } guard_value{endpoint_};
            const auto packet = quic.write_packet(packet_buffer, now);
            if (packet.size_ == 0) {
                return sent;
            }
            if (!send_packet(packet_buffer, packet)) {
                return std::nullopt;
            }
            ++sent;
        }
    } catch (const ruvia::quic_error&) {
        return std::nullopt;
    }
}

bool http3_quic_wire_owner::send_packet(std::span<std::byte> packet_buffer,
    const ruvia::quic_packet_result& packet) {
    const auto source = to_udp_endpoint(packet.local_);
    const auto peer = to_udp_endpoint(packet.peer_);
    if (source.index() != 0 || peer.index() != 0 || packet.size_ == 0 || packet.size_ > packet_buffer.size()) {
        return false;
    }
    const auto sent = endpoint_.send_datagram(
        std::span<const std::byte>(packet_buffer).first(packet.size_), std::get<0>(source), std::get<0>(peer));
    if (sent == http3_worker_datagram_endpoint::pump_result::error) {
        throw std::system_error(endpoint_.error(), "send HTTP/3 QUIC packet");
    }
    return true;
}

bool http3_quic_wire_owner::output_available() noexcept {
    require_owner_thread();
    return endpoint_.outbound_capacity();
}

void http3_quic_wire_owner::schedule(std::optional<clock_type::time_point> local_deadline) noexcept {
    require_owner_thread();
    if (stopping_ || !transport_) {
        desired_deadline_.reset();
    } else {
        desired_deadline_ = transport_->server().next_expiry();
        if (local_deadline && (!desired_deadline_ || *local_deadline < *desired_deadline_)) {
            desired_deadline_ = local_deadline;
        }
    }
    reconcile_timer();
}

bool http3_quic_wire_owner::take_expiry() noexcept {
    require_owner_thread();
    return std::exchange(expired_, false);
}

std::uint16_t http3_quic_wire_owner::bound_port() const noexcept {
    require_owner_thread();
    return endpoint_.bound_port();
}

void http3_quic_wire_owner::request_stop() noexcept {
    require_owner_thread();
    if (!stopping_) {
        stopping_ = true;
        desired_deadline_.reset();
        if (timer_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++timer_generation_;
        if (timer_wait_outstanding_ && !timer_cancel_requested_ && !timer_completion_delivered_) {
            timer_cancel_requested_ = true;
            timer_.cancel();
        }
    }
    endpoint_.request_stop();
    capture_endpoint_error();
}

void http3_quic_wire_owner::poll_stop() noexcept {
    require_owner_thread();
    if (transport_destroyed_ || !stopping_) {
        return;
    }
    endpoint_.poll_channel();
    if (!timer_handlers_retired_ || !endpoint_.endpoint_retired() || endpoint_.outbound_pending()) {
        return;
    }
    transport_.reset();
    capture_endpoint_error();
    endpoint_.retire_channel();
    transport_destroyed_ = true;
}

void http3_quic_wire_owner::endpoint_notification(void* context_value,
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    auto& owner_value = *static_cast<http3_quic_wire_owner*>(context_value);
    owner_value.require_owner_thread();
    // The endpoint notifies synchronously from the owner's own poll, receive,
    // start and stop calls, all made inside a protocol turn that observes the
    // result. Waking from here would be level-triggered: a retired endpoint or
    // pending input re-notifies on every poll, so the protocol loop would never
    // suspend and starve every other task on the worker. Cross-thread channel
    // events arrive through the worker notification instead.
    if (kind == http3_worker_datagram_endpoint::notification_kind::stopping) {
        // The Acceptor closed the channel or the endpoint failed.
        owner_value.capture_endpoint_error();
        owner_value.stopping_ = true;
        owner_value.desired_deadline_.reset();
        owner_value.reconcile_timer();
    }
}

void http3_quic_wire_owner::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

void http3_quic_wire_owner::wake() noexcept {
    if (wake_.wake_ != nullptr) {
        wake_.wake_(wake_.context_);
    }
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
            timer_.cancel();
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
            try {
                throw std::system_error(error, "HTTP/3 QUIC timer");
            } catch (...) {
                record_current_failure();
            }
        }
        return;
    }
    if (generation != timer_generation_ || stopping_) {
        return;
    }
    expired_ = true;
    wake();
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
        if (stopping_) {
            // poll_stop may now destroy the transport.
            wake();
        }
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
    if (stopping_) {
        wake();
    }
}

void http3_quic_wire_owner::record_failure(std::exception_ptr failure) noexcept {
    if (!failure_) {
        failure_ = failure;
    }
    request_stop();
    wake();
}

void http3_quic_wire_owner::record_current_failure() noexcept {
    record_failure(std::current_exception());
}

void http3_quic_wire_owner::capture_endpoint_error() noexcept {
    const auto error = endpoint_.error();
    if (error && !failure_) {
        try {
            throw std::system_error(error, "HTTP/3 worker datagram endpoint");
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
}

}  // namespace ruvia::detail
