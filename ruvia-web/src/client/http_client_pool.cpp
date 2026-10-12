#include "client/http_client_pool.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/worker_cancellation.h"

#include "client/client_transport.h"
#include "client/http_client_connection.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "http1/http1_client_exchange.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::shared_ptr<http_client_result_budget_domain> require_http_client_result_budget_domain(
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain) {
    if (!result_budget_domain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return result_budget_domain;
}

// An abandoned response may still be queued for a slot or connecting, before
// its exchange registers a cancellation id: it must not write a request.
void throw_if_abandoned(const http_client_response_state& state_value) {
    if (state_value.abandoned_) {
        throw http_client_error(
            http_client_error::code_type::cancelled, "HTTP request abandoned before transport admission");
    }
}

}  // namespace

static_assert(worker_cancellation_post_is_inline<http_client_cancellation_target>);

void http_client_slot_policy::throw_acquire_failure(const http_client_pool&, const pool_waiter_result& result) {
    if (result.timed_out() != nullptr) {
        throw http_client_error(
            http_client_error::code_type::timeout, "http client connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw http_client_error(http_client_error::code_type::cancelled, "http client request cancelled");
    }
    if (result.closed() != nullptr) {
        throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
    }
    std::terminate();
}

void http_client_slot_policy::throw_aborted(const http_client_pool&, pool_slot_abort_reason reason) {
    if (reason == pool_slot_abort_reason::cancelled) {
        throw http_client_error(http_client_error::code_type::cancelled, "http client request cancelled");
    }
    throw http_client_error(http_client_error::code_type::closing, "http client pool is closing");
}

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config, std::shared_ptr<http_client_result_budget_domain> result_budget_domain,
    std::pmr::memory_resource* resource, http_client_response_storage response_storage)
    : io_context_(io_context),
      worker_(worker_value),
      resource_(pmr_resource_or_default(resource)),
      config_(std::move(config)),
      origin_(config_.origin()),
      policy_(config_, resource_),
      result_budget_domain_(require_http_client_result_budget_domain(std::move(result_budget_domain))),
      tls_context_(asio::ssl::context::tls_client),
      slots_(*this, config_.connection_count_, worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)),
      background_tasks_(worker_, {.resource_ = resource_}),
      connections_(resource_) {
    if (config_.scheme_ == http_scheme::https) {
        configure_client_tls_context(*tls_context_.native_handle(), config_.tls_);
    }
    connections_.reserve(config_.connection_count_);
    for (std::size_t index = 0; index < config_.connection_count_; ++index) {
        connections_.push_back(make_pmr_object<http_client_connection>(resource_, *this, index));
    }
    response_memory_ = response_storage == http_client_response_storage::worker_resource
                           ? http_client_response_memory_domain::create(worker_, result_budget_domain_, *resource_)
                           : http_client_response_memory_domain::create_response_owned(worker_, result_budget_domain_);
}

http_client_pool::~http_client_pool() {
    close_now();
}

void http_client_pool::close_now() noexcept {
    cancellation_target_->detach(*this);
    if (!slots_.close()) {
        return;
    }
    // slots_.close() retires queued waiters and connection shutdown aborts
    // every connect and exchange; a transferred slot fails at admission.
    for (auto& connection_value : connections_) {
        connection_value->shutdown();
    }
}

task<void> http_client_pool::join() {
    if (background_joined_) {
        co_return;
    }
    background_joined_ = true;
    std::exception_ptr failure;
    try {
        co_await background_tasks_.join();
    } catch (...) {
        // A child failure is observable only after join retired every child.
        // If join itself could not start, its live drivers still borrow the
        // transport owners below: this is a terminal ownership violation.
        if (background_tasks_.size() != 0) {
            std::terminate();
        }
        failure = std::current_exception();
    }
    // Consumers own their response storage independently of transport. Retire
    // every client borrow, including completed responses, before its owners.
    response_memory_->detach_transport_bindings(*this);
    response_memory_.reset();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_stats http_client_pool::stats() const noexcept {
    return {requests_buffered_, requests_in_flight_, completed_requests_, failed_requests_, wire_counters_.sent_, wire_counters_.received_};
}

void http_client_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    for (auto& connection_value : connections_) {
        if (connection_value->cancel_by_id(cancellation_id)) {
            return;
        }
    }
}

void http_client_pool::require_accepting() const {
    // join() retires background_tasks_, which then rejects every spawn.
    if (background_joined_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client pool is retired");
    }
}

void http_client_pool::spawn_request(
    http_client_request_storage request, operation_options options, http_client_response_state& state_value) {
    state_value.buffered_limit_ = config_.max_response_bytes_;
    background_tasks_.spawn(execute_into(std::move(request), std::move(options), &state_value));
}

task<http_client_tunnel_result> http_client_pool::open_tunnel(
    http_client_request_storage request, http_client_tunnel_config config, operation_options options) {
    require_accepting();
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.tunnel_.emplace(state_value.memory_domain()->worker(), state_value.resource_, config);
    state_value.tunnel_->udp_ = owned_request.udp_tunnel();
    if (state_value.tunnel_->udp_) {
        state_value.tunnel_->config_.datagrams_ = true;
    }
    owned_request.bind_tunnel(*state_value.tunnel_);
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_await state_value.await_head("CONNECT handshake failed");
    if (state_value.tunnel_->accepted_) {
        co_return http_client_tunnel_result(http_client_tunnel(std::move(response)));
    }
    co_return http_client_tunnel_result(std::move(response));
}

task<http_client_exchange> http_client_pool::open_request(
    http_client_request_storage request, http_client_upload_config upload, operation_options options) {
    require_accepting();
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.upload_.emplace(state_value.memory_domain()->worker(), state_value.resource_, upload);
    state_value.upload_->content_released_ = upload.expectation_ == http_client_request_expectation::none;
    owned_request.bind_upload(*state_value.upload_);
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_return http_client_exchange(std::move(response));
}

task<http_client_response> http_client_pool::execute(http_client_request_storage request, operation_options options) {
    require_accepting();
    // The request owns all data before transport work can outlive the caller.
    auto owned_request = std::move(request).into_resource(resource_);
    http_client_response response(*this);
    auto& state_value = *response.state_;
    spawn_request(std::move(owned_request), std::move(options), state_value);
    co_await state_value.await_head("HTTP client request failed before the response head");
    co_return response;
}

task<void> http_client_pool::execute_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    http_client_response keep_alive(state_value, true);
    try {
        throw_if_abandoned(*state_value);
        co_await execute_request_into(std::move(request), std::move(options), state_value);
    } catch (...) {
        state_value->failure_ = std::current_exception();
    }
    state_value->complete_ = true;
    if (auto* output = state_value->output()) {
        output->stop();
    }
    state_value->head_signal_.notify();
    state_value->data_signal_.notify();
}

task<void> http_client_pool::execute_request_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    const operation_timeout timeout(options.timeout_.has_value() ? options.timeout_ : config_.request_timeout_);
    if (requests_buffered_ >= config_.max_buffered_requests_) {
        ++failed_requests_;
        throw http_client_error(http_client_error::code_type::queue_full, "http client request buffer is full");
    }
    ++requests_buffered_;
    std::size_t index = 0;
    try {
        index = co_await slots_.acquire(timeout, config_.acquire_timeout_, options.stop_token_);
    } catch (...) {
        --requests_buffered_;
        ++failed_requests_;
        throw;
    }
    --requests_buffered_;
    ++requests_in_flight_;
    struct lease_type final {
        http_client_pool& pool_;
        std::size_t index_;
        // Set while a transport operation may leave request bytes or a
        // half-written connect on the connection.
        http_client_connection* discard_{nullptr};
        ~lease_type() {
            if (discard_ != nullptr) {
                discard_->close();
            }
            pool_.slots_.release(index_);
        }
    } lease{*this, index};
    try {
        // A slot transferred by a releasing lease resumes its waiter after
        // close_now() may already have retired every connection.
        slots_.throw_if_aborted(index);
        auto& tcp_connection = connection(index);
        state_value->connection_index_ = index;
        // Abandonment before the exchange starts leaves no request bytes on
        // the connection, so it stays reusable.
        throw_if_abandoned(*state_value);
        lease.discard_ = &tcp_connection;
        co_await tcp_connection.ensure_connected(timeout, options.stop_token_, *state_value);
        lease.discard_ = nullptr;
        throw_if_abandoned(*state_value);
        lease.discard_ = &tcp_connection;
        http1_client_exchange exchange(*this, tcp_connection);
        co_await exchange.run(request, timeout, options.stop_token_, *state_value);
        lease.discard_ = nullptr;
        --requests_in_flight_;
        ++completed_requests_;
    } catch (...) {
        --requests_in_flight_;
        ++failed_requests_;
        throw;
    }
}

void http_client_pool::abandon_response(http_client_response_state& state_value) noexcept {
    if (state_value.abandoned_) {
        return;
    }
    state_value.abandoned_ = true;
    // A connect or exchange in progress published its id; a response still
    // queued for a slot has none and fails at admission.
    cancel_operation_by_id(state_value.cancellation_id_);
    if (auto* output = state_value.output()) {
        output->stop();
    }
    state_value.space_signal_.notify();
}

}  // namespace ruvia::detail
