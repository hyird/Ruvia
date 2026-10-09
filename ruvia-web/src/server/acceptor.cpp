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

#include "ruvia/core/async.h"
#include "ruvia/core/memory/process_resource.h"

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

acceptor::acceptor(std::span<const http_server_listener_definition> listeners,
    std::span<const worker_target> targets, void* failure_target, failure_callback failure)
    : runtime_({.queue_capacity_ = 128, .io_policy_ = worker_io_policy::single_owner}),
      io_context_(runtime_.context().io_context()),
      listeners_(process_resource()),
      targets_(targets.begin(), targets.end(), process_resource()),
      quic_channels_(process_resource()),
      udp_(nullptr, pmr_object_deleter<http3_acceptor_datagram_endpoint>{process_resource()}),
      quic_notification_(runtime_.context()),
      quic_retirement_(io_context_),
      failure_target_(failure_target),
      failure_callback_(failure) {
    if (listeners.empty()) {
        throw std::invalid_argument("acceptor requires at least one TCP listener");
    }
    const auto quic_count = std::ranges::count_if(listeners,
        [](const auto& configured) { return configured.http3_.has_value(); });
    if (quic_count > 1) {
        throw std::invalid_argument("acceptor supports one QUIC listener");
    }
    if (quic_count != 0 && (targets.empty() ||
                               targets.size() > std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("QUIC acceptor requires representable worker targets");
    }
    for (const auto& target : targets_) {
        if (!target.submission_.valid() || target.object_ == nullptr || target.available_ == nullptr ||
            target.accept_ == nullptr || (quic_count != 0 && target.stage_quic_ == nullptr)) {
            throw std::invalid_argument("acceptor worker target is incomplete");
        }
    }
    listeners_.reserve(listeners.size());
    quic_channels_.reserve(quic_count != 0 ? targets.size() : 0);
    for (const auto& configured : listeners) {
        listeners_.push_back(make_pmr_object<listener>(process_resource(), io_context_,
            configured.endpoint_, configured.http3_ ? std::optional(normalize_http3_capacity(*configured.http3_, targets.size())) : std::nullopt));
    }
    runtime_.configure({
        .startup_ = [this] {
            prepare_quic();
            if (runtime_.state() == runtime_lifecycle::state_type::running) {
                (void)completion_.mark_startup_ready();
            } else {
                completion_.mark_startup_aborted();
            } },
        .stop_admission_ = [this] { stop_on_owner(); },
        .failure_ = [this](std::exception_ptr error) noexcept { fail(std::move(error)); },
        .shutdown_ = [this]() noexcept {
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
        [](const auto& bound) { return bound->quic_.has_value(); });
    if (configured == listeners_.end()) {
        return;
    }
    const auto& tcp = (*configured)->endpoint_;
    const auto capacity = *(*configured)->quic_;
    quic_pool_.emplace(capacity.packet_slots_,
        http3_capacity::packet_bytes, process_resource());
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
    for (std::size_t worker_value = 0; worker_value < targets_.size(); ++worker_value) {
        auto channel = make_pmr_object<http3_datagram_channel>(process_resource(),
            *quic_pool_, quic_notification_, process_resource(),
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
        const auto worker_value = ruvia::quic_datagram_partition(packet->view().bytes_,
            static_cast<std::uint32_t>(targets_.size()));
        if (worker_value) {
            (void)quic_channels_[*worker_value]->acceptor_push(std::move(*packet));
        }
    }
    // A route/drop above or an independent RX credit return can make storage
    // available. No second receive pool and no packet copy exist at this edge.
    udp_->poll_receive();
    if (udp_->outbound_pending()) {
        return;
    }
    for (std::size_t offset = 0; offset < quic_channels_.size(); ++offset) {
        const auto worker_value = (next_output_ + offset) % quic_channels_.size();
        auto packet = quic_channels_[worker_value]->acceptor_take_output();
        if (!packet) {
            continue;
        }
        (void)udp_->send_owned_datagram(std::move(*packet));
        next_output_ = (worker_value + 1) % quic_channels_.size();
        break;
    }
}

void acceptor::prepare() {
    if (prepared_ || runtime_.state() != runtime_lifecycle::state_type::ready) {
        throw std::logic_error("acceptor can only be prepared once before launch");
    }
    for (auto& configured : listeners_) {
        auto& socket = configured->socket_;
        asio::error_code error;
        socket.open(configured->endpoint_.protocol(), error);
        if (!error) {
            socket.set_option(asio::socket_base::reuse_address(true), error);
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
        } else if (runtime_.state() != runtime_lifecycle::state_type::running) {
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
    if (runtime_.state() != runtime_lifecycle::state_type::running || serve_requested_.exchange(true)) {
        return;
    }
    const auto posted = runtime_.post_control([this] {
        if (runtime_.state() != runtime_lifecycle::state_type::running) {
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
            if (runtime_.state() == runtime_lifecycle::state_type::running && completion_.failure() == nullptr) {
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
    return listeners_.at(index)->endpoint_;
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
    if (runtime_.state() != runtime_lifecycle::state_type::running || targets_.empty()) {
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
        fail(std::current_exception());
    }
}

void acceptor::accepted(std::size_t index, const asio::error_code& error,
    asio::ip::tcp::socket socket) noexcept {
    if (error) {
        if (error == asio::error::operation_aborted && runtime_.state() != runtime_lifecycle::state_type::running) {
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
    native_accepted_socket_ticket ticket(listeners_[index]->endpoint_.protocol(), index, native);
    if (runtime_.state() != runtime_lifecycle::state_type::running || targets_.empty()) {
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
        schedule_retry(index);
        return;
    }
    const auto target = targets_[selected];
    try {
        auto posted = target.submission_.post([target, ticket = std::move(ticket)]() mutable noexcept {
            if (target.available_(target.object_)) {
                target.accept_(target.object_, std::move(ticket));
            }
        });
        if (!posted.accepted()) {
            auto rejected = std::move(posted).take_rejected();
            rejected = {};
        }
    } catch (...) {
        fail(std::current_exception());
        return;
    }
    if (runtime_.state() == runtime_lifecycle::state_type::running) {
        begin_accept(index);
    }
}

void acceptor::schedule_retry(std::size_t index) noexcept {
    if (runtime_.state() != runtime_lifecycle::state_type::running) {
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
        configured->retry_.cancel();
        ignored.clear();
        configured->socket_.cancel(ignored);
        ignored.clear();
        configured->socket_.close(ignored);
    }
}

}  // namespace ruvia::detail
