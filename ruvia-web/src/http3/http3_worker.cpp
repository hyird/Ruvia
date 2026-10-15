#include "http3/http3_worker.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/async.h"

#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {
namespace {

// Bounded work per protocol turn; a turn with work left yields to the loop.
constexpr std::size_t datagram_batch = 64;
constexpr std::size_t connection_batch = 64;
// Request stream reads land here once and are fed straight to the HTTP layer.
constexpr std::size_t scratch_bytes = 16 * 1024;

ruvia::quic_server_config transport_config(const http_server_options& options,
    ruvia::quic_cid_partition partition) {
    ruvia::quic_server_config config;
    config.max_active_connections_ = options.server_.max_connections_per_worker_.value();
    config.max_pending_connections_ = config.max_active_connections_;
    config.cid_partition_ = partition;
    config.limits_.max_lifetime_peer_streams_ =
        http3_transport_lifetime_stream_capacity(options.server_.max_requests_per_connection_.value());
    config.local_transport_parameters_.idle_timeout_ms_ =
        options.server_.idle_timeout_ ? static_cast<std::uint64_t>(options.server_.idle_timeout_->count()) : 0;
    config.local_transport_parameters_.max_datagram_frame_size_ = 65536;
    // New peer paths remain on this worker through its CID partition.
    config.local_transport_parameters_.disable_active_migration_ = false;
    return config;
}

}  // namespace

http3_worker::http3_worker(worker_runtime_context& runtime, worker_memory& memory,
    const route_table& routes_value, worker_capabilities& capabilities,
    connection_scanner& scanner, const http_server_options& options,
    const stop_token& stop_token_value, const http_server_listener_definition::tls_type& tls,
    http3_listen_config config, connection_drain& active_connections,
    std::atomic<std::size_t>& refused_connections, failure_notification failure)
    : runtime_(runtime),
      memory_(memory),
      routes_(routes_value),
      capabilities_(capabilities),
      scanner_(scanner),
      options_(options),
      stop_token_(stop_token_value),
      tls_config_(tls),
      config_(config),
      active_connections_(active_connections),
      refused_connections_(refused_connections),
      failure_notification_(failure),
      signal_(runtime.handle()),
      body_budget_(body_budget_bytes),
      drivers_(memory.resource()),
      connections_(memory.resource()),
      ready_(memory.resource()),
      send_blocked_(memory.resource()),
      offers_(memory.resource()),
      scratch_(memory.resource()),
      joins_(runtime.handle(), {.resource_ = memory.resource()}),
      notifications_(runtime.handle(), {.resource_ = memory.resource()}),
      tasks_(runtime.handle(), {.resource_ = memory.resource()}) {
    if (config_.drain_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("HTTP/3 drain timeout must be greater than zero");
    }
}

http3_worker::~http3_worker() {
    if (channel_ != nullptr || staged_channel_.load(std::memory_order_acquire) != nullptr ||
        tasks_.size() != 0 || joins_.size() != 0 || notifications_.size() != 0) {
        std::terminate();
    }
}

void http3_worker::stage(http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, quic_cid_partition partition) {
    if (staged_channel_.load(std::memory_order_acquire) != nullptr) {
        throw std::logic_error("HTTP/3 worker already has a staged channel");
    }
    channel.stage_worker(runtime_);
    endpoint_ = std::move(endpoint);
    partition_ = partition;
    staged_channel_.store(&channel, std::memory_order_release);
    // The second check closes the stop-before-publication race.
    if (abandoned_.load(std::memory_order_acquire)) {
        abandon_before_launch();
    }
}

void http3_worker::abandon_before_launch() noexcept {
    abandoned_.store(true, std::memory_order_release);
    if (auto* channel = staged_channel_.exchange(nullptr, std::memory_order_acq_rel)) {
        channel->abandon_worker();
    }
}

void http3_worker::start() {
    if (!runtime_.handle().is_current() || started_) {
        throw std::logic_error("HTTP/3 component must start once on its owner");
    }
    started_ = true;
    struct cold_channel_owner final {
        http3_datagram_channel* channel_;
        ~cold_channel_owner() {
            if (channel_) {
                channel_->abandon_worker();
            }
        }
    } cold{staged_channel_.exchange(nullptr, std::memory_order_acq_rel)};
    if (!cold.channel_) {
        throw std::logic_error("HTTP/3 worker has no staged acceptor channel");
    }
    const auto max_connections = options_.server_.max_connections_per_worker_.value();
    const http3_connection_driver_config driver_config{
        .max_requests_per_connection_ = options_.server_.max_requests_per_connection_.value(),
        .request_header_timeout_ = options_.server_.request_header_timeout_,
        .request_body_timeout_ = options_.server_.request_body_timeout_,
        .write_timeout_ = options_.server_.write_timeout_,
        .drain_timeout_ = config_.drain_timeout_,
        .handshake_timeout_ = config_.handshake_timeout_,
        .local_settings_ = {.qpack_max_table_capacity_ = config_.qpack_.max_table_capacity_,
            .qpack_blocked_streams_ = config_.qpack_.max_blocked_streams_,
            .enable_connect_protocol_ = true,
            .h3_datagram_ = true}};
    // Everything that can fail cold is built before the wire borrows the channel.
    drivers_.reserve(max_connections);
    for (std::size_t index = 0; index < max_connections; ++index) {
        drivers_.push_back(make_pmr_object<http3_connection_driver>(memory_.resource(), *this, driver_config));
    }
    connections_.reserve(max_connections);
    // Queued entries (one per driver) plus the entries served in one turn.
    ready_.reserve(max_connections + connection_batch);
    send_blocked_.reserve(max_connections);
    offers_.reserve(max_connections);
    scratch_.resize(scratch_bytes);
    tls_.emplace(tls_config_, memory_.resource());
    wire_.emplace(runtime_.io_context(), *cold.channel_, endpoint_, *tls_,
        transport_config(options_, partition_), memory_.resource(),
        http3_quic_wire_owner::wake_type{this, [](void* context) noexcept {
                                             static_cast<http3_worker*>(context)->wake();
                                         }});
    cold.channel_->worker_start();
    channel_ = std::exchange(cold.channel_, nullptr);
    // From here the channel is borrowed: every exit runs the ordered stop that
    // waits for the Acceptor's close before the final ACK.
    wire_->start();
    if (stop_token_.stop_requested()) {
        begin_stop();
    }
    // A spawn failure leaves the stop and retirement to join().
    notifications_.spawn(run_notifications());
    tasks_.spawn(run());
    wake();
}

void http3_worker::drain() noexcept {
    if (draining_ || stopping_ || channel_ == nullptr) {
        return;
    }
    draining_ = true;
    offers_.clear();
    for (auto& driver : drivers_) {
        if (!driver->vacant()) {
            driver->request_drain();
            mark_ready(*driver);
        }
    }
    wake();
}

void http3_worker::stop() noexcept {
    if (!runtime_.handle().is_current()) {
        std::terminate();
    }
    if (channel_ == nullptr) {
        abandon_before_launch();
        return;
    }
    begin_stop();
}

task<void> http3_worker::join() {
    std::exception_ptr failure;
    try {
        co_await tasks_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    if (channel_ != nullptr) {
        // The protocol loop never ran (start failed after the channel borrow).
        begin_stop();
        co_await run();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

task<void> http3_worker::run() {
    while (!wire_->stopped()) {
        const bool more = turn();
        if (wire_->stopped()) {
            break;
        }
        try {
            if (more) {
                co_await yield();
            } else {
                co_await signal_.wait();
            }
        } catch (...) {
            report_failure(std::current_exception());
        }
    }
    native_stopping_ = true;
    // Every native wait joins before the final ACK lets the Acceptor reclaim
    // the channel and its notification.
    channel_->worker_notification().close();
    try {
        co_await notifications_.join();
    } catch (...) {
        report_failure(std::current_exception());
    }
    try {
        co_await joins_.join();
    } catch (...) {
        report_failure(std::current_exception());
    }
    wire_.reset();
    tls_.reset();
    finish_channel();
}

task<void> http3_worker::run_notifications() {
    // The worker's only native datagram wait; local work uses signal_.
    try {
        while (!native_stopping_) {
            const auto status = co_await channel_->worker_notification().wait();
            if (native_stopping_) {
                break;
            }
            if (status == worker_notification_wait_status::closed) {
                begin_stop();
                break;
            }
            wake();
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    wake();
}

task<void> http3_worker::yield() {
    const auto yielded = co_await ruvia::async_asio([this](auto completion) {
        asio::post(runtime_.io_context().get_executor(),
            asio::bind_allocator(std::pmr::polymorphic_allocator<std::byte>(memory_.resource()),
                [completion = std::move(completion)]() mutable {
                    completion(asio::error_code{});
                }));
    });
    if (yielded.error_code()) {
        throw std::system_error(yielded.error_code(), "yield HTTP/3 protocol worker");
    }
}

bool http3_worker::turn() {
    bool more = false;
    try {
        wire_->poll_channel();
        if (const auto failure = wire_->failure()) {
            report_failure(failure);
        }
        if (channel_->acceptor_closed() || wire_->stopping()) {
            begin_stop();
        }
        const auto now = clock_type::now();
        if (!stopping_) {
            receive(now);
            admit(now);
        }
        if (wire_->take_expiry()) {
            expire(now);
        }
        if (!send_blocked_.empty() && wire_->output_available()) {
            for (auto* driver : send_blocked_) {
                driver->send_blocked_ = false;
                mark_ready(*driver);
            }
            send_blocked_.clear();
        }
        serve_ready(now);
        more = ready_head_ != ready_.size() || wire_->receive_pending();
        if (stopping_) {
            if (connections_retired() && joins_.size() == 0) {
                wire_->request_stop();
            }
            wire_->poll_stop();
        } else {
            wire_->schedule(local_deadline_);
        }
    } catch (...) {
        report_failure(std::current_exception());
        more = true;
    }
    return more;
}

void http3_worker::receive(clock_type::time_point now) {
    for (std::size_t count = 0; count < datagram_batch; ++count) {
        const auto received = wire_->receive_one(now);
        using kind_type = http3_quic_wire_owner::received_type::kind_type;
        if (received.kind_ == kind_type::none) {
            return;
        }
        if (received.kind_ == kind_type::initial_offer) {
            if (!draining_ && offers_.size() < offers_.capacity() &&
                std::ranges::none_of(offers_, [&received](const ruvia::quic_initial_offer& pending) {
                    return pending.offer_id_ == received.offer_.offer_id_;
                })) {
                offers_.push_back(received.offer_);
            }
            continue;
        }
        if (received.kind_ != kind_type::connection && received.kind_ != kind_type::failed_connection) {
            continue;
        }
        auto* driver = find(received.connection_);
        if (driver == nullptr) {
            continue;
        }
        if (received.kind_ == kind_type::connection) {
            driver->transport_activity();
        } else {
            driver->transport_failure();
        }
        mark_ready(*driver);
    }
}

void http3_worker::admit(clock_type::time_point now) {
    if (draining_) {
        return;
    }
    while (!offers_.empty()) {
        const auto vacant = std::ranges::find_if(drivers_, [](const driver_owner& driver) {
            return driver->vacant();
        });
        if (vacant == drivers_.end()) {
            return;  // Offers wait for a retired slot; QUIC bounds pending Initials.
        }
        const auto status = (*vacant)->admit(offers_.front(), now);
        if (status == ruvia::quic_operation_status::would_block) {
            return;
        }
        offers_.erase(offers_.begin());
        if (status == ruvia::quic_operation_status::accepted) {
            mark_ready(**vacant);
        }
    }
}

void http3_worker::expire(clock_type::time_point now) {
    try {
        (void)wire_->transport().server().handle_expiry(now);
    } catch (const ruvia::quic_error&) {
        // A failed connection is retired by its driver after the next turn.
    }
    // QUIC retransmission/idle expiry and local deadlines both need a driver
    // turn; scanning here happens once per timer expiry, never per datagram.
    local_deadline_.reset();
    for (auto& driver : drivers_) {
        if (!driver->vacant()) {
            mark_ready(*driver);
        }
    }
}

void http3_worker::serve_ready(clock_type::time_point now) {
    for (std::size_t count = 0; count < connection_batch && ready_head_ != ready_.size(); ++count) {
        auto& driver = *ready_[ready_head_++];
        driver.queued_ = false;
        const bool again = driver.pump(scratch_, now);
        flush(driver, now);
        if (const auto deadline = driver.deadline(); deadline && (!local_deadline_ || *deadline < *local_deadline_)) {
            local_deadline_ = deadline;
        }
        if (again) {
            mark_ready(driver);
        }
    }
    if (ready_head_ == ready_.size()) {
        ready_.clear();
        ready_head_ = 0;
    } else if (ready_head_ != 0) {
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(ready_head_));
        ready_head_ = 0;
    }
}

void http3_worker::flush(http3_connection_driver& driver, clock_type::time_point now) {
    const auto token = driver.token();
    if (!token || !wire_->transport_available()) {
        return;
    }
    if (!wire_->send(*token, now)) {
        driver.transport_failure();
        mark_ready(driver);
        return;
    }
    if (!wire_->output_available() && !driver.send_blocked_) {
        // More packets may remain; resume once the Acceptor returns output credit.
        driver.send_blocked_ = true;
        send_blocked_.push_back(&driver);
    }
}

void http3_worker::begin_stop() noexcept {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    offers_.clear();
    for (auto& driver : drivers_) {
        driver->request_stop();
        if (!driver->vacant()) {
            mark_ready(*driver);
        }
    }
    wake();
}

bool http3_worker::connections_retired() const noexcept {
    return std::ranges::all_of(drivers_, [](const driver_owner& driver) { return driver->vacant(); });
}

void http3_worker::report_failure(std::exception_ptr failure) noexcept {
    if (!failure_) {
        failure_ = failure;
    }
    begin_stop();
    if (failure_reported_) {
        return;
    }
    failure_reported_ = true;
    if (failure_notification_.notify_ != nullptr) {
        failure_notification_.notify_(failure_notification_.context_, failure_);
    }
}

void http3_worker::finish_channel() noexcept {
    // The wire released its endpoint; no channel borrow survives the final ACK.
    auto* channel = std::exchange(channel_, nullptr);
    channel->worker_close();
}

http3_connection_driver* http3_worker::find(ruvia::quic_connection_token token) noexcept {
    const auto found = connections_.find(token.value_);
    return found == connections_.end() ? nullptr : found->second;
}

void http3_worker::mark_ready(http3_connection_driver& driver) noexcept {
    if (driver.queued_) {
        return;
    }
    driver.queued_ = true;
    ready_.push_back(&driver);
    signal_.notify();
}

void http3_worker::map_token(ruvia::quic_connection_token token, http3_connection_driver& driver) {
    if (!connections_.try_emplace(token.value_, &driver).second) {
        throw std::logic_error("QUIC server issued a duplicate connection token");
    }
}

void http3_worker::unmap_token(ruvia::quic_connection_token token) noexcept {
    connections_.erase(token.value_);
}

void http3_worker::spawn_join(http3_connection_driver& driver) {
    joins_.spawn(driver.join_retired());
}

void http3_worker::wake() noexcept {
    signal_.notify();
}

}  // namespace ruvia::detail
