#include "server/acceptor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/socket_base.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/process_resource.h"

#include "http3/http3_datagram_channel.h"

namespace ruvia::detail {
namespace {

// Errors that concern only the connection being accepted or a transient
// resource shortage. accept(2) reports network errors already pending on the
// new connection and requires callers to treat them like EAGAIN; none of these
// invalidate the listener, so accepting resumes after a short backoff.
bool recoverable_accept_error(const asio::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again || error == asio::error::would_block ||
        error == asio::error::interrupted || error == asio::error::no_descriptors ||
        error == asio::error::no_buffer_space || error == asio::error::no_memory ||
        error == std::errc::too_many_files_open_in_system || error == std::errc::protocol_error ||
        error == asio::error::network_down || error == asio::error::network_unreachable ||
        error == asio::error::host_unreachable || error == asio::error::no_protocol_option ||
        error == asio::error::operation_not_supported || error == asio::error::no_permission ||
        error == asio::error::timed_out) {
        return true;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    return error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET);
#else
    return false;
#endif
}

// Bind the listener exclusively. Windows SO_REUSEADDR (asio's reuse_address)
// lets another socket bind a port that is already listening, so Windows uses
// SO_EXCLUSIVEADDRUSE; elsewhere SO_REUSEADDR only permits rebinding over
// TIME_WAIT connections.
void set_listener_address_option(asio::ip::tcp::acceptor& socket, asio::error_code& error) noexcept {
#if defined(_WIN32)
    const BOOL exclusive = TRUE;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        error = asio::error_code(::WSAGetLastError(), asio::system_category());
    }
#else
    socket.set_option(asio::socket_base::reuse_address(true), error);
#endif
}

}  // namespace

acceptor::acceptor(std::span<const http_server_listener_definition> listeners,
    std::span<const worker_target> targets, void* failure_target, failure_callback failure)
    : runtime_({.queue_capacity_ = 128, .io_policy_ = worker_io_policy::single_owner}),
      io_context_(runtime_.context().io_context()),
      listeners_(process_resource()),
      targets_(targets.begin(), targets.end(), process_resource()),
      quic_channels_(process_resource()),
      udp_(nullptr, pmr_object_deleter<http3_acceptor_datagram_endpoint>{process_resource()}),
      quic_notification_(runtime_.context()),
      quic_visits_(process_resource()),
      quic_retirement_(io_context_),
      failure_target_(failure_target),
      failure_callback_(failure) {
    for (const auto& target : targets_) {
        if (!target.submission_.valid()) {
            throw std::invalid_argument("acceptor worker target is incomplete");
        }
    }
    const auto quic_count = std::ranges::count_if(listeners, [](const auto& listener) { return listener.http3_.has_value(); });
    listeners_.reserve(listeners.size());
    quic_channels_.reserve(quic_count != 0 ? targets.size() : 0);
    for (const auto& configured : listeners) {
        listeners_.push_back(make_pmr_object<listener>(process_resource(), io_context_,
            configured.endpoint_, configured.http3_ ? std::optional(normalize_http3_capacity(*configured.http3_, targets.size())) : std::nullopt));
    }
    runtime_.configure({
        .startup_ = [this] {
            prepare_quic();
            runtime_.mark_ready(); },
        .serve_ = [this] {
            if (udp_ && udp_->start() == http3_acceptor_datagram_endpoint::pump_result::error) {
                throw std::system_error(udp_->error(), "start QUIC acceptor UDP listener");
            }
            for (std::size_t i = 0; i < listeners_.size(); ++i) {
                begin_accept(i);
            } },
        .stop_admission_ = [this] { stop_on_owner(); },
        .failure_ = [this](std::exception_ptr) noexcept { failure_callback_(failure_target_); },
        .shutdown_ = [this]() noexcept {
            close_listeners();
            udp_.reset();
            quic_channels_.clear();
            quic_ready_.reset();
            quic_pool_.reset(); },
    });
}

acceptor::~acceptor() {
    // Ingress has no external producers: it finalizes itself once QUIC retires.
    runtime_.request_stop();
    runtime_.join();
}

void acceptor::prepare_quic() {
    const auto configured = std::ranges::find_if(listeners_,
        [](const auto& bound) { return bound->quic_.has_value(); });
    if (configured == listeners_.end()) {
        return;
    }
    const auto& tcp = (*configured)->endpoint_;
    const auto capacity = *(*configured)->quic_;
    quic_pool_.emplace(capacity.packet_slots_,
        http3_capacity::packet_bytes, process_resource());
    quic_ready_.emplace(quic_notification_, targets_.size(), process_resource());
    quic_visits_.reserve(targets_.size());
    udp_ = make_pmr_object<http3_acceptor_datagram_endpoint>(process_resource(), io_context_,
        asio::ip::udp::endpoint(tcp.address(), tcp.port()),
        http3_acceptor_datagram_endpoint::notification{this, &datagram_ready}, *quic_pool_);
    udp_->prepare();
    const asio::ip::udp::endpoint endpoint(tcp.address(), udp_->bound_port());
    quic_running_ = true;
    try {
        asio::co_spawn(io_context_, ruvia::as_awaitable(run_quic()),
            asio::bind_allocator(asio::recycling_allocator<void>(),
                [this](std::exception_ptr error) noexcept {
                    quic_running_ = false;
                    if (error) {
                        (void)runtime_.fail(std::move(error));
                        // An unrecoverable runner error must not bypass leases.
                        stop_on_owner();
                        if (!quic_retired()) {
                            std::terminate();
                        }
                        quic_notification_.close();
                    }
                    runtime_.finalize();
                }));
    } catch (...) {
        quic_running_ = false;
        throw;
    }
    for (std::size_t worker_value = 0; worker_value < targets_.size(); ++worker_value) {
        auto channel = make_pmr_object<http3_datagram_channel>(process_resource(),
            *quic_pool_, *quic_ready_, worker_value, process_resource(),
            capacity.input_slots_, capacity.output_credits_);
        // A staging failure must not leave an unowned channel waiting for an ACK.
        try {
            targets_[worker_value].stage_quic_(targets_[worker_value].object_, *channel, endpoint,
                {.index_ = static_cast<std::uint32_t>(worker_value),
                    .count_ = static_cast<std::uint32_t>(targets_.size())});
        } catch (...) {
            channel->acceptor_close();
            channel->abandon_worker();
            if (!channel->acceptor_finalize()) {
                std::terminate();
            }
            throw;
        }
        quic_channels_.push_back(std::move(channel));
    }
}

task<void> acceptor::run_quic() {
    for (;;) {
        pump_quic();
        if (quic_retired()) {
            quic_notification_.close();
            co_return;
        }
        if (quic_stopping_) {
            // ACK publication follows the last cross-thread notification call.
            // A cold timer covers wake-before-ACK without hot-path polling.
            quic_retirement_.expires_after(std::chrono::milliseconds(1));
            const auto completed = co_await ruvia::async_asio([this](auto handler) {
                quic_retirement_.async_wait(std::move(handler));
            });
            if (completed.error_code()) {
                throw std::system_error(completed.error_code(), "QUIC acceptor retirement wait");
            }
        } else {
            try {
                if (co_await quic_notification_.wait() == worker_notification_wait_status::closed) {
                    stop_on_owner();
                }
            } catch (...) {
                (void)runtime_.fail(std::current_exception());
                stop_on_owner();
            }
        }
    }
}

bool acceptor::quic_retired() noexcept {
    if (!quic_stopping_) {
        return false;
    }
    // Drain both normal and cold-abandoned owners before examining final ACKs.
    pump_quic();
    if (!udp_->endpoint_retired()) {
        return false;
    }
    for (auto& channel : quic_channels_) {
        if (!channel->acceptor_finalize()) {
            return false;
        }
    }
    return quic_pool_->outstanding() == 0;
}

void acceptor::datagram_ready(void* object, http3_acceptor_datagram_endpoint::notification_kind) noexcept {
    auto& self = *static_cast<acceptor*>(object);
    (void)self.quic_notification_.notify();
}

void acceptor::pump_quic() noexcept {
    if (const auto error = udp_->error()) {
        try {
            throw std::system_error(error, "QUIC acceptor UDP I/O");
        } catch (...) {
            (void)runtime_.fail(std::current_exception());
        }
        stop_on_owner();
    }
    if (quic_stopping_) {
        // Retirement is cold: every owner is polled. Queued descriptors retire
        // here; a descriptor already submitted to native UDP remains inside the
        // endpoint until its callback.
        for (auto& channel : quic_channels_) {
            channel->acceptor_poll();
            while (channel->acceptor_take_output()) {
            }
        }
        return;
    }
    // Only channels that signalled since the last wake are polled; each credit
    // lane uses one bounded two-wrap batch and re-signals when more remain.
    quic_visits_.clear();
    quic_ready_->drain([this](std::size_t index) noexcept {
        // A staging rollback can signal an index that never joined the channels.
        if (index < quic_channels_.size()) {
            quic_channels_[index]->acceptor_poll();
            quic_visits_.push_back(static_cast<std::uint32_t>(index));
        }
    });
    // Route every held packet. The endpoint holds at most its receive slots and
    // completes none during this loop, which bounds the per-wake batch.
    while (auto packet = udp_->take_receive()) {
        const auto worker_value = ruvia::quic_datagram_partition(packet->view().bytes_,
            static_cast<std::uint32_t>(targets_.size()));
        if (worker_value) {
            (void)quic_channels_[*worker_value]->acceptor_push(std::move(*packet));
        }
    }
    // Routing, drops and independent RX credit returns make storage available.
    // No second receive pool and no packet copy exist at this edge.
    udp_->poll_receive();
    // Send one packet per signalled channel per round, starting after the last
    // channel served, while native send slots remain.
    const auto first = std::ranges::lower_bound(quic_visits_, next_output_);
    std::ranges::rotate(quic_visits_, first);
    while (!quic_visits_.empty() && udp_->outbound_capacity()) {
        std::size_t kept = 0;
        for (const auto index : quic_visits_) {
            if (!udp_->outbound_capacity()) {
                quic_visits_[kept++] = index;
                continue;
            }
            auto packet = quic_channels_[index]->acceptor_take_output();
            if (!packet) {
                continue;
            }
            (void)udp_->send_owned_datagram(std::move(*packet));
            next_output_ = index + 1;
            quic_visits_[kept++] = index;
        }
        quic_visits_.resize(kept);
    }
    // Send slots ran out first. A native send completion wakes the next pump,
    // which revisits every channel not yet seen empty.
    for (const auto index : quic_visits_) {
        quic_ready_->mark(index);
    }
}

void acceptor::prepare() {
    if (runtime_.state() != runtime_lifecycle::state_type::ready) {
        throw std::logic_error("acceptor listeners must be prepared before launch");
    }
    for (auto& configured : listeners_) {
        auto& socket = configured->socket_;
        asio::error_code error;
        socket.open(configured->endpoint_.protocol(), error);
        if (!error) {
            set_listener_address_option(socket, error);
        }
        if (!error) {
            socket.bind(configured->endpoint_, error);
        }
        if (!error) {
            socket.listen(asio::socket_base::max_listen_connections, error);
        }
        if (!error) {
            configured->endpoint_ = socket.local_endpoint(error);
        }
        if (error) {
            close_listeners();
            throw std::system_error(error, "failed to prepare TCP acceptor listener");
        }
    }
}

void acceptor::close_admission() noexcept {
    // A refused control means stopping has already closed the listeners.
    (void)runtime_.post_control([this] {
        admitting_ = false;
        close_listeners();
    });
}

bool acceptor::admitting() const noexcept {
    return admitting_ && runtime_.state() == runtime_lifecycle::state_type::running;
}

void acceptor::stop_on_owner() noexcept {
    admitting_ = false;
    close_listeners();
    quic_stopping_ = true;
    if (udp_) {
        udp_->request_stop();
    }
    for (auto& channel : quic_channels_) {
        channel->acceptor_close(udp_ ? udp_->error() : std::error_code{});
    }
    if (quic_running_) {
        (void)quic_notification_.notify();
    } else {
        quic_notification_.close();
        runtime_.finalize();
    }
}

void acceptor::begin_accept(std::size_t index) noexcept {
    if (!admitting() || targets_.empty()) {
        return;
    }
    auto& socket = listeners_[index]->socket_;
    if (!socket.is_open()) {
        return;
    }
    try {
        socket.async_accept([this, index](const asio::error_code& error, asio::ip::tcp::socket accepted_socket) mutable {
            accepted(index, error, std::move(accepted_socket));
        });
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void acceptor::accepted(std::size_t index, const asio::error_code& error,
    asio::ip::tcp::socket socket) noexcept {
    if (error) {
        if (error == asio::error::operation_aborted && !admitting()) {
            return;
        }
        if (!recoverable_accept_error(error)) {
            try {
                throw std::system_error(error, "fatal TCP accept error");
            } catch (...) {
                (void)runtime_.fail(std::current_exception());
            }
            return;
        }
        schedule_retry(index);
        return;
    }
    asio::error_code release_error;
    const auto native = socket.release(release_error);
    if (release_error) {
        try {
            throw std::system_error(release_error, "failed to detach accepted TCP socket");
        } catch (...) {
            (void)runtime_.fail(std::current_exception());
        }
        return;
    }
    native_accepted_socket_ticket ticket(listeners_[index]->endpoint_.protocol(), index, native);
    if (!admitting() || targets_.empty()) {
        return;
    }
    auto selected = targets_.size();
    for (std::size_t offset = 0; offset < targets_.size(); ++offset) {
        const auto worker_value = (next_target_ + offset) % targets_.size();
        if (targets_[worker_value].available_(targets_[worker_value].object_) && targets_[worker_value].submission_.accepting()) {
            selected = worker_value;
            next_target_ = (worker_value + 1) % targets_.size();
            break;
        }
    }
    if (selected == targets_.size()) {
        // Dropping the ticket closes the connection at ingress.
        refused_connections_.fetch_add(1, std::memory_order_relaxed);
        schedule_retry(index);
        return;
    }
    const auto target = targets_[selected];
    try {
        // Selection above is advisory. The worker's accept path holds the one
        // authoritative capacity check and counts its own refusals.
        auto posted = target.submission_.post([target, ticket = std::move(ticket)]() mutable noexcept {
            target.accept_(target.object_, std::move(ticket));
        });
        if (!posted.accepted()) {
            refused_connections_.fetch_add(1, std::memory_order_relaxed);
            auto rejected = std::move(posted).take_rejected();
            rejected = {};
        }
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
        return;
    }
    if (admitting()) {
        begin_accept(index);
    }
}

void acceptor::schedule_retry(std::size_t index) noexcept {
    if (!admitting()) {
        return;
    }
    try {
        auto& timer = listeners_[index]->retry_;
        timer.expires_after(std::chrono::milliseconds(25));
        timer.async_wait([this, index](const asio::error_code& error) {
            if (!error) {
                begin_accept(index);
            }
        });
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void acceptor::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
        configured->retry_.cancel();
        ignored.clear();
        configured->socket_.cancel(ignored);
        ignored.clear();
        configured->socket_.close(ignored);
    }
}

}  // namespace ruvia::detail
