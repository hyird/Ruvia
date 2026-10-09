#include "server/acceptor.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/socket_base.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/memory/ProcessResource.h"

#include "http3/http3_datagram_channel.h"

namespace ruvia::detail {
namespace {

bool recoverable_accept_error(const asio::error_code& error) noexcept {
    return error == asio::error::connection_aborted || error == asio::error::connection_reset ||
           error == asio::error::eof || error == asio::error::try_again ||
           error == asio::error::would_block || error == asio::error::no_descriptors ||
           error == asio::error::no_buffer_space;
}

}  // namespace

acceptor::acceptor(std::span<const HttpServerListenerDefinition> listeners,
    std::span<const worker_target> targets, void* failure_target, failure_callback failure)
    : runtime_({.queue_capacity = 128, .io_policy = worker_io_policy::single_owner}),
      io_context_(runtime_.context().ioContext()),
      listeners_(processResource()),
      targets_(targets.begin(), targets.end(), processResource()),
      quic_channels_(processResource()),
      udp_(nullptr, PmrObjectDeleter<http3_acceptor_datagram_endpoint>{processResource()}),
      quic_notification_(runtime_.context()),
      quic_retirement_(io_context_),
      failure_target_(failure_target),
      failure_callback_(failure) {
    if (listeners.empty()) {
        throw std::invalid_argument("acceptor requires at least one TCP listener");
    }
    const auto quic_count = std::ranges::count_if(listeners,
        [](const auto& configured) { return configured.http3.has_value(); });
    if (quic_count > 1) {
        throw std::invalid_argument("acceptor supports one QUIC listener");
    }
    if (quic_count != 0 && (targets.empty() ||
                               targets.size() > std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("QUIC acceptor requires representable worker targets");
    }
    for (const auto& target : targets_) {
        if (!target.submission.valid() || target.object == nullptr || target.available == nullptr ||
            target.accept == nullptr || (quic_count != 0 && target.stage_quic == nullptr)) {
            throw std::invalid_argument("acceptor worker target is incomplete");
        }
    }
    listeners_.reserve(listeners.size());
    quic_channels_.reserve(quic_count != 0 ? targets.size() : 0);
    for (const auto& configured : listeners) {
        listeners_.push_back(makePmrObject<listener>(processResource(), io_context_,
            configured.endpoint, configured.http3 ? std::optional(normalize_http3_capacity(*configured.http3, targets.size())) : std::nullopt));
    }
    runtime_.configure({
        .startup = [this] {
            prepare_quic();
            if (runtime_.state() == RuntimeLifecycle::State::kRunning) {
                (void)completion_.mark_startup_ready();
            } else {
                completion_.mark_startup_aborted();
            } },
        .stop_admission = [this] { stop_on_owner(); },
        .failure = [this](std::exception_ptr error) noexcept { fail(std::move(error)); },
        .shutdown = [this]() noexcept {
            close_listeners();
            udp_.reset();
            quic_channels_.clear();
            quic_pool_.reset();
            completion_.mark_startup_aborted();
            completion_.mark_serving_aborted(); },
    });
}

acceptor::~acceptor() {
    stop();
    join();
}

void acceptor::prepare_quic() {
    const auto configured = std::ranges::find_if(listeners_,
        [](const auto& bound) { return bound->quic.has_value(); });
    if (configured == listeners_.end()) {
        return;
    }
    const auto& tcp = (*configured)->endpoint;
    const auto capacity = *(*configured)->quic;
    quic_pool_.emplace(capacity.packet_slots,
        http3_capacity::packet_bytes, processResource());
    udp_ = makePmrObject<http3_acceptor_datagram_endpoint>(processResource(), io_context_,
        asio::ip::udp::endpoint(tcp.address(), tcp.port()),
        http3_acceptor_datagram_endpoint::notification{this, &datagram_ready}, *quic_pool_);
    udp_->prepare();
    const asio::ip::udp::endpoint endpoint(tcp.address(), udp_->bound_port());
    quic_running_ = true;
    try {
        asio::co_spawn(io_context_, ruvia::asAwaitable(run_quic()),
            asio::bind_allocator(asio::recycling_allocator<void>(),
                [this](std::exception_ptr error) noexcept {
                    quic_running_ = false;
                    if (error) {
                        fail(std::move(error));
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
    for (std::size_t worker = 0; worker < targets_.size(); ++worker) {
        auto channel = makePmrObject<http3_datagram_channel>(processResource(),
            *quic_pool_, quic_notification_, processResource(),
            capacity.input_slots, capacity.output_credits);
        // A staging failure must not leave an unowned channel waiting for an ACK.
        try {
            targets_[worker].stage_quic(targets_[worker].object, *channel, endpoint,
                {.index = static_cast<std::uint32_t>(worker),
                    .count = static_cast<std::uint32_t>(targets_.size())});
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

Task<void> acceptor::run_quic() {
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
            const auto completed = co_await ruvia::asyncAsio([this](auto handler) {
                quic_retirement_.async_wait(std::move(handler));
            });
            if (completed.errorCode()) {
                throw std::system_error(completed.errorCode(), "QUIC acceptor retirement wait");
            }
        } else {
            try {
                if (co_await quic_notification_.wait() == WorkerNotificationWaitStatus::kClosed) {
                    stop_on_owner();
                }
            } catch (...) {
                fail(std::current_exception());
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
            fail(std::current_exception());
        }
        stop_on_owner();
    }
    // Every wake scans all owners. Each credit lane uses one bounded two-wrap
    // batch and republishes the aggregate latch when more credits remain.
    for (auto& channel : quic_channels_) {
        channel->acceptor_poll();
    }
    if (quic_stopping_) {
        for (auto& channel : quic_channels_) {
            // Queued descriptors retire here; a descriptor already submitted to
            // native UDP remains inside the endpoint until its callback.
            for (std::size_t count = 0;
                count < http3_datagram_channel::default_output_window; ++count) {
                auto packet = channel->acceptor_take_output();
                if (!packet) {
                    break;
                }
            }
        }
        return;
    }
    if (auto packet = udp_->take_receive()) {
        const auto worker = ruvia::quic_datagram_partition(packet->view().bytes,
            static_cast<std::uint32_t>(targets_.size()));
        if (worker) {
            (void)quic_channels_[*worker]->acceptor_push(std::move(*packet));
        }
    }
    // A route/drop above or an independent RX credit return can make storage
    // available. No second receive pool and no packet copy exist at this edge.
    udp_->poll_receive();
    if (udp_->outbound_pending()) {
        return;
    }
    for (std::size_t offset = 0; offset < quic_channels_.size(); ++offset) {
        const auto worker = (next_output_ + offset) % quic_channels_.size();
        auto packet = quic_channels_[worker]->acceptor_take_output();
        if (!packet) {
            continue;
        }
        (void)udp_->send_owned_datagram(std::move(*packet));
        next_output_ = (worker + 1) % quic_channels_.size();
        break;
    }
}

void acceptor::prepare() {
    if (prepared_ || runtime_.state() != RuntimeLifecycle::State::kReady) {
        throw std::logic_error("acceptor can only be prepared once before launch");
    }
    for (auto& configured : listeners_) {
        auto& socket = configured->socket;
        asio::error_code error;
        socket.open(configured->endpoint.protocol(), error);
        if (!error) {
            socket.set_option(asio::socket_base::reuse_address(true), error);
        }
        if (!error) {
            socket.bind(configured->endpoint, error);
        }
        if (!error) {
            socket.listen(asio::socket_base::max_listen_connections, error);
        }
        if (!error) {
            configured->endpoint = socket.local_endpoint(error);
        }
        if (error) {
            close_listeners();
            throw std::system_error(error, "failed to prepare TCP acceptor listener");
        }
    }
    prepared_ = true;
}

void acceptor::launch() {
    if (!prepared_) {
        throw std::logic_error("acceptor must be prepared before launch");
    }
    try {
        runtime_.start();
    } catch (...) {
        if (const auto failure = runtime_.failure()) {
            (void)completion_.record_failure(failure);
        } else if (runtime_.state() != RuntimeLifecycle::State::kRunning) {
            completion_.mark_startup_aborted();
            completion_.mark_serving_aborted();
        }
        throw;
    }
}

void acceptor::wait_until_ready() {
    completion_.wait_for_startup();
}

void acceptor::request_serve() {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning || serve_requested_.exchange(true)) {
        return;
    }
    const auto posted = runtime_.post_control([this] {
        if (runtime_.state() != RuntimeLifecycle::State::kRunning) {
            completion_.mark_serving_aborted();
            return;
        }
        try {
            if (udp_ && udp_->start() == http3_acceptor_datagram_endpoint::pump_result::error) {
                throw std::system_error(udp_->error(), "start QUIC acceptor UDP listener");
            }
            for (std::size_t i = 0; i < listeners_.size(); ++i) {
                begin_accept(i);
            }
            if (runtime_.state() == RuntimeLifecycle::State::kRunning && completion_.failure() == nullptr) {
                (void)completion_.mark_serving();
            } else {
                completion_.mark_serving_aborted();
            }
        } catch (...) {
            fail(std::current_exception());
        }
    });
    if (!posted) {
        stop();
    }
}

bool acceptor::wait_until_serving() {
    return completion_.wait_for_serving();
}

void acceptor::stop() noexcept {
    runtime_.request_stop();
    completion_.mark_startup_aborted();
    completion_.mark_serving_aborted();
}

void acceptor::stop_on_owner() noexcept {
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
    completion_.mark_startup_aborted();
    completion_.mark_serving_aborted();
}

void acceptor::join() {
    runtime_.join();
    completion_.mark_startup_aborted();
    completion_.mark_serving_aborted();
}

asio::ip::tcp::endpoint acceptor::local_endpoint(std::size_t index) const {
    return listeners_.at(index)->endpoint;
}

std::exception_ptr acceptor::failure() const noexcept {
    return completion_.failure();
}

void acceptor::rethrow_failure() const {
    if (const auto error = failure()) {
        std::rethrow_exception(error);
    }
    runtime_.rethrow_failure();
}

void acceptor::begin_accept(std::size_t index) noexcept {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning || targets_.empty()) {
        return;
    }
    auto& socket = listeners_[index]->socket;
    if (!socket.is_open()) {
        return;
    }
    try {
        socket.async_accept([this, index](const asio::error_code& error, asio::ip::tcp::socket accepted_socket) mutable {
            accepted(index, error, std::move(accepted_socket));
        });
    } catch (...) {
        fail(std::current_exception());
    }
}

void acceptor::accepted(std::size_t index, const asio::error_code& error,
    asio::ip::tcp::socket socket) noexcept {
    if (error) {
        if (error == asio::error::operation_aborted && runtime_.state() != RuntimeLifecycle::State::kRunning) {
            return;
        }
        if (!recoverable_accept_error(error)) {
            try {
                throw std::system_error(error, "fatal TCP accept error");
            } catch (...) {
                fail(std::current_exception());
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
            fail(std::current_exception());
        }
        return;
    }
    NativeAcceptedSocketTicket ticket(listeners_[index]->endpoint.protocol(), index, native);
    if (runtime_.state() != RuntimeLifecycle::State::kRunning || targets_.empty()) {
        return;
    }
    auto selected = targets_.size();
    for (std::size_t offset = 0; offset < targets_.size(); ++offset) {
        const auto worker = (next_target_ + offset) % targets_.size();
        if (targets_[worker].available(targets_[worker].object) && targets_[worker].submission.accepting()) {
            selected = worker;
            next_target_ = (worker + 1) % targets_.size();
            break;
        }
    }
    if (selected == targets_.size()) {
        schedule_retry(index);
        return;
    }
    const auto target = targets_[selected];
    try {
        auto posted = target.submission.post([target, ticket = std::move(ticket)]() mutable noexcept {
            if (target.available(target.object)) {
                target.accept(target.object, std::move(ticket));
            }
        });
        if (!posted.accepted()) {
            auto rejected = std::move(posted).takeRejected();
            rejected = {};
        }
    } catch (...) {
        fail(std::current_exception());
        return;
    }
    if (runtime_.state() == RuntimeLifecycle::State::kRunning) {
        begin_accept(index);
    }
}

void acceptor::schedule_retry(std::size_t index) noexcept {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning) {
        return;
    }
    try {
        auto& timer = listeners_[index]->retry;
        timer.expires_after(std::chrono::milliseconds(25));
        timer.async_wait([this, index](const asio::error_code& error) {
            if (!error) {
                begin_accept(index);
            }
        });
    } catch (...) {
        fail(std::current_exception());
    }
}

void acceptor::fail(std::exception_ptr error) noexcept {
    (void)completion_.record_failure(std::move(error));
    runtime_.request_stop();
    if (failure_callback_ != nullptr) {
        failure_callback_(failure_target_);
    }
}

void acceptor::close_listeners() noexcept {
    for (auto& configured : listeners_) {
        asio::error_code ignored;
        configured->retry.cancel();
        ignored.clear();
        configured->socket.cancel(ignored);
        ignored.clear();
        configured->socket.close(ignored);
    }
}

}  // namespace ruvia::detail
