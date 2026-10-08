#include "http3/http3_worker_server.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/Async.h"

#include "context/ContextServices.h"
#include "integration/WorkerCapabilities.h"
#include "router/RouteTable.h"
#include "server/HttpServerOptions.h"
#include "server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {
constexpr std::size_t body_budget_bytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t pump_budget = 256;
constexpr std::size_t input_turn_budget = 64;
}  // namespace

http3_worker_server::http3_worker_server(const WorkerHandle& worker,
    WorkerMemory& memory, const RouteTable& routes, WorkerCapabilities& capabilities,
    ConnectionScanner& connection_scanner, asio::any_io_executor executor,
    const HttpServerOptions& options, const StopToken& stop_token,
    std::size_t max_connections, std::uint32_t buffer_capacity,
    std::atomic<std::size_t>& active_connections,
    std::atomic<std::size_t>& refused_connections)
    : worker_(worker),
      memory_(memory),
      routes_(routes),
      capabilities_(capabilities),
      connection_scanner_(connection_scanner),
      executor_(std::move(executor)),
      options_(options),
      stop_token_(stop_token),
      active_connections_(active_connections),
      refused_connections_(refused_connections),
      signal_(worker),
      response_buffer_(buffer_capacity, buffer_capacity, buffer_capacity, memory.resource()),
      body_budget_(body_budget_bytes),
      retirement_tasks_(worker, {.resource = memory.resource()}),
      slots_(memory.resource()),
      pending_input_(memory.resource()),
      max_connections_(max_connections) {
    slots_.reserve(max_connections_);
    pending_input_.resize(buffer_capacity);
    for (std::size_t i = 0; i < max_connections_; ++i) {
        slots_.emplace_back(memory.resource());
    }
}

http3_worker_server::~http3_worker_server() {
    if (installed_ && !drained_) {
        std::terminate();
    }
}

void http3_worker_server::wake() noexcept {
    signal_.notify();
}

bool http3_worker_server::stage_install(install_link link) noexcept {
    if (staged_ || link.request_buffer == nullptr || link.connections.size() != slots_.size() ||
        link.request_buffer->block_capacity() > pending_input_.size() || link.protocol_ready.notify == nullptr) {
        return false;
    }
    for (auto* state : link.connections) {
        if (state == nullptr) {
            return false;
        }
    }
    request_buffer_ = link.request_buffer;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].state = link.connections[i];
    }
    response_buffer_.set_local_notifications({.ready = link.protocol_ready,
        .capacity = {this, [](void* context, std::uint8_t lanes) noexcept {
                         auto& server = *static_cast<http3_worker_server*>(context);
                         if (server.scheduler_) {
                             server.scheduler_->receive_capacity(lanes);
                         }
                         server.wake();
                     }}});
    staged_ = true;
    return true;
}

bool http3_worker_server::install() noexcept {
    if (!worker_.isCurrent() || !staged_ || installed_ || stopping_ || drained_) {
        return false;
    }
    try {
        scheduler_.emplace(worker_, max_connections_, memory_.resource(),
            http3_ready_scheduler::local_ready_callback{this, [](void* context) noexcept {
                                                            static_cast<http3_worker_server*>(context)->wake();
                                                        }});
    } catch (...) {
        return false;
    }
    installed_ = true;
    wake();
    return true;
}

Task<void> http3_worker_server::run() {
    if (!worker_.isCurrent() || run_started_ || !installed_) {
        std::terminate();
    }
    run_started_ = true;
    while (!drained_) {
        bool exhausted = true;
        for (std::size_t pass = 0; pass < pump_budget; ++pass) {
            if (!pump()) {
                exhausted = false;
                break;
            }
        }
        if (stopping_) {
            finish_stopped_slots();
            const bool live = std::ranges::any_of(slots_, [](const slot& target) {
                return target.connection != nullptr || target.identity.epoch != 0;
            });
            if (!live) {
                (void)request_buffer_->stop();
                (void)response_buffer_.stop();
                co_await retirement_tasks_.join();
                scheduler_.reset();
                drained_ = true;
                co_return;
            }
        }
        if (exhausted) {
            const auto yielded = co_await ruvia::asyncAsio([this](auto completion) {
                asio::post(executor_, asio::bind_allocator(
                                          std::pmr::polymorphic_allocator<std::byte>(memory_.resource()),
                                          [completion = std::move(completion)]() mutable {
                                              completion(asio::error_code{});
                                          }));
            });
            if (yielded.errorCode()) {
                request_stop();
            }
        } else {
            // Capacity, state and input publications are worker-local and latched.
            // Native cross-thread notification is owned only by the datagram runner.
            co_await signal_.wait();
        }
    }
}

void http3_worker_server::request_stop() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto& pending : pending_input_) {
        pending.block.release();
    }
    pending_input_count_ = 0;
    for (auto& target : slots_) {
        if (target.state) {
            target.state->stop_admission();
        }
        if (target.connection) {
            begin_retirement(target);
        }
    }
    if (!installed_) {
        if (request_buffer_) {
            (void)request_buffer_->stop();
        }
        (void)response_buffer_.stop();
        drained_ = !run_started_;
    }
    wake();
}

void http3_worker_server::abandon_before_launch() noexcept {
    if (run_started_ || (installed_ && !worker_.isCurrent())) {
        std::terminate();
    }
    if (drained_) {
        return;
    }
    stopping_ = true;
    for (auto& target : slots_) {
        if (target.state) {
            target.state->stop_admission();
        }
    }
    if (request_buffer_) {
        (void)request_buffer_->stop();
    }
    (void)response_buffer_.stop();
    scheduler_.reset();
    drained_ = true;
}

bool http3_worker_server::pump() noexcept {
    if (!worker_.isCurrent() || !installed_ || !scheduler_) {
        return false;
    }
    bool progress = pump_states();
    progress = pump_input() || progress;
    for (auto& target : slots_) {
        if (!target.connection || target.retirement_started || !target.state) {
            continue;
        }
        http3_connection_state::datagram datagram;
        for (std::size_t pass = 0; pass < 16; ++pass) {
            if (target.state->pop_request_datagram(datagram) != http3_connection_state::status::changed) {
                break;
            }
            target.connection->receiveDatagram(datagram.bytes());
            datagram.storage.reset();
            progress = true;
        }
        progress = target.connection->resumeQpackInput() || progress;
    }
    progress = pump_scheduler() || progress;
    progress = publish_drain_completions() || progress;
    finish_stopped_slots();
    return progress;
}

bool http3_worker_server::pump_states() noexcept {
    bool progress = false;
    for (auto& target : slots_) {
        auto& state = *target.state;
        if (target.identity.epoch != 0 && (state.slot_reusable() || state.identity() != target.identity)) {
            if (target.connection) {
                std::terminate();
            }
            clear_slot(target);
            progress = true;
        }
        if (!stopping_ && state.admission() == http3_connection_state::admission_phase::vacant) {
            if (state.reserve(*scheduler_, epoch_, next_connection_generation_) == http3_connection_state::status::changed) {
                if (++next_connection_generation_ == 0) {
                    std::terminate();
                }
                target.identity = *state.identity();
                target.registration = *state.registration();
                progress = true;
            }
        }
        if (state.admission() == http3_connection_state::admission_phase::bound && !target.connection) {
            const auto binding = state.binding();
            if (stopping_ || !binding || !construct_connection(target, *binding)) {
                (void)state.reject(target.identity, stopping_
                                                        ? http3_connection_state::reject_reason::stopping
                                                        : http3_connection_state::reject_reason::construction_failed);
                refused_connections_.fetch_add(1, std::memory_order_relaxed);
            }
            progress = true;
        }
        if (target.connection && state.transport_retired() && !target.retirement_started) {
            begin_retirement(target);
            progress = true;
        }
        if (target.identity.epoch != 0 && state.transport_retired()) {
            if (!target.connection && !state.worker_finalized()) {
                progress = state.mark_worker_finalized(target.identity) ==
                               http3_connection_state::status::changed ||
                           progress;
            }
            if (state.worker_finalized() &&
                state.retire(target.identity) == http3_connection_state::status::changed) {
                if (target.connection) {
                    target.connection.reset();
                    active_connections_.fetch_sub(1, std::memory_order_relaxed);
                }
                clear_slot(target);
                progress = true;
            }
        }
    }
    return progress;
}

bool http3_worker_server::pump_input() noexcept {
    if (request_buffer_ == nullptr) {
        return false;
    }
    bool progress = false;
    const auto sameStream = [](const http3_stream_id& a, const http3_stream_id& b) noexcept {
        return a.epoch == b.epoch && a.connection_generation == b.connection_generation && a.stream_id == b.stream_id;
    };
    const auto consume = [this, &progress](http3_stream_buffer::borrowed_block& block) noexcept {
        auto* slot = find_slot(block.id());
        if (stopping_ || slot == nullptr || slot->connection == nullptr) {
            block.release();
            progress = true;
            return;
        }
        if (!slot->connection->canAcceptInput(block.id().stream_id, block.bytes().size())) {
            return;
        }
        const auto result = slot->connection->acceptData(block);
        block.release();
        progress = true;
        if (result.connectionCloseRequired) {
            (void)slot->connection->requestStop();
            begin_retirement(*slot);
        }
    };
    for (auto& pending : pending_input_) {
        if (pending_input_count_ == 0) {
            break;
        }
        if (!pending.block) {
            continue;
        }
        const bool earlier = std::ranges::any_of(pending_input_, [&](const pending_input& other) {
            return other.block && other.sequence < pending.sequence && sameStream(other.block.id(), pending.block.id());
        });
        if (!earlier) {
            consume(pending.block);
            if (!pending.block) {
                --pending_input_count_;
            }
        }
    }
    for (std::size_t count = 0; count < input_turn_budget;) {
        bool received = false;
        http3_stream_control control;
        if (request_buffer_->try_receive_control(control)) {
            received = true;
            progress = true;
            ++count;
            if (auto* slot = find_slot(control.id); slot != nullptr && slot->connection != nullptr) {
                const auto result = slot->connection->acceptControl(control);
                if (result.connectionCloseRequired) {
                    (void)slot->connection->requestStop();
                    begin_retirement(*slot);
                }
            }
        }
        http3_stream_buffer::borrowed_block block;
        if (count < input_turn_budget && request_buffer_->try_receive(block)) {
            received = true;
            progress = true;
            ++count;
            const bool earlier = pending_input_count_ != 0 && std::ranges::any_of(pending_input_, [&](const pending_input& pending) {
                return pending.block && sameStream(pending.block.id(), block.id());
            });
            if (!earlier) {
                consume(block);
            }
            if (block) {
                auto free = std::ranges::find_if(pending_input_, [](const pending_input& pending) { return !pending.block; });
                if (free == pending_input_.end() || next_input_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
                    std::terminate();
                }
                free->block = std::move(block);
                free->sequence = next_input_sequence_++;
                ++pending_input_count_;
            }
        }
        if (!received) {
            break;
        }
    }
    // If the budget was exhausted, the run loop immediately takes another
    // bounded turn (and yields after its outer budget); queued work is not
    // dependent on a fresh producer notification.
    return request_buffer_->has_pending() || progress;
}

bool http3_worker_server::pump_scheduler() noexcept {
    bool progress = false;
    for (std::size_t count = 0; count < pump_budget; ++count) {
        const auto step = scheduler_->step();
        if (step.kind == http3_ready_scheduler::step_kind::idle) {
            break;
        }
        if (step.kind == http3_ready_scheduler::step_kind::wrong_worker) {
            std::terminate();
        }
        progress = true;
        if (step.kind != http3_ready_scheduler::step_kind::transport_intent) {
            continue;
        }
        const auto found = std::ranges::find_if(slots_, [&step](const slot& target) {
            return target.registration.token == step.connection;
        });
        if (found == slots_.end() || !found->state) {
            std::terminate();
        }
        const auto executed = found->state->execute_intent(found->identity, step.intent);
        if ((executed.outcome != http3_connection_state::execution_outcome::executed &&
                executed.outcome != http3_connection_state::execution_outcome::transport_retired) ||
            !scheduler_->acknowledge_intent(step.connection, step.intent.token, executed.push_stream)) {
            std::terminate();
        }
    }
    return progress;
}

bool http3_worker_server::publish_drain_completions() noexcept {
    bool progress = false;
    for (auto& target : slots_) {
        if (!target.connection || target.retirement_started || target.state->worker_drained()) {
            continue;
        }
        const auto sealed = target.state->admission_seal();
        if (sealed && target.connection->drainReady(sealed->expected_admitted_requests)) {
            if (target.state->mark_worker_drained(target.identity) != http3_connection_state::status::changed) {
                std::terminate();
            }
            progress = true;
        }
    }
    return progress;
}

http3_worker_server::slot* http3_worker_server::find_slot(http3_stream_id id) noexcept {
    return find_slot(http3_connection_identity{id.epoch, id.connection_generation});
}

http3_worker_server::slot* http3_worker_server::find_slot(http3_connection_identity identity) noexcept {
    const auto found = std::ranges::find_if(slots_, [&identity](const slot& target) {
        return target.connection && target.identity == identity;
    });
    return found == slots_.end() ? nullptr : &*found;
}

bool http3_worker_server::construct_connection(slot& target,
    const http3_connection_state::binding_snapshot& binding) noexcept {
    if (target.connection || binding.identity != target.identity ||
        binding.metadata.remote_address.empty() || binding.metadata.remote_port == 0 ||
        !options_.maxConnections ||
        active_connections_.load(std::memory_order_relaxed) >= *options_.maxConnections) {
        return false;
    }
    try {
        target.remote_address = binding.metadata.remote_address;
        target.client_certificate_subject = binding.metadata.client_certificate_subject;
        target.remote_port = binding.metadata.remote_port;
        auto services = capabilities_.contextServices(stop_token_).withTlsTransport(target.remote_address, target.client_certificate_subject, target.remote_port);
        const auto max_requests = options_.max_requests_per_connection.value_or(0);
        if (max_requests == 0) {
            return false;
        }
        const auto tracked = http3WorkerTrackedStreamCapacity(max_requests);
        Http3ServerConnectionConfig config{
            .epoch = binding.identity.epoch,
            .connectionGeneration = binding.identity.connection_generation,
            .session = {
                .max_buffered_body_bytes = options_.max_buffered_body_bytes,
                .max_stream_body_bytes = options_.max_stream_body_bytes,
                .maxLiveStreams = tracked,
                .maxBufferedBytesInFlight = body_budget_bytes,
                .connection = {.maxActiveStreams = tracked,
                    .qpackMaxTableCapacity = static_cast<std::size_t>(binding.settings.qpackMaxTableCapacity),
                    .qpackBlockedStreams = static_cast<std::size_t>(binding.settings.qpackBlockedStreams),
                    .enableConnectProtocol = binding.settings.enableConnectProtocol,
                    .enableDatagrams = binding.settings.h3Datagram},
                .maxQuicDatagramPayloadBytes = binding.max_quic_datagram_payload_bytes,
                .inbound_buffer_pool = options_.inbound_buffer_pool,
                .max_inbound_buffer_bytes = options_.max_inbound_buffer_bytes_per_connection,
            },
            .maxTrackedStreams = tracked,
            .connectionScanner = &connection_scanner_,
            .executor = executor_,
            .datagramOutput = {.context = &target, .send = [](void* context, std::uint64_t stream_id, std::span<const std::byte> bytes) {
                                   auto& output = *static_cast<slot*>(context);
                                   if (!output.state || output.retirement_started || output.state->transport_retired()) {
                                       throw std::runtime_error("HTTP Datagram connection is closed");
                                   }
                                   (void)output.state->publish_response_datagram(
                                       output.identity, stream_id, bytes);
                               }},
        };
        target.connection = makePmrObject<Http3ServerConnection>(memory_.resource(), routes_,
            memory_, services, options_, response_buffer_, target.registration.activation,
            body_budget_, config);
        if (target.state->attach_handler(target.identity, *target.connection) != http3_connection_state::status::changed) {
            std::terminate();
        }
        active_connections_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        if (target.connection) {
            std::terminate();
        }
        return false;
    }
}

void http3_worker_server::begin_retirement(slot& target) noexcept {
    if (!target.connection || target.retirement_started) {
        return;
    }
    target.retirement_started = true;
    (void)target.connection->requestStop();
    if (target.state->start_worker_draining(target.identity) != http3_connection_state::status::changed) {
        std::terminate();
    }
}

Task<void> http3_worker_server::join_retired_slot(slot& target) {
    co_await target.connection->join();
    if (target.state->mark_worker_finalized(target.identity) != http3_connection_state::status::changed) {
        std::terminate();
    }
    wake();
}

void http3_worker_server::clear_slot(slot& target) noexcept {
    if (target.connection) {
        std::terminate();
    }
    target.registration = {};
    target.identity = {};
    target.retirement_started = false;
    target.join_started = false;
    target.remote_address.clear();
    target.client_certificate_subject.clear();
    target.remote_port = 0;
}

void http3_worker_server::finish_stopped_slots() noexcept {
    for (auto& target : slots_) {
        if (!target.connection || !target.retirement_started || !target.state->transport_retired() ||
            target.join_started || target.connection->pendingTransportIntentCount() != 0) {
            continue;
        }
        target.join_started = true;
        try {
            retirement_tasks_.spawn(join_retired_slot(target));
        } catch (...) {
            std::terminate();
        }
    }
}

}  // namespace ruvia::detail
