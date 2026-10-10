#include "http3/http3_worker_server.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/async.h"

#include "context/context_services.h"
#include "integration/worker_capabilities.h"
#include "router/route_table.h"
#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {
namespace {
constexpr std::size_t body_budget_bytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t pump_budget = 256;
constexpr std::size_t input_turn_budget = 64;
}  // namespace

http3_worker_server::http3_worker_server(const worker_handle& worker_value,
    worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
    connection_scanner& connection_scanner_value, asio::any_io_executor executor,
    const http_server_options& options, const stop_token& stop_token_value,
    std::size_t max_connections, std::uint32_t buffer_capacity,
    std::atomic<std::size_t>& active_connections,
    std::atomic<std::size_t>& refused_connections)
    : worker_(worker_value),
      memory_(memory),
      routes_(routes_value),
      capabilities_(capabilities),
      connection_scanner_(connection_scanner_value),
      executor_(std::move(executor)),
      options_(options),
      stop_token_(stop_token_value),
      active_connections_(active_connections),
      refused_connections_(refused_connections),
      signal_(worker_value),
      response_buffer_(buffer_capacity, buffer_capacity, buffer_capacity, memory.resource()),
      body_budget_(body_budget_bytes),
      retirement_tasks_(worker_value, {.resource_ = memory.resource()}),
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
    if (staged_ || link.request_buffer_ == nullptr || link.connections_.size() != slots_.size() ||
        link.request_buffer_->block_capacity() > pending_input_.size() || link.protocol_ready_.notify_ == nullptr) {
        return false;
    }
    for (auto* state : link.connections_) {
        if (state == nullptr) {
            return false;
        }
    }
    request_buffer_ = link.request_buffer_;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].state_ = link.connections_[i];
    }
    response_buffer_.set_local_notifications({.ready_ = link.protocol_ready_,
        .capacity_ = {this, [](void* context_value, std::uint8_t lanes) noexcept {
                          auto& server = *static_cast<http3_worker_server*>(context_value);
                          if (server.scheduler_) {
                              server.scheduler_->receive_capacity(lanes);
                          }
                          server.wake();
                      }}});
    staged_ = true;
    return true;
}

bool http3_worker_server::install() noexcept {
    if (!worker_.is_current() || !staged_ || installed_ || stopping_ || drained_) {
        return false;
    }
    try {
        scheduler_.emplace(worker_, max_connections_, memory_.resource(),
            http3_ready_scheduler::local_ready_callback{this, [](void* context_value) noexcept {
                                                            static_cast<http3_worker_server*>(context_value)->wake();
                                                        }});
    } catch (...) {
        return false;
    }
    installed_ = true;
    wake();
    return true;
}

task<void> http3_worker_server::run() {
    if (!worker_.is_current() || run_started_ || !installed_) {
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
                return target.connection_ != nullptr || target.identity_.epoch_ != 0;
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
            const auto yielded = co_await ruvia::async_asio([this](auto completion) {
                asio::post(executor_, asio::bind_allocator(
                                          std::pmr::polymorphic_allocator<std::byte>(memory_.resource()),
                                          [completion = std::move(completion)]() mutable {
                                              completion(asio::error_code{});
                                          }));
            });
            if (yielded.error_code()) {
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
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto& pending : pending_input_) {
        pending.block_.release();
    }
    pending_input_count_ = 0;
    for (auto& target : slots_) {
        if (target.state_) {
            target.state_->stop_admission();
        }
        if (target.connection_) {
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
    if (run_started_ || (installed_ && !worker_.is_current())) {
        std::terminate();
    }
    if (drained_) {
        return;
    }
    stopping_ = true;
    for (auto& target : slots_) {
        if (target.state_) {
            target.state_->stop_admission();
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
    if (!worker_.is_current() || !installed_ || !scheduler_) {
        return false;
    }
    bool progress_value = pump_states();
    progress_value = pump_input() || progress_value;
    for (auto& target : slots_) {
        if (!target.connection_ || target.retirement_started_ || !target.state_) {
            continue;
        }
        http3_connection_state::datagram datagram;
        for (std::size_t pass = 0; pass < 16; ++pass) {
            if (target.state_->pop_request_datagram(datagram) != http3_connection_state::status::changed) {
                break;
            }
            target.connection_->receive_datagram(datagram.bytes());
            datagram.storage_.reset();
            progress_value = true;
        }
        progress_value = target.connection_->resume_qpack_input() || progress_value;
    }
    progress_value = pump_scheduler() || progress_value;
    progress_value = publish_drain_completions() || progress_value;
    finish_stopped_slots();
    return progress_value;
}

bool http3_worker_server::pump_states() noexcept {
    bool progress_value = false;
    for (auto& target : slots_) {
        auto& state_value = *target.state_;
        if (target.identity_.epoch_ != 0 && (state_value.slot_reusable() || state_value.identity() != target.identity_)) {
            if (target.connection_) {
                std::terminate();
            }
            clear_slot(target);
            progress_value = true;
        }
        if (!stopping_ && state_value.admission() == http3_connection_state::admission_phase::vacant) {
            if (state_value.reserve(*scheduler_, epoch_, next_connection_generation_) == http3_connection_state::status::changed) {
                if (++next_connection_generation_ == 0) {
                    std::terminate();
                }
                target.identity_ = *state_value.identity();
                target.registration_ = *state_value.registration();
                progress_value = true;
            }
        }
        if (state_value.admission() == http3_connection_state::admission_phase::bound && !target.connection_) {
            const auto binding = state_value.binding();
            std::optional<http3_connection_state::reject_reason> rejection;
            if (stopping_) {
                rejection = http3_connection_state::reject_reason::stopping;
            } else if (options_.max_connections_ &&
                       active_connections_.load(std::memory_order_relaxed) >= *options_.max_connections_) {
                rejection = http3_connection_state::reject_reason::capacity;
            } else if (!binding || !construct_connection(target, *binding)) {
                rejection = http3_connection_state::reject_reason::construction_failed;
            }
            if (rejection) {
                (void)state_value.reject(target.identity_, *rejection);
                refused_connections_.fetch_add(1, std::memory_order_relaxed);
            }
            progress_value = true;
        }
        if (target.connection_ && state_value.transport_retired() && !target.retirement_started_) {
            begin_retirement(target);
            progress_value = true;
        }
        if (target.identity_.epoch_ != 0 && state_value.transport_retired()) {
            if (!target.connection_ && !state_value.worker_finalized()) {
                progress_value = state_value.mark_worker_finalized(target.identity_) ==
                                     http3_connection_state::status::changed ||
                                 progress_value;
            }
            if (state_value.worker_finalized() &&
                state_value.retire(target.identity_) == http3_connection_state::status::changed) {
                if (target.connection_) {
                    target.connection_.reset();
                    active_connections_.fetch_sub(1, std::memory_order_relaxed);
                }
                clear_slot(target);
                progress_value = true;
            }
        }
    }
    return progress_value;
}

bool http3_worker_server::pump_input() noexcept {
    if (request_buffer_ == nullptr) {
        return false;
    }
    bool progress_value = false;
    const auto same_stream = [](const http3_stream_id& a, const http3_stream_id& b) noexcept {
        return a.epoch_ == b.epoch_ && a.connection_generation_ == b.connection_generation_ && a.stream_id_ == b.stream_id_;
    };
    const auto consume = [this, &progress_value](http3_stream_buffer::borrowed_block& block) noexcept {
        auto* slot = find_slot(block.id());
        if (stopping_ || slot == nullptr || slot->connection_ == nullptr) {
            block.release();
            progress_value = true;
            return;
        }
        if (!slot->connection_->can_accept_input(block.id().stream_id_, block.bytes().size())) {
            return;
        }
        const auto result_value = slot->connection_->accept_data(block);
        block.release();
        progress_value = true;
        if (result_value.connection_close_required_) {
            (void)slot->connection_->request_stop();
            begin_retirement(*slot);
        }
    };
    for (auto& pending : pending_input_) {
        if (pending_input_count_ == 0) {
            break;
        }
        if (!pending.block_) {
            continue;
        }
        const bool earlier = std::ranges::any_of(pending_input_, [&](const pending_input& other) {
            return other.block_ && other.sequence_ < pending.sequence_ && same_stream(other.block_.id(), pending.block_.id());
        });
        if (!earlier) {
            consume(pending.block_);
            if (!pending.block_) {
                --pending_input_count_;
            }
        }
    }
    for (std::size_t count = 0; count < input_turn_budget;) {
        bool received_value = false;
        http3_stream_control control;
        if (request_buffer_->try_receive_control(control)) {
            received_value = true;
            progress_value = true;
            ++count;
            if (auto* slot = find_slot(control.id_); slot != nullptr && slot->connection_ != nullptr) {
                const auto result_value = slot->connection_->accept_control(control);
                if (result_value.connection_close_required_) {
                    (void)slot->connection_->request_stop();
                    begin_retirement(*slot);
                }
            }
        }
        http3_stream_buffer::borrowed_block block;
        if (count < input_turn_budget && request_buffer_->try_receive(block)) {
            received_value = true;
            progress_value = true;
            ++count;
            const bool earlier = pending_input_count_ != 0 && std::ranges::any_of(pending_input_, [&](const pending_input& pending) {
                return pending.block_ && same_stream(pending.block_.id(), block.id());
            });
            if (!earlier) {
                consume(block);
            }
            if (block) {
                auto free = std::ranges::find_if(pending_input_, [](const pending_input& pending) { return !pending.block_; });
                if (free == pending_input_.end() || next_input_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
                    std::terminate();
                }
                free->block_ = std::move(block);
                free->sequence_ = next_input_sequence_++;
                ++pending_input_count_;
            }
        }
        if (!received_value) {
            break;
        }
    }
    // If the budget was exhausted, the run loop immediately takes another
    // bounded turn (and yields after its outer budget); queued work is not
    // dependent on a fresh producer notification.
    return request_buffer_->has_pending() || progress_value;
}

bool http3_worker_server::pump_scheduler() noexcept {
    bool progress_value = false;
    for (std::size_t count = 0; count < pump_budget; ++count) {
        const auto step = scheduler_->step();
        if (step.kind_ == http3_ready_scheduler::step_kind::idle) {
            break;
        }
        if (step.kind_ == http3_ready_scheduler::step_kind::wrong_worker) {
            std::terminate();
        }
        progress_value = true;
        if (step.kind_ != http3_ready_scheduler::step_kind::transport_intent) {
            continue;
        }
        const auto found = std::ranges::find_if(slots_, [&step](const slot& target) {
            return target.registration_.token_ == step.connection_;
        });
        if (found == slots_.end() || !found->state_) {
            std::terminate();
        }
        const auto executed = found->state_->execute_intent(found->identity_, step.intent_);
        if ((executed.outcome_ != http3_connection_state::execution_outcome::executed &&
                executed.outcome_ != http3_connection_state::execution_outcome::transport_retired) ||
            !scheduler_->acknowledge_intent(step.connection_, step.intent_.token_, executed.push_stream_)) {
            std::terminate();
        }
    }
    return progress_value;
}

bool http3_worker_server::publish_drain_completions() noexcept {
    bool progress_value = false;
    for (auto& target : slots_) {
        if (!target.connection_ || target.retirement_started_ || target.state_->worker_drained()) {
            continue;
        }
        const auto sealed = target.state_->admission_seal();
        if (sealed && target.connection_->drain_ready(sealed->expected_admitted_requests_)) {
            if (target.state_->mark_worker_drained(target.identity_) != http3_connection_state::status::changed) {
                std::terminate();
            }
            progress_value = true;
        }
    }
    return progress_value;
}

http3_worker_server::slot* http3_worker_server::find_slot(http3_stream_id id) noexcept {
    return find_slot(http3_connection_identity{id.epoch_, id.connection_generation_});
}

http3_worker_server::slot* http3_worker_server::find_slot(http3_connection_identity identity) noexcept {
    const auto found = std::ranges::find_if(slots_, [&identity](const slot& target) {
        return target.connection_ && target.identity_ == identity;
    });
    return found == slots_.end() ? nullptr : &*found;
}

bool http3_worker_server::construct_connection(slot& target,
    const http3_connection_state::binding_snapshot& binding) noexcept {
    if (target.connection_ || binding.identity_ != target.identity_ ||
        binding.metadata_.remote_address_.empty() || binding.metadata_.remote_port_ == 0 ||
        !options_.max_connections_) {
        return false;
    }
    try {
        target.remote_address_ = binding.metadata_.remote_address_;
        target.client_certificate_subject_ = binding.metadata_.client_certificate_subject_;
        target.remote_port_ = binding.metadata_.remote_port_;
        auto services = capabilities_.make_context_services(stop_token_).with_tls_transport(target.remote_address_, target.client_certificate_subject_, target.remote_port_);
        const auto max_requests = options_.max_requests_per_connection_.value_or(0);
        if (max_requests == 0) {
            return false;
        }
        const auto tracked = http3_worker_tracked_stream_capacity(max_requests);
        http3_server_connection_config config{
            .epoch_ = binding.identity_.epoch_,
            .connection_generation_ = binding.identity_.connection_generation_,
            .session_ = {
                .max_buffered_body_bytes_ = options_.max_buffered_body_bytes_,
                .max_stream_body_bytes_ = options_.max_stream_body_bytes_,
                .max_live_streams_ = tracked,
                .max_buffered_bytes_in_flight_ = body_budget_bytes,
                .connection_ = {.max_active_streams_ = tracked,
                    .qpack_max_table_capacity_ = static_cast<std::size_t>(binding.settings_.qpack_max_table_capacity_),
                    .qpack_blocked_streams_ = static_cast<std::size_t>(binding.settings_.qpack_blocked_streams_),
                    .enable_connect_protocol_ = binding.settings_.enable_connect_protocol_,
                    .enable_datagrams_ = binding.settings_.h3_datagram_},
                .max_quic_datagram_payload_bytes_ = binding.max_quic_datagram_payload_bytes_,
                .inbound_buffer_pool_ = options_.inbound_buffer_pool_,
                .max_inbound_buffer_bytes_ = options_.max_inbound_buffer_bytes_per_connection_,
            },
            .max_tracked_streams_ = tracked,
            .connection_scanner_ = &connection_scanner_,
            .executor_ = executor_,
            .datagram_output_ = {.context_ = &target, .send_ = [](void* context_value, std::uint64_t stream_id, std::span<const std::byte> bytes_value) {
                                     auto& output = *static_cast<slot*>(context_value);
                                     if (!output.state_ || output.retirement_started_ || output.state_->transport_retired()) {
                                         throw std::runtime_error("HTTP Datagram connection is closed");
                                     }
                                     (void)output.state_->publish_response_datagram(
                                         output.identity_, stream_id, bytes_value);
                                 }},
        };
        target.connection_ = make_pmr_object<http3_server_connection>(memory_.resource(), routes_,
            memory_, services, options_, response_buffer_, target.registration_.activation_,
            body_budget_, config);
        if (target.state_->attach_handler(target.identity_, *target.connection_) != http3_connection_state::status::changed) {
            std::terminate();
        }
        active_connections_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        if (target.connection_) {
            std::terminate();
        }
        return false;
    }
}

void http3_worker_server::begin_retirement(slot& target) noexcept {
    if (!target.connection_ || target.retirement_started_) {
        return;
    }
    target.retirement_started_ = true;
    (void)target.connection_->request_stop();
    if (target.state_->start_worker_draining(target.identity_) != http3_connection_state::status::changed) {
        std::terminate();
    }
}

task<void> http3_worker_server::join_retired_slot(slot& target) {
    co_await target.connection_->join();
    if (target.state_->mark_worker_finalized(target.identity_) != http3_connection_state::status::changed) {
        std::terminate();
    }
    wake();
}

void http3_worker_server::clear_slot(slot& target) noexcept {
    if (target.connection_) {
        std::terminate();
    }
    target.registration_ = {};
    target.identity_ = {};
    target.retirement_started_ = false;
    target.join_started_ = false;
    target.remote_address_.clear();
    target.client_certificate_subject_.clear();
    target.remote_port_ = 0;
}

void http3_worker_server::finish_stopped_slots() noexcept {
    for (auto& target : slots_) {
        if (!target.connection_ || !target.retirement_started_ || !target.state_->transport_retired() ||
            target.join_started_ || target.connection_->pending_transport_intent_count() != 0) {
            continue;
        }
        target.join_started_ = true;
        try {
            retirement_tasks_.spawn(join_retired_slot(target));
        } catch (...) {
            std::terminate();
        }
    }
}

}  // namespace ruvia::detail
