#include "http3/http3_worker.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/timer.h"

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

// Linux steers by connection id partition; elsewhere every worker accepts
// every connection id because the kernel distributes by other means.
std::uint32_t steering_partitions(const http3_worker::listener_binding& binding) noexcept {
#if defined(__linux__)
    return binding.worker_count_;
#else
    (void)binding;
    return 1;
#endif
}

// Probe layout: short-header fixed bit, a 16-byte connection id whose first
// four bytes carry the partition word, the process secret, and the worker
// index again; zero padded. It is never a valid QUIC packet for any peer.
constexpr std::size_t probe_bytes = 64;
constexpr std::size_t probe_word_offset = 1;
constexpr std::size_t probe_secret_offset = 17;
constexpr std::size_t probe_index_offset = probe_secret_offset + http3_worker::probe_secret_bytes;
constexpr auto probe_timeout = std::chrono::seconds(2);
constexpr auto probe_poll_interval = std::chrono::milliseconds(1);

void write_word(std::span<std::byte> output, std::uint32_t word) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        output[i] = static_cast<std::byte>((word >> (8 * (3 - i))) & 0xffU);
    }
}

std::uint32_t read_word(std::span<const std::byte> input) noexcept {
    std::uint32_t word{};
    for (std::size_t i = 0; i < 4; ++i) {
        word = (word << 8) | std::to_integer<std::uint8_t>(input[i]);
    }
    return word;
}

void write_probe(std::span<std::byte> output, const http3_worker::listener_binding& binding) noexcept {
    std::ranges::fill(output, std::byte{});
    output[0] = std::byte{0x40};
    write_word(output.subspan(probe_word_offset, 4), binding.worker_index_);
    std::ranges::copy(binding.probe_secret_, output.subspan(probe_secret_offset).begin());
    write_word(output.subspan(probe_index_offset, 4), binding.worker_index_);
}

// The partition word of an authenticated probe of this process.
std::optional<std::uint32_t> read_probe(std::span<const std::byte> input,
    const http3_worker::listener_binding& binding) noexcept {
    if (input.size() != probe_bytes || input[0] != std::byte{0x40} ||
        !std::ranges::equal(input.subspan(probe_secret_offset, http3_worker::probe_secret_bytes), binding.probe_secret_)) {
        return std::nullopt;
    }
    const auto word = read_word(input.subspan(probe_word_offset, 4));
    if (read_word(input.subspan(probe_index_offset, 4)) != word || word >= binding.worker_count_) {
        return std::nullopt;
    }
    return word;
}

// Probes travel through the kernel to the listen endpoint; a wildcard bind is
// reached through its loopback address.
asio::ip::udp::endpoint probe_destination(const http3_worker::listener_binding& binding, std::uint16_t port) {
    const auto& address = binding.endpoint_.address();
    if (!address.is_unspecified()) {
        return asio::ip::udp::endpoint(address, port);
    }
    return asio::ip::udp::endpoint(
        address.is_v6() ? asio::ip::address(asio::ip::address_v6::loopback()) : asio::ip::address(asio::ip::address_v4::loopback()), port);
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
      tasks_(runtime.handle(), {.resource_ = memory.resource()}) {
    if (config_.drain_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("HTTP/3 drain timeout must be greater than zero");
    }
}

http3_worker::~http3_worker() {
    if (wire_ || endpoint_ || tasks_.size() != 0 || joins_.size() != 0) {
        std::terminate();
    }
}

void http3_worker::stage(listener_binding binding) {
    if (binding_) {
        throw std::logic_error("HTTP/3 worker already has a staged listener");
    }
    if (binding.worker_count_ == 0 || binding.worker_index_ >= binding.worker_count_) {
        throw std::invalid_argument("HTTP/3 worker listener binding requires index < count");
    }
    binding_ = std::move(binding);
}

void http3_worker::start() {
    if (!runtime_.handle().is_current() || started_) {
        throw std::logic_error("HTTP/3 component must start once on its owner");
    }
    if (!binding_) {
        throw std::logic_error("HTTP/3 worker has no staged listener");
    }
    started_ = true;
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
    const auto steering = steering_partitions(*binding_);
    // The Linux index is provisional until serve calibrates it.
    partition_ = steering > 1 ? quic_cid_partition{.index_ = binding_->worker_index_, .count_ = steering}
                              : quic_cid_partition{.index_ = 0, .count_ = 1};
    endpoint_.emplace(runtime_.io_context(), binding_->endpoint_, steering,
        http3_worker_datagram_endpoint::notification{this, &endpoint_notification}, memory_.resource());
    // Binds this worker's socket; a failure is this worker's startup failure.
    endpoint_->prepare();
}

void http3_worker::serve() {
    if (!runtime_.handle().is_current() || !started_ || serving_) {
        throw std::logic_error("HTTP/3 component must serve once after start on its owner");
    }
    serving_ = true;
    if (stopping_) {
        return;
    }
    if (endpoint_->start() == http3_worker_datagram_endpoint::pump_result::error) {
        throw std::system_error(endpoint_->error(), "start HTTP/3 worker UDP listener");
    }
    tasks_.spawn(run());
    wake();
}

void http3_worker::drain() noexcept {
    if (draining_ || stopping_ || !serving_) {
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
    if (!started_) {
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
    if (endpoint_) {
        // The protocol loop never ran (stopped before serve, or serve failed).
        begin_stop();
        co_await retire_endpoint();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void http3_worker::endpoint_notification(void* context_value,
    http3_worker_datagram_endpoint::notification_kind kind) noexcept {
    auto& self = *static_cast<http3_worker*>(context_value);
    if (kind == http3_worker_datagram_endpoint::notification_kind::stopping && self.wire_) {
        self.wire_->on_endpoint_stopping();
    }
    // Native completions are edge events: one wake per received datagram or
    // completed send keeps the loop from spinning on level state.
    self.wake();
}

task<void> http3_worker::run() {
    try {
        co_await calibrate_partition();
        if (!stopping_) {
            wire_.emplace(runtime_.io_context(), *endpoint_, *tls_,
                transport_config(options_, partition_), memory_.resource(),
                http3_quic_wire_owner::wake_type{this, [](void* context) noexcept {
                                                     static_cast<http3_worker*>(context)->wake();
                                                 }});
            wire_->start();
            if (stop_token_.stop_requested()) {
                begin_stop();
            }
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    if (!wire_) {
        co_await retire_endpoint();
        co_return;
    }
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
    try {
        co_await joins_.join();
    } catch (...) {
        report_failure(std::current_exception());
    }
    wire_.reset();
    endpoint_.reset();
    tls_.reset();
}

// Every worker sends one datagram whose partition word is its worker index;
// steering delivers it to the group socket of that index, whose worker learns
// the partition it must generate connection ids in. Datagrams that are not
// probes arrive before serving and are dropped; QUIC peers retransmit.
task<void> http3_worker::calibrate_partition() {
    if (partition_.count_ <= 1 || stopping_) {
        co_return;
    }
    const auto probe_buffer = endpoint_->packet_buffer();
    if (probe_buffer.size() < probe_bytes) {
        throw std::runtime_error("HTTP/3 worker cannot reserve its partition probe");
    }
    write_probe(probe_buffer.first(probe_bytes), *binding_);
    const auto destination = probe_destination(*binding_, endpoint_->bound_port());
    if (endpoint_->send_datagram(std::span<const std::byte>(probe_buffer).first(probe_bytes),
            destination, destination) != http3_worker_datagram_endpoint::pump_result::pending) {
        throw std::system_error(endpoint_->error(), "send HTTP/3 worker partition probe");
    }
    const auto deadline = clock_type::now() + probe_timeout;
    std::optional<std::uint32_t> learned;
    while (!stopping_) {
        while (const auto received = endpoint_->receive_slot()) {
            if (const auto word = read_probe(received->bytes_, *binding_)) {
                const auto index = *word % partition_.count_;
                if (learned && *learned != index) {
                    throw std::runtime_error("HTTP/3 reuseport steering delivered two partitions to one worker");
                }
                learned = index;
            }
            if (endpoint_->consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
                throw std::system_error(endpoint_->error(), "consume HTTP/3 worker partition probe");
            }
        }
        if (learned) {
            partition_.index_ = *learned;
            co_return;
        }
        if (clock_type::now() >= deadline) {
            throw std::runtime_error("HTTP/3 reuseport steering delivered no partition probe to this worker");
        }
        if (co_await ruvia::sleep_for(runtime_.handle(), probe_poll_interval) !=
            ruvia::timer_sleep_result::elapsed) {
            co_return;
        }
    }
}

task<void> http3_worker::retire_endpoint() {
    if (!endpoint_) {
        co_return;
    }
    endpoint_->request_stop();
    while (!endpoint_->endpoint_retired()) {
        // Cancelled native operations complete on this loop.
        if (co_await ruvia::sleep_for(runtime_.handle(), probe_poll_interval) !=
            ruvia::timer_sleep_result::elapsed) {
            co_await yield();
        }
    }
    endpoint_.reset();
    tls_.reset();
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
        wire_->poll_endpoint();
        if (const auto failure = wire_->failure()) {
            report_failure(failure);
        }
        if (wire_->stopping()) {
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
        // More packets may remain; resume once a native send completes.
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
