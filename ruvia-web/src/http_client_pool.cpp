#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"

#include "web_internal.h"

namespace ruvia::detail {
namespace {
void throw_if_abandoned(const http_client_response_state& state_value) {
    if (state_value.abandoned_) {
        throw http_client_error(
            std::make_error_code(std::errc::operation_canceled), "HTTP request abandoned before transport admission");
    }
}

// Position for a new connection owner: the first free entry, else appended.
// Existing owners never move; only the vector of owner pointers may grow.
template <typename owner_type>
[[nodiscard]] std::size_t free_connection_position(std::vector<owner_type>& owners) {
    const auto free = std::ranges::find(owners, nullptr);
    if (free != owners.end()) {
        return static_cast<std::size_t>(free - owners.begin());
    }
    owners.emplace_back(nullptr, typename owner_type::deleter_type{});
    return owners.size() - 1;
}

#ifdef RUVIA_ENABLE_HTTP3
// How long an alternative service that failed to connect stays unused.
constexpr std::chrono::minutes alternative_service_retry_delay{5};

// RFC 7838 field members: commas and semicolons inside quoted strings do not
// delimit members. Unsupported ALPN identifiers are skipped, never downgraded.
#endif
}  // namespace

bool can_retry_reused_connection_request(std::optional<std::error_code> failure,
    const http_client_request_storage& request, const http_client_response_state& response) noexcept {
    return failure == make_error_code(http_client_errc::io_error) && request.upload() == nullptr && !request.is_tunnel() &&
           is_http_method_idempotent(request.method()) && !response.head_ready_ && response.informational_.empty() &&
           !response.abandoned_;
}

// Moves a request from buffered to in flight once a connection serves it.
struct http_client_pool::request_admission final {
    http_client_pool& pool_;
    bool in_flight_{false};
    void admit() noexcept {
        if (!std::exchange(in_flight_, true)) {
            --pool_.requests_buffered_;
            ++pool_.requests_in_flight_;
        }
    }
    ~request_admission() {
        if (in_flight_) {
            --pool_.requests_in_flight_;
        } else {
            --pool_.requests_buffered_;
        }
    }
};

// One admission wait's cancellation: a stop reaches cancel_operation_by_id(),
// which wakes admission waiters to observe it.
struct http_client_pool::admission_waiter final {
    admission_waiter(http_client_pool& pool, http_client_response_state& state_value, const stop_token& stop)
        : pool_(pool),
          next_(pool.admission_waiters_),
          cancellation_(pool.cancellation_target_, cancellation_id_, state_value) {
        pool_.admission_waiters_ = this;
        try {
            cancellation_.arm(stop);
        } catch (...) {
            unlink();
            throw;
        }
    }
    admission_waiter(const admission_waiter&) = delete;
    admission_waiter& operator=(const admission_waiter&) = delete;
    ~admission_waiter() {
        unlink();
    }
    void unlink() noexcept {
        auto** link = &pool_.admission_waiters_;
        while (*link != this) {
            if (*link == nullptr) {
                std::terminate();
            }
            link = &(*link)->next_;
        }
        *link = next_;
    }

    http_client_pool& pool_;
    admission_waiter* next_;
    std::uint64_t cancellation_id_{};
    http_client_response_cancellation<http_client_cancellation_target> cancellation_;
};

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_ref& worker_value,
    http_client_config_storage config, const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      config_(std::move(config)),
      origin_(config_.fixed_origin_ ? std::optional(config_.origin()) : std::nullopt),
      policy_(config_),
      advertisements_(worker_, config_.advertisements_),
      pushes_(),
      result_budget_domain_(services.result_budget_),
      worker_limits_(services.limits_),
      tls_context_(asio::ssl::context::tls_client),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)),
      background_tasks_(worker_),
      connections_(),
      admission_signal_(worker_),
      reclaim_signal_(worker_),
      dns_changed_(worker_) {
    // Connections are created on demand; a DNS alias still needs its lookup
    // pool from the start.
    if (!config_.dial_host_.empty()) {
        require_client_host_lookup(config_.dial_host_, worker_);
    }
    if (config_.scheme_ == http_scheme::https) {
        configure_client_tls_context(*tls_context_.native_handle(), config_.tls_);
    }
    response_memory_ = http_client_response_memory_domain::create(worker_, result_budget_domain_);
    schedule_dns_refresh();
}

std::uint64_t http_client_pool::next_connection_id() noexcept {
    static std::atomic<std::uint64_t> next{0};
    const auto id = next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == 0) {
        std::terminate();
    }
    return id;
}

std::vector<http_client_endpoint_address> http_client_pool::dns_snapshot() const {
    std::vector<http_client_endpoint_address> result;
    const auto append = [&result](const asio::ip::tcp::endpoint& endpoint) {
        result.emplace_back(to_ip_address(endpoint.address()).normalized_ipv4(), endpoint.port());
    };
    if (dns_addresses_.literal_) {
        append(*dns_addresses_.literal_);
    } else {
        result.reserve(dns_addresses_.addresses_.size());
        for (const auto& address : dns_addresses_.addresses_) {
            append(address.endpoint());
        }
    }
    return result;
}

void http_client_pool::validate_addresses(const client_resolve_result& result) const {
    if (!config_.address_validator_) {
        return;
    }
    const auto validate = [this](const asio::ip::tcp::endpoint& endpoint) {
        if (!(*config_.address_validator_)(
                to_ip_address(endpoint.address()).normalized_ipv4(), endpoint.port())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP dial address rejected");
        }
    };
    if (result.literal_) {
        validate(*result.literal_);
    } else {
        for (const auto& address : result.addresses_) {
            validate(address.endpoint());
        }
    }
}

void http_client_pool::install_addresses(client_resolve_result result) {
    validate_addresses(result);
    if (result.error_ || (!result.literal_ && result.addresses_.empty())) {
        throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP DNS resolution failed");
    }
    const bool had_addresses = dns_addresses_.literal_.has_value() || !dns_addresses_.addresses_.empty();
    const bool changed = dns_addresses_.literal_ != result.literal_ ||
                         !std::equal(dns_addresses_.addresses_.begin(), dns_addresses_.addresses_.end(),
                             result.addresses_.begin(), result.addresses_.end(),
                             [](const auto& left, const auto& right) { return left.endpoint() == right.endpoint(); });
    dns_addresses_ = std::move(result);
    ++dns_revision_;
    if (had_addresses && changed) {
        // Only connections dialed through this snapshot: request routes with
        // their own dial host keep theirs.
        for (auto& connection : connections_) {
            if (connection && connection->transport_.endpoint().port_ == config_.dial_port_ &&
                http_ascii_equals_ignore_case(connection->transport_.endpoint().host_, config_.dial_host_)) {
                connection->retire_addresses();
            }
        }
#ifdef RUVIA_ENABLE_HTTP3
        for (auto& connection : http3_connections_) {
            if (connection && connection->route().port_ == config_.dial_port_ &&
                http_ascii_equals_ignore_case(connection->route().host_, config_.dial_host_)) {
                connection->retire_addresses();
            }
        }
        if (http3_tls_) {
            http3_tls_->forget_endpoints();
        }
#endif
    }
}

void http_client_pool::update_dns(std::span<const http_client_endpoint_address> addresses) {
    require_accepting();
    require_fixed_dial();
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client pool is closed");
    }
    if (addresses.empty()) {
        throw std::invalid_argument("HTTP DNS snapshot must not be empty");
    }
    std::vector<asio::ip::tcp::endpoint> endpoints;
    endpoints.reserve(addresses.size());
    for (const auto& value : addresses) {
        if (value.port_.value() == 0) {
            throw std::invalid_argument("HTTP DNS snapshot requires numeric addresses and positive ports");
        }
        const asio::ip::tcp::endpoint endpoint(to_asio_address(value.ip_), value.port_.value());
        if (std::ranges::find(endpoints, endpoint) == endpoints.end()) {
            endpoints.push_back(endpoint);
        }
    }
    client_resolve_result result;
    if (endpoints.size() == 1) {
        result.literal_ = endpoints.front();
    } else {
        result.addresses_ = asio::ip::tcp::resolver::results_type::create(endpoints.begin(), endpoints.end(), "", "");
    }
    install_addresses(std::move(result));
}

task<client_resolve_result> http_client_pool::resolve_addresses(std::string_view host, std::uint16_t port,
    client_host_resolver& resolver, const operation_deadline& timeout) {
    const bool primary = host == config_.dial_host_ && port == config_.dial_port_;
    if (primary && (dns_addresses_.literal_ || !dns_addresses_.addresses_.empty())) {
        validate_addresses(dns_addresses_);
        co_return dns_addresses_;
    }
    const auto revision = dns_revision_;
    auto pending = resolver.resolve(host, port, timeout, dns_stop_.token());
    client_resolve_result result;
    if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
        result = std::move(*immediate);
    } else {
        result = co_await std::move(std::get<task<client_resolve_result>>(pending));
    }
    if (primary && revision != dns_revision_) {
        validate_addresses(dns_addresses_);
        co_return dns_addresses_;
    }
    if (!result.error_) {
        if (primary) {
            install_addresses(result);
        } else {
            validate_addresses(result);
        }
    }
    co_return result;
}

task<void> http_client_pool::refresh_dns(operation_options options) {
    require_accepting();
    require_fixed_dial();
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client pool is closed");
    }
    ++dns_active_;
    struct refresh_lease final {
        http_client_pool& pool_;
        ~refresh_lease() {
            --pool_.dns_active_;
            pool_.dns_changed_.notify();
        }
    } lease{*this};
    client_host_resolver resolver(worker_);
    stop_group cancellation(dns_stop_.token(), options.stop_token_);
    const operation_deadline timeout(options.timeout_.has_value() ? options.timeout_ : std::optional{config_.connect_timeout_});
    const auto revision = dns_revision_;
    auto pending = resolver.resolve(config_.dial_host_, config_.dial_port_, timeout, cancellation.token());
    client_resolve_result result;
    if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
        result = std::move(*immediate);
    } else {
        result = co_await std::move(std::get<task<client_resolve_result>>(pending));
    }
    if (cancellation.token().stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP DNS refresh cancelled");
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP DNS refresh timed out");
    }
    if (revision == dns_revision_) {
        install_addresses(std::move(result));
    }
}

void http_client_pool::schedule_dns_refresh() {
    if (!config_.dns_refresh_interval_ || closing_ || background_joined_) {
        return;
    }
    (void)worker_.schedule_timer(dns_refresh_timer_, deadline_after(config_.dns_refresh_interval_->value()),
        [this](worker_timer_outcome outcome) noexcept {
            if (outcome != worker_timer_outcome::expired || closing_) {
                return;
            }
            try {
                background_tasks_.spawn(background_refresh_dns());
            } catch (...) {
                report_unhandled_failure("HTTP DNS refresh", std::current_exception());
            }
        });
}

namespace {
[[nodiscard]] bool resolved_to(const client_resolve_result& result, const asio::ip::address& address) noexcept {
    if (result.literal_) {
        return result.literal_->address() == address;
    }
    return std::ranges::any_of(result.addresses_, [&address](const auto& entry) { return entry.endpoint().address() == address; });
}
}  // namespace

void http_client_pool::retire_unresolved(std::string_view host, std::uint16_t port, const client_resolve_result& result) noexcept {
    for (auto& connection : connections_) {
        if (connection && connection->connected_ && connection->transport_.endpoint().port_ == port &&
            http_ascii_equals_ignore_case(connection->transport_.endpoint().host_, host) &&
            !resolved_to(result, connection->transport_.peer_endpoint().address())) {
            connection->retire_addresses();
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& connection : http3_connections_) {
        if (connection == nullptr || connection->route().port_ != port ||
            !http_ascii_equals_ignore_case(connection->route().host_, host)) {
            continue;
        }
        if (const auto peer = connection->peer_address(); peer && !resolved_to(result, *peer)) {
            connection->retire_addresses();
        }
    }
#endif
}

task<void> http_client_pool::refresh_live_routes() {
    // Distinct DNS dial targets of the live connections; IP literals and
    // routes without a connection cost no query.
    std::vector<std::pair<std::string, std::uint16_t>> targets;
    const auto add = [&targets](std::string_view host, std::uint16_t port) {
        if (!client_host_needs_lookup(host)) {
            return;
        }
        for (const auto& target : targets) {
            if (target.second == port && http_ascii_equals_ignore_case(target.first, host)) {
                return;
            }
        }
        targets.emplace_back(std::string(host), port);
    };
    for (const auto& connection : connections_) {
        if (connection && (connection->connected_ || connection->users_ != 0)) {
            add(connection->transport_.endpoint().host_, connection->transport_.endpoint().port_);
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (const auto& connection : http3_connections_) {
        if (connection && (connection->connection_id() != 0 || connection->users_ != 0)) {
            add(connection->route().host_, connection->route().port_);
        }
    }
#endif
    for (const auto& [host, port] : targets) {
        // A worker running its bound of refreshes defers the rest to the next period.
        if (closing_ || !worker_limits_->try_begin_dns_refresh()) {
            co_return;
        }
        struct refresh_slot final {
            http_client_worker_limits& limits_;
            ~refresh_slot() {
                limits_.end_dns_refresh();
            }
        } slot{*worker_limits_};
        ++dns_refreshes_;
        client_host_resolver resolver(worker_);
        const operation_deadline timeout(config_.connect_timeout_);
        const auto revision = dns_revision_;
        auto pending = resolver.resolve(host, port, timeout, dns_stop_.token());
        client_resolve_result result;
        if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
            result = std::move(*immediate);
        } else {
            result = co_await std::move(std::get<task<client_resolve_result>>(pending));
        }
        if (closing_ || result.error_ || (!result.literal_ && result.addresses_.empty())) {
            continue;
        }
        if (host == config_.dial_host_ && port == config_.dial_port_) {
            if (revision == dns_revision_) {
                try {
                    install_addresses(std::move(result));
                } catch (const http_client_error&) {
                    // A snapshot the validator rejects leaves the previous one.
                }
            }
        } else {
            retire_unresolved(host, port, result);
        }
    }
}

task<void> http_client_pool::background_refresh_dns() {
    try {
        co_await refresh_live_routes();
    } catch (...) {
        if (!closing_) {
            report_unhandled_failure("HTTP DNS refresh", std::current_exception());
        }
    }
    schedule_dns_refresh();
}

bool http_client_pool::close_connection(std::uint64_t id) noexcept {
    if (id == 0) {
        return false;
    }
    for (auto& connection : connections_) {
        if (connection && connection->connection_id() == id && connection->connected_) {
            connection->close();
            return true;
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& connection : http3_connections_) {
        if (connection && connection->connection_id() == id) {
            connection->close();
            return true;
        }
    }
#endif
    return false;
}

http_client_pool::~http_client_pool() {
    close_now();
    worker_limits_->leave(*this);
    cancellation_target_->detach(*this);
}

void http_client_pool::close_now() noexcept {
    if (std::exchange(closing_, true)) {
        return;
    }
    dns_refresh_timer_.cancel();
    dns_stop_.request_stop();
    // Admission waiters observe closing_; connection shutdown aborts every
    // connect and exchange.
    admission_signal_.notify();
    reclaim_signal_.notify();
    for (auto& connection_value : connections_) {
        if (connection_value) {
            connection_value->shutdown();
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& connection_value : http3_connections_) {
        if (connection_value) {
            connection_value->shutdown();
        }
    }
#endif
}

task<void> http_client_pool::join() {
    if (background_joined_) {
        co_return;
    }
    background_joined_ = true;
    dns_refresh_timer_.cancel();
    worker_limits_->leave(*this);
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
    while (dns_active_ != 0) {
        co_await dns_changed_.wait();
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& connection_value : http3_connections_) {
        if (connection_value) {
            co_await connection_value->join();
        }
    }
#endif
    // Consumers own their response storage independently of transport. Retire
    // every client borrow, including completed responses, before its owners.
    response_memory_->detach_transport_bindings(*this);
    pushes_.clear();
    advertisements_.retire();
    response_memory_.reset();
    {
        decltype(connections_) retired_connections;
        retired_connections.swap(connections_);
#ifdef RUVIA_ENABLE_HTTP3
        decltype(http3_connections_) retired_http3;
        retired_http3.swap(http3_connections_);
        decltype(alternative_services_) retired_alternatives;
        retired_alternatives.swap(alternative_services_);
#endif
    }
    dns_addresses_ = {};
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_stats http_client_pool::stats() const noexcept {
    std::size_t idle = 0;
    for (const auto& connection_value : connections_) {
        idle += connection_value && connection_value->idle() ? 1U : 0U;
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (const auto& connection_value : http3_connections_) {
        idle += connection_value && connection_value->idle() ? 1U : 0U;
    }
#endif
    return {.buffered_requests_ = requests_buffered_,
        .in_flight_requests_ = requests_in_flight_,
        .completed_requests_ = completed_requests_,
        .failed_requests_ = failed_requests_,
        .plaintext_bytes_sent_ = wire_counters_.sent_,
        .plaintext_bytes_received_ = wire_counters_.received_,
        .idle_connections_with_pending_data_ = idle_connections_with_pending_data_,
        .open_connections_ = open_connections_,
        .idle_connections_ = idle,
        .dns_refreshes_ = dns_refreshes_,
        .dropped_advertisements_ = advertisements_.dropped(),
        .received_pushes_ = received_pushes_,
        .rejected_pushes_ = rejected_pushes_};
}

void http_client_pool::cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id == 0) {
        return;
    }
    for (auto* waiter = admission_waiters_; waiter != nullptr; waiter = waiter->next_) {
        if (waiter->cancellation_id_ == cancellation_id) {
            admission_signal_.notify();
            return;
        }
    }
    for (auto& connection_value : connections_) {
        if (connection_value && connection_value->cancel_by_id(cancellation_id)) {
            return;
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& connection_value : http3_connections_) {
        if (connection_value && connection_value->cancel_by_id(cancellation_id)) {
            return;
        }
    }
#endif
}

void http_client_pool::reprioritize(http_client_response_state& state_value, http_priority priority) {
    if (!worker_.is_current()) {
        std::terminate();
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (state_value.transport_ == http_client_response_transport::http2) {
        if (auto* connection_value = find_connection(state_value)) {
            connection_value->reprioritize(state_value, priority);
            return;
        }
    }
#endif
#ifdef RUVIA_ENABLE_HTTP3
    if (state_value.transport_ == http_client_response_transport::http3) {
        if (auto* connection_value = find_http3_connection(state_value)) {
            connection_value->reprioritize(state_value, priority);
            return;
        }
    }
#endif
    (void)state_value;
    (void)priority;
    throw http_client_error(
        make_error_code(http_client_errc::protocol_unavailable), "priority updates require HTTP/2 or HTTP/3");
}

std::optional<http_client_push> http_client_pool::next_push() {
    if (!worker_.is_current()) {
        throw std::logic_error("push requires its owner worker");
    }
    if (pushes_.empty()) {
        return std::nullopt;
    }
    auto result_value = std::move(pushes_.front());
    pushes_.pop_front();
    return result_value;
}

void http_client_pool::spawn_request(
    http_client_request_storage request, operation_options options, http_client_response_state& state_value) {
    state_value.buffered_limit_ = config_.max_response_bytes_;
    state_value.effective_connect_timeout_ = request.options_.timeouts_.connect_.resolve(config_.connect_timeout_);
    state_value.effective_read_timeout_ = request.options_.timeouts_.read_.resolve(config_.read_timeout_);
    state_value.effective_write_timeout_ = request.options_.timeouts_.write_.resolve(config_.write_timeout_);
    state_value.effective_acquire_timeout_ = request.options_.timeouts_.acquire_.resolve(config_.acquire_timeout_);
    state_value.decode_content_ = request.options_.decode_content_.value_or(config_.decode_content_);
    state_value.informational_callback_ = request.informational_;
    if (state_value.tunnel_) {
        request.bind_tunnel(*state_value.tunnel_);
    }
    if (state_value.upload_) {
        request.bind_upload(*state_value.upload_);
    }
    background_tasks_.spawn(execute_into(std::move(request), std::move(options), &state_value));
}

task<http_client_tunnel_result> http_client_pool::open_tunnel(
    http_client_request_storage request, http_client_tunnel_config config, operation_options options) {
    require_accepting();
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.tunnel_.emplace(state_value.memory_domain().worker(), config);
    state_value.tunnel_->udp_ = request.udp_tunnel();
    spawn_request(std::move(request), std::move(options), state_value);
    co_await state_value.await_head("CONNECT handshake failed");
    if (state_value.tunnel_->accepted_) {
        co_return http_client_tunnel_result(http_client_tunnel(std::move(response)));
    }
    co_return http_client_tunnel_result(std::move(response));
}

task<http_client_exchange> http_client_pool::open_request(
    http_client_request_storage request, http_client_upload_config upload, operation_options options) {
    require_accepting();
    http_client_response response(*this);
    auto& state_value = *response.state_;
    state_value.upload_.emplace(state_value.memory_domain().worker(), upload);
    state_value.upload_->content_released_ = upload.expectation_ == http_client_request_expectation::none;
    spawn_request(std::move(request), std::move(options), state_value);
    co_return http_client_exchange(std::move(response));
}

task<http_client_response> http_client_pool::execute(http_client_request_storage request, operation_options options) {
    require_accepting();
    http_client_response response(*this);
    auto& state_value = *response.state_;
    spawn_request(std::move(request), std::move(options), state_value);
    co_await state_value.await_head("HTTP client request failed before the response head");
    co_return response;
}

task<void> http_client_pool::execute_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    http_client_response keep_alive(state_value);
    try {
        co_await execute_request_into(std::move(request), std::move(options), state_value);
    } catch (...) {
        auto failure = std::current_exception();
        {
            try {
                std::rethrow_exception(failure);
            } catch (const http_client_error& error) {
                try {
                    throw http_client_error(error.code(), error.what(), error.status(),
                        state_value->connection_info_.id_ != 0 ? &state_value->connection_info_ : nullptr, state_value->request_written_);
                } catch (...) {
                    failure = std::current_exception();
                }
            } catch (...) {
            }
        }
        if (state_value->complete_ && state_value->upload_ && !state_value->failure_) {
            if (!state_value->upload_->failure_) {
                state_value->upload_->failure_ = failure;
            }
        } else {
            state_value->discard_failed_encoded_response();
            state_value->failure_ = failure;
        }
    }
    state_value->complete_ = true;
    if (auto* output = state_value->output()) {
        output->stop();
    }
    state_value->head_signal_.notify();
    state_value->data_signal_.notify();
}

void http_client_pool::require_accepting() const {
    // join() retires background_tasks_, which then rejects every spawn.
    if (background_joined_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client pool is retired", std::nullopt, nullptr, false);
    }
}

void http_client_pool::require_fixed_dial() const {
    if (config_.dial_host_.empty()) {
        throw std::logic_error("HTTP client alias without a fixed origin or dial host has no DNS snapshot");
    }
}

void http_client_pool::require_open() const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client pool is closed");
    }
}

http_client_connection* http_client_pool::find_connection(const http_client_response_state& state_value) noexcept {
    if (state_value.connection_index_ >= connections_.size()) {
        return nullptr;
    }
    auto* const connection_value = connections_[state_value.connection_index_].get();
    return connection_value != nullptr && connection_value->serial() == state_value.connection_serial_
               ? connection_value
               : nullptr;
}

void http_client_pool::admission_changed() noexcept {
    admission_signal_.notify();
}

void http_client_pool::connection_retired() noexcept {
    admission_signal_.notify();
    if (reclaimer_started_) {
        reclaim_signal_.notify();
    }
}

void http_client_pool::start_reclaimer() {
    if (!reclaimer_started_) {
        worker_limits_->join(*this);
        background_tasks_.spawn(run_reclaimer());
        reclaimer_started_ = true;
    }
}

task<void> http_client_pool::run_reclaimer() {
    while (!closing_) {
        co_await reclaim_signal_.wait();
        if (!closing_) {
            reclaim_connections();
        }
    }
}

void http_client_pool::reclaim_connections() noexcept {
    // Runs on its own worker turn, after the coroutine that retired a
    // connection unwound: reclaimable() owners have no borrower left.
    bool reclaimed = false;
    for (auto& owner : connections_) {
        if (owner && owner->reclaimable()) {
            owner.reset();
            --open_connections_;
            reclaimed = true;
        }
    }
    while (!connections_.empty() && !connections_.back()) {
        connections_.pop_back();
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& owner : http3_connections_) {
        if (owner && owner->reclaimable()) {
            owner.reset();
            --open_connections_;
            reclaimed = true;
        }
    }
    while (!http3_connections_.empty() && !http3_connections_.back()) {
        http3_connections_.pop_back();
    }
#endif
    if (reclaimed) {
        admission_signal_.notify();
    }
}

void http_client_pool::mark_idle(std::uint64_t& idle_order, bool multiplexed) noexcept {
    if (idle_order == 0) {
        ++(multiplexed ? idle_multiplexed_ : idle_http1_);
    }
    idle_order = worker_limits_->next_idle_order();
    enforce_idle_limits();
}

std::size_t http_client_pool::idle_count(bool multiplexed) const noexcept {
    return multiplexed ? idle_multiplexed_ : idle_http1_;
}

std::optional<std::uint64_t> http_client_pool::least_recent_idle(bool multiplexed) const noexcept {
    std::optional<std::uint64_t> oldest;
    for (const auto& owner : connections_) {
        if (owner && owner->users_ == 0 && owner->idle() &&
            (owner->protocol_ != http_client_connection::protocol_type::http1) == multiplexed &&
            (!oldest || owner->idle_order_ < *oldest)) {
            oldest = owner->idle_order_;
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (const auto& owner : http3_connections_) {
        if (multiplexed && owner && owner->users_ == 0 && owner->idle() && (!oldest || owner->idle_order_ < *oldest)) {
            oldest = owner->idle_order_;
        }
    }
#endif
    return oldest;
}

void http_client_pool::enforce_idle_limits() noexcept {
    const auto& limits = config_.connection_limits_;
    if (idle_http1_ > limits.max_idle_http1_ || idle_multiplexed_ > limits.max_idle_multiplexed_) {
        recount_idle();
        while (idle_http1_ > limits.max_idle_http1_ && close_least_recent_idle(false)) {
        }
        while (idle_multiplexed_ > limits.max_idle_multiplexed_ && close_least_recent_idle(true)) {
        }
    }
    worker_limits_->enforce_idle_limits();
}

void http_client_pool::recount_idle() noexcept {
    // The counters are upper bounds: a connection also stops idling by
    // closing on its own.
    idle_http1_ = 0;
    idle_multiplexed_ = 0;
    for (auto& owner : connections_) {
        if (!owner || owner->idle_order_ == 0) {
            continue;
        }
        if (!owner->idle()) {
            owner->idle_order_ = 0;
            continue;
        }
        ++(owner->protocol_ == http_client_connection::protocol_type::http1 ? idle_http1_ : idle_multiplexed_);
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (auto& owner : http3_connections_) {
        if (!owner || owner->idle_order_ == 0) {
            continue;
        }
        if (!owner->idle()) {
            owner->idle_order_ = 0;
            continue;
        }
        ++idle_multiplexed_;
    }
#endif
}

bool http_client_pool::close_least_recent_idle(std::optional<bool> multiplexed) noexcept {
    http_client_connection* oldest = nullptr;
    std::uint64_t oldest_order = (std::numeric_limits<std::uint64_t>::max)();
    for (auto& owner : connections_) {
        if (!owner || owner->users_ != 0 || !owner->idle()) {
            continue;
        }
        const bool owner_multiplexed = owner->protocol_ != http_client_connection::protocol_type::http1;
        if ((!multiplexed || *multiplexed == owner_multiplexed) && owner->idle_order_ < oldest_order) {
            oldest = owner.get();
            oldest_order = owner->idle_order_;
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    http3_client_connection* oldest_http3 = nullptr;
    if (!multiplexed || *multiplexed) {
        for (auto& owner : http3_connections_) {
            if (owner && owner->users_ == 0 && owner->idle() && owner->idle_order_ < oldest_order) {
                oldest = nullptr;
                oldest_http3 = owner.get();
                oldest_order = owner->idle_order_;
            }
        }
    }
    if (oldest_http3 != nullptr) {
        if (std::exchange(oldest_http3->idle_order_, 0) != 0 && idle_multiplexed_ != 0) {
            --idle_multiplexed_;
        }
        oldest_http3->close();
        return true;
    }
#endif
    if (oldest == nullptr) {
        return false;
    }
    auto& counter = oldest->protocol_ == http_client_connection::protocol_type::http1 ? idle_http1_ : idle_multiplexed_;
    if (std::exchange(oldest->idle_order_, 0) != 0 && counter != 0) {
        --counter;
    }
    oldest->close_idle();
    return true;
}

bool http_client_pool::admits_new_connection() noexcept {
    const auto& max_open = config_.connection_limits_.max_open_;
    if (!max_open || open_connections_ < *max_open) {
        return true;
    }
    // An owner nothing uses that no longer idles is closing; its reclamation
    // frees a position, so no further idle connection is closed for it.
    for (const auto& owner : connections_) {
        if (owner && owner->users_ == 0 && !owner->idle()) {
            return false;
        }
    }
#ifdef RUVIA_ENABLE_HTTP3
    for (const auto& owner : http3_connections_) {
        if (owner && owner->users_ == 0 && !owner->idle()) {
            return false;
        }
    }
#endif
    (void)close_least_recent_idle(std::nullopt);
    return false;
}

void http_client_pool::throw_if_admission_ended(const stop_token& stop, const http_client_response_state& state_value) const {
    require_open();
    throw_if_abandoned(state_value);
    if (stop.stop_requested()) {
        throw_http_client_wait_failure(worker_wait_status::cancelled, "HTTP client connection pool acquire");
    }
}

task<void> http_client_pool::wait_for_admission(const operation_deadline& acquire_timeout) {
    // A connection available at once is taken even past the bound; only
    // waiting is bounded.
    if (acquire_timeout.expired()) {
        throw_http_client_wait_failure(worker_wait_status::timed_out, "HTTP client connection pool acquire");
    }
    worker_timer_registration deadline_timer;
    if (const auto deadline = acquire_timeout.deadline()) {
        (void)worker_.schedule_timer(deadline_timer, *deadline, [this](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                admission_signal_.notify();
            }
        });
    }
    co_await admission_signal_.wait();
}

client_alpn http_client_pool::tcp_offer(const http_client_request_storage& request) const noexcept {
    const auto& versions = request.route().versions_;
    // HTTP/1.1 Upgrade never runs over h2 (RFC 9113 section 8.6).
    if (request.is_upgrade() || !versions.http2_) {
        return client_alpn::http11;
    }
    if (!versions.http1_) {
        return client_alpn::http2;
    }
    // Plaintext offers nothing to negotiate: HTTP/1.1 is preferred to h2c.
    return config_.scheme_ == http_scheme::http || prefers_http11_ ? client_alpn::http11 : client_alpn::negotiate;
}

task<http_client_connection*> http_client_pool::acquire_tcp_connection(const http_client_request_storage& request,
    const operation_deadline& acquire_timeout, stop_token stop, http_client_response_state& state_value,
    request_admission& admission) {
    const auto& route = request.route();
    const auto offer = tcp_offer(request);
    const bool fresh = request.options_.fresh_connection_;
#ifdef RUVIA_ENABLE_HTTP2
    const bool may_multiplex = offer == client_alpn::http2 || offer == client_alpn::negotiate;
#endif
    admission_waiter waiter(*this, state_value, stop);
    for (;;) {
        throw_if_admission_ended(stop, state_value);
        http_client_connection* selected = nullptr;
        http_client_connection* reconnectable = nullptr;
        bool negotiating = false;
        for (auto& owner : connections_) {
            auto* const candidate = owner.get();
            if (fresh || candidate == nullptr || !candidate->serves(route.dial_host_, route.dial_port_, route.server_name_, offer)) {
                continue;
            }
            if (candidate->users_ == 0 && candidate->idle_unconnected()) {
                reconnectable = reconnectable != nullptr ? reconnectable : candidate;
                continue;
            }
#ifdef RUVIA_ENABLE_HTTP2
            if (candidate->protocol_ == http_client_connection::protocol_type::http2) {
                if (candidate->available_http2_request_streams() != 0) {
                    selected = candidate;
                    break;
                }
                continue;
            }
            if (candidate->users_ != 0) {
                // A connection of this route still negotiating may offer h2.
                negotiating = negotiating || (may_multiplex && candidate->protocol_ == http_client_connection::protocol_type::unknown);
                continue;
            }
#else
            if (candidate->users_ != 0) {
                continue;
            }
#endif
            // The most recently used idle HTTP/1 connection, so the others age out.
            if (candidate->protocol_ == http_client_connection::protocol_type::http1 && candidate->connected_ &&
                candidate->http1_operations_ == 0 && (selected == nullptr || candidate->idle_order_ > selected->idle_order_)) {
                selected = candidate;
            }
        }
        if (selected == nullptr && !negotiating) {
            selected = reconnectable;
        }
        if (selected == nullptr && !negotiating && admits_new_connection()) {
            start_reclaimer();
            const auto index = free_connection_position(connections_);
            connections_[index] = std::make_unique<http_client_connection>(*this, index,
                ++next_connection_serial_, route.dial_host_, route.dial_port_, route.server_name_, route.alias_transport_,
                offer);
            ++open_connections_;
            selected = connections_[index].get();
            selected->fresh_ = fresh;
        }
        if (selected != nullptr) {
            if (selected->idle_order_ != 0) {
                auto& counter = selected->protocol_ == http_client_connection::protocol_type::http1 ? idle_http1_ : idle_multiplexed_;
                counter -= counter != 0 ? 1U : 0U;
                selected->idle_order_ = 0;
            }
            ++selected->users_;
            admission.admit();
            co_return selected;
        }
        co_await wait_for_admission(acquire_timeout);
    }
}

void http_client_pool::release_tcp_connection(http_client_connection& connection_value) noexcept {
    if (connection_value.users_ == 0) {
        std::terminate();
    }
    --connection_value.users_;
    if (connection_value.users_ == 0 && connection_value.idle()) {
        mark_idle(connection_value.idle_order_,
            connection_value.protocol_ != http_client_connection::protocol_type::http1);
    }
    if (connection_value.reclaimable()) {
        connection_retired();
    } else {
        admission_changed();
    }
}

task<void> http_client_pool::execute_tcp_request(const http_client_request_storage& request,
    const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop,
    http_client_response_state& state_value, request_admission& admission) {
    auto* const connection_value = co_await acquire_tcp_connection(request, acquire_timeout, stop, state_value, admission);
    struct connection_use final {
        http_client_pool& pool_;
        http_client_connection& connection_;
        bool fresh_;
        ~connection_use() {
            if (fresh_) {
                connection_.close();
            }
            pool_.release_tcp_connection(connection_);
        }
    } use{*this, *connection_value, request.options_.fresh_connection_};
    state_value.connection_index_ = connection_value->index();
    state_value.connection_serial_ = connection_value->serial();
    co_await execute_tcp(*connection_value, request, timeout, acquire_timeout, std::move(stop), state_value);
}

task<void> http_client_pool::execute_request_into(
    http_client_request_storage request, operation_options options, http_client_response_state* state_value) {
    try {
        throw_if_abandoned(*state_value);
        // The storage is at its final address in this frame.
        request.resolve_route(config_);
    } catch (...) {
        ++failed_requests_;
        throw;
    }
    // The route is borrowed by response heads published during this exchange.
    struct route_binding final {
        http_client_response_state& state_;
        ~route_binding() {
            state_.route_ = nullptr;
        }
    } route_binding_value{*state_value};
    state_value->route_ = &request.route();
    const operation_deadline timeout(options.timeout_.has_value() ? options.timeout_ : config_.request_timeout_);
    const std::array stop_tokens{request.host_stop(), options.stop_token_, dns_stop_.token()};
    stop_group cancellation(stop_tokens);
    options.stop_token_ = cancellation.token();
    state_value->informational_stop_ = options.stop_token_;
    const auto acquire_timeout = timeout.constrained_by(state_value->effective_acquire_timeout_);
    if (requests_buffered_ >= config_.max_buffered_requests_) {
        ++failed_requests_;
        throw http_client_error(make_error_code(http_client_errc::queue_full), "HTTP client request buffer is full");
    }
    ++requests_buffered_;
    request_admission admission{*this};
    try {
        require_open();
        if (request.is_upgrade() && !request.route().versions_.http1_) {
            throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP Upgrade requires HTTP/1.1");
        }
#ifdef RUVIA_ENABLE_HTTP3
        if (co_await execute_http3(request, timeout, acquire_timeout, options.stop_token_, *state_value, admission)) {
            ++completed_requests_;
            co_return;
        }
#endif
        co_await execute_tcp_request(request, timeout, acquire_timeout, options.stop_token_, *state_value, admission);
        ++completed_requests_;
    } catch (...) {
        ++failed_requests_;
        throw;
    }
}

task<void> http_client_pool::execute_tcp(http_client_connection& connection_value,
    const http_client_request_storage& request, const operation_deadline& timeout,
    const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    std::optional<std::uint64_t> connected_http1_generation;
#ifdef RUVIA_ENABLE_HTTP2
    // A cancelled HTTP/1 producer still owns its SSL stream until its
    // serialized exchange unwinds. Join that exchange before reconnecting,
    // including when negotiated HTTP/2-capacity leases share the pool slot.
    if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
        // This lease shares a multiplexed session with background drivers and
        // possibly other requests; a request-local failure must not discard it.
        // A session that ended and was replaced by an HTTP/1.1 connection has
        // sent nothing yet: the request continues as an HTTP/1 exchange below.
        if (co_await connection_value.execute_http2(request, timeout, acquire_timeout, stop_token_value, state_value)) {
            co_return;
        }
        connected_http1_generation = connection_value.generation_;
    }
#endif
    if (co_await execute_http1(connection_value, request, timeout, acquire_timeout, stop_token_value, state_value,
            connected_http1_generation)) {
        co_return;
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (co_await connection_value.execute_http2(request, timeout, acquire_timeout, stop_token_value, state_value)) {
        co_return;
    }
    // The HTTP/2 session negotiated above ended before this request could use
    // it and its replacement speaks HTTP/1.1. One protocol change per request,
    // unless the origin required HTTP/1.1: then the replacement offered no h2
    // and the change is final.
    if (!prefers_http11() ||
        !co_await execute_http1(connection_value, request, timeout, acquire_timeout, stop_token_value, state_value,
            connection_value.generation_)) {
        throw http_client_error(
            make_error_code(http_client_errc::protocol_unavailable), "upstream renegotiated the protocol repeatedly");
    }
#endif
}

task<bool> http_client_pool::execute_http1(http_client_connection& connection_value,
    const http_client_request_storage& request, const operation_deadline& timeout,
    const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value,
    std::optional<std::uint64_t> connected_http1_generation) {
    // A negotiated HTTP/1 connection can have several operations that already
    // hold outer HTTP/2-capacity slots. Waiting for this connection's single
    // exchange slot does not own its socket: a timeout/cancellation here must
    // not close the exchange currently using it.
    const bool must_buffer = connection_value.http1_operations_ != 0;
    if (must_buffer && requests_buffered_ >= config_.max_buffered_requests_) {
        throw http_client_error(make_error_code(http_client_errc::queue_full), "HTTP client request buffer is full");
    }
    ++connection_value.http1_operations_;
    if (must_buffer) {
        --requests_in_flight_;
        ++requests_buffered_;
    }
    struct http1_operation final {
        http_client_pool& pool_;
        http_client_connection& connection_;
        bool buffered_;
        ~http1_operation() {
            if (buffered_) {
                --pool_.requests_buffered_;
                ++pool_.requests_in_flight_;
            }
            if (connection_.http1_operations_ == 0) {
                std::terminate();
            }
            --connection_.http1_operations_;
        }
    } operation{*this, connection_value, must_buffer};
    const auto acquired =
        co_await connection_value.http1_scheduler_.acquire({.timeout_ = acquire_timeout.timeout(), .stop_token_ = stop_token_value});
    if (!acquired.has_value()) {
        throw_http_client_wait_failure(acquired.status(), "HTTP/1 connection acquire");
    }
    const auto slot = acquired.value();
    if (operation.buffered_) {
        --requests_buffered_;
        ++requests_in_flight_;
        operation.buffered_ = false;
    }
    struct http1_release final {
        pool_lease_scheduler& scheduler_;
        std::size_t slot_;
        ~http1_release() {
            scheduler_.release(slot_);
        }
    } release{connection_value.http1_scheduler_, slot};

    throw_if_abandoned(state_value);
    if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
        co_return false;
    }
    // A previous HTTP/1 exchange may have closed this connection while this
    // request waited for the serial exchange slot. Reconnect only after
    // acquiring that slot, so the connection cannot be replaced underneath
    // an active HTTP/1 exchange. Every final failure below closes the
    // connection before the slot is released; pool handoff resumes the next
    // waiter synchronously.
    try {
        // The HTTP/2 fallback already connected and negotiated HTTP/1.
        // Do not idle-probe this fresh TLS stream before its first response:
        // session tickets can look like unsolicited response bytes. A connect
        // or exchange while waiting for this slot invalidates the snapshot.
        bool reused_http1 = false;
        if (!connection_value.connected_ || connected_http1_generation != connection_value.generation_) {
            reused_http1 =
                co_await connection_value.ensure_connected(timeout, acquire_timeout, stop_token_value, state_value);
        }
        if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
            co_return false;
        }
        bool retried = false;
        for (;;) {
            std::exception_ptr failure;
            try {
                connection_value.capture_connection_info(state_value, reused_http1);
                http1_client_exchange exchange(*this, connection_value);
                co_await exchange.run(request, timeout, stop_token_value, state_value);
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure == nullptr) {
                connection_value.begin_idle();
                co_return true;
            }
            if (retried || !reused_http1 ||
                !can_retry_reused_connection_request(http_client_failure_code(failure), request, state_value)) {
                std::rethrow_exception(failure);
            }
            connection_value.close();
            connection_value.retain_idle_buffers();
            retried = true;
            reused_http1 = false;
            (void)co_await connection_value.ensure_connected(timeout, acquire_timeout, stop_token_value, state_value);
            if (connection_value.protocol_ == http_client_connection::protocol_type::http2) {
                co_return false;
            }
        }
    } catch (...) {
        if (connection_value.protocol_ != http_client_connection::protocol_type::http2) {
            connection_value.close();
            // exchange.run() has joined its child operations, so no I/O
            // still borrows these failed exchange buffers.
            connection_value.retain_idle_buffers();
        }
        throw;
    }
}

void http_client_pool::abandon_response(http_client_response_state& state_value, bool malformed) noexcept {
    if (state_value.abandoned_) {
        return;
    }
    state_value.abandoned_ = true;
    state_value.request_informational_stop();
    switch (state_value.transport_) {
        case http_client_response_transport::http2:
#ifdef RUVIA_ENABLE_HTTP2
            if (auto* const found = find_connection(state_value)) {
                const auto error = malformed ? http2_error_code::protocol_error : http2_error_code::cancel;
                auto& conn = *found;
                if (conn.generation_ == state_value.connection_generation_ &&
                    error == http2_error_code::protocol_error && state_value.stream_id_ != 0) {
                    conn.http2_.submit_reset(static_cast<std::uint32_t>(state_value.stream_id_), error);
                }
                if (state_value.request_id_ != 0) {
                    conn.cancel_http2_stream(state_value.request_id_, client_abort_reason::cancelled, error);
                }
                // A retry may be waiting for the old session without a
                // registered stream. Wake it to observe this abandonment.
                conn.http2_.state_signal().notify();
            }
#endif
            break;
        case http_client_response_transport::http3:
#ifdef RUVIA_ENABLE_HTTP3
            if (auto* const conn = find_http3_connection(state_value)) {
                conn->abandon(state_value, malformed);
            }
#endif
            break;
        case http_client_response_transport::unassigned:
        case http_client_response_transport::http1:
            if (state_value.cancellation_id_ != 0) {
                auto* const conn = find_connection(state_value);
                if (conn == nullptr || !conn->cancel_by_id(state_value.cancellation_id_)) {
                    cancel_operation_by_id(state_value.cancellation_id_);
                }
            } else if (auto* const conn = find_connection(state_value); conn != nullptr && conn->http1_response_ == &state_value) {
                conn->close();
            }
            break;
    }
#if !defined(RUVIA_ENABLE_HTTP2) && !defined(RUVIA_ENABLE_HTTP3)
    (void)malformed;
#endif
    if (auto* output = state_value.output()) {
        output->stop();
    }
    state_value.space_signal_.notify();
}

void http_client_pool::release_response_data(http_client_response_state& state_value) noexcept {
#ifdef RUVIA_ENABLE_HTTP3
    if (state_value.transport_ == http_client_response_transport::http3) {
        if (auto* const conn = find_http3_connection(state_value)) {
            conn->release_data(state_value);
        } else {
            state_value.http3_data_credit_.reset();
        }
        return;
    }
#endif
#ifdef RUVIA_ENABLE_HTTP2
    if (auto* const conn = find_connection(state_value)) {
        conn->release_response_data(state_value);
    } else {
        state_value.http2_data_credit_.reset();
    }
#else
    (void)state_value;
#endif
}

void http_client_pool::observe_alt_svc(const http_client_route& route, std::span<const http_header> fields) {
#ifdef RUVIA_ENABLE_HTTP3
    // Only TLS-authenticated responses, for requests that may use HTTP/3 beside
    // a TCP version, advertise alternatives. Entries are keyed by the actual
    // route, so an alias without a fixed origin learns them per destination.
    if (config_.scheme_ != http_scheme::https || (!config_.versions_.http3_ && !route.versions_.http3_) ||
        (!route.versions_.http1_ && !route.versions_.http2_)) {
        return;
    }
    const auto names_route = [&route](const alternative_service& service) noexcept {
        return service.route_port_ == route.dial_port_ && http_ascii_equals_ignore_case(service.route_host_, route.dial_host_) &&
               http_ascii_equals_ignore_case(service.server_name_, route.server_name_);
    };
    // The route's failed alternatives stay unavailable across a re-advertisement.
    std::vector<alternative_service> previous;
    bool first = true;
    for (const auto& field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "alt-svc")) {
            continue;
        }
        if (first) {
            for (const auto& service : alternative_services_) {
                if (names_route(service)) {
                    previous.push_back(service);
                }
            }
            std::erase_if(alternative_services_, names_route);
            first = false;
        }
        auto value = http_trim_ows(field.value());
        if (value == "clear") {
            std::erase_if(alternative_services_, names_route);
            return;
        }
        http_visit_comma_separated_quoted_field_items(value, [&](std::string_view member) {
            if (member.empty()) {
                return true;
            }
            const auto first_semi = http_find_unquoted_delimiter(member, 0, ';');
            auto authority = http_trim_ows(member.substr(0, first_semi));
            auto params_tail = first_semi < member.size() ? member.substr(first_semi + 1) : std::string_view{};
            const auto equals = authority.find('=');
            if (equals == std::string_view::npos || http_trim_ows(authority.substr(0, equals)) != "h3") {
                return true;
            }
            authority = http_trim_ows(authority.substr(equals + 1));
            if (authority.size() < 4 || authority.front() != '"' || authority.back() != '"') {
                return true;
            }
            authority.remove_prefix(1);
            authority.remove_suffix(1);
            if (authority.find_first_of("\\\" \t\r\n/@?#") != std::string_view::npos) {
                return true;
            }
            std::string_view host;
            std::uint16_t port = 0;
            if (!authority.empty() && authority.front() == ':') {
                const auto port_text = authority.substr(1);
                unsigned parsed_port = 0;
                const auto parsed = std::from_chars(
                    port_text.data(), port_text.data() + port_text.size(), parsed_port);
                if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
                    parsed_port == 0 || parsed_port > 65535) {
                    return true;
                }
                port = static_cast<std::uint16_t>(parsed_port);
                host = route.dial_host_;
            } else {
                const auto parsed_authority = parse_http_authority(authority);
                if (!parsed_authority || parsed_authority->port_kind() != http_authority_port_kind::value ||
                    *parsed_authority->port() == 0) {
                    return true;
                }
                port = *parsed_authority->port();
                host = parsed_authority->host();
                if (!host.empty() && host.front() == '[' && host.back() == ']') {
                    host.remove_prefix(1);
                    host.remove_suffix(1);
                }
                if (host.empty()) {
                    host = route.dial_host_;
                }
            }
            if (host.size() > 253) {
                return true;
            }
            std::uint64_t age = 86400;
            bool valid = true;
            bool age_seen = false;
            std::size_t param_start = 0;
            while (param_start < params_tail.size()) {
                const auto param_end = param_start + http_find_unquoted_delimiter(params_tail.substr(param_start), 0, ';');
                const auto parameter = http_trim_ows(params_tail.substr(param_start, param_end - param_start));
                param_start = param_end + 1;
                const auto separator = parameter.find('=');
                if (separator == std::string_view::npos) {
                    continue;
                }
                if (!http_ascii_equals_ignore_case(http_trim_ows(parameter.substr(0, separator)), "ma")) {
                    continue;
                }
                if (age_seen) {
                    valid = false;
                    break;
                }
                age_seen = true;
                auto digits = http_trim_ows(parameter.substr(separator + 1));
                if (digits.size() >= 2 && digits.front() == '"' && digits.back() == '"') {
                    digits.remove_prefix(1);
                    digits.remove_suffix(1);
                }
                if (digits.empty()) {
                    valid = false;
                    break;
                }
                const auto number = std::from_chars(digits.data(), digits.data() + digits.size(), age);
                if (number.ec != std::errc{} || number.ptr != digits.data() + digits.size()) {
                    valid = false;
                    break;
                }
            }
            if (!valid || age == 0) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::steady_clock::time_point::max)() - now).count();
            const auto seconds = (std::min)(age, static_cast<std::uint64_t>(maximum));
            if (alternative_services_.size() == config_.http3_.max_alt_svc_entries_) {
                alternative_services_.erase(std::ranges::min_element(alternative_services_, {}, &alternative_service::used_));
            }
            alternative_service entry{.route_host_ = std::string(route.dial_host_),
                .route_port_ = route.dial_port_,
                .server_name_ = std::string(route.server_name_),
                .host_ = std::string(host),
                .port_ = port,
                .expires_ = now + std::chrono::seconds(seconds),
                .broken_until_ = {},
                .used_ = ++alternative_clock_};
            for (const auto& failed : previous) {
                if (failed.port_ == port && http_ascii_equals_ignore_case(failed.host_, host)) {
                    entry.broken_until_ = failed.broken_until_;
                }
            }
            alternative_services_.push_back(std::move(entry));
            return true;
        });
    }
#else
    (void)route;
    (void)fields;
#endif
}

#ifdef RUVIA_ENABLE_HTTP3
http3_client_connection* http_client_pool::find_http3_connection(const http_client_response_state& state_value) noexcept {
    if (state_value.connection_index_ >= http3_connections_.size()) {
        return nullptr;
    }
    auto* const connection_value = http3_connections_[state_value.connection_index_].get();
    return connection_value != nullptr && connection_value->serial() == state_value.connection_serial_
               ? connection_value
               : nullptr;
}

void http_client_pool::release_http3_connection(http3_client_connection& connection_value) noexcept {
    if (connection_value.users_ == 0) {
        std::terminate();
    }
    --connection_value.users_;
    if (connection_value.users_ == 0 && connection_value.idle()) {
        mark_idle(connection_value.idle_order_, true);
    }
    if (connection_value.reclaimable()) {
        connection_retired();
    } else {
        admission_changed();
    }
}

task<bool> http_client_pool::execute_http3(const http_client_request_storage& request,
    const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop,
    http_client_response_state& state, request_admission& admission) {
    require_open();
    const auto& route = request.route();
    const auto& versions = route.versions_;
    if (!versions.http3_) {
        co_return false;
    }
    // HTTP/3 alone dials the route; beside a TCP version it follows only an
    // Alt-Svc this route advertised, and Upgrade stays on HTTP/1.1.
    const bool required = !versions.http1_ && !versions.http2_;
    // Another response can replace the cache while DNS or the handshake waits,
    // so the selected endpoint is owned across suspension. TLS verifies the
    // route's server name, never the advertised transport name.
    std::string host;
    std::uint16_t port = 0;
    if (required) {
        host.assign(route.dial_host_);
        port = route.dial_port_;
    } else {
        if (request.is_upgrade()) {
            co_return false;
        }
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(alternative_services_, [now](const alternative_service& service) { return service.expires_ <= now; });
        auto* const alternative = find_alternative_service(route, now);
        if (alternative == nullptr) {
            co_return false;
        }
        alternative->used_ = ++alternative_clock_;
        host.assign(alternative->host_);
        port = alternative->port_;
    }
    const bool fresh = request.options_.fresh_connection_;
    http3_client_connection* selected = nullptr;
    {
        admission_waiter waiter(*this, state, stop);
        for (;;) {
            throw_if_admission_ended(stop, state);
            for (auto& owner : http3_connections_) {
                if (!fresh && owner && owner->serves(host, port, route.server_name_) &&
                    (owner->available() != 0 || owner->connecting())) {
                    selected = owner.get();
                    break;
                }
            }
            if (selected == nullptr && admits_new_connection()) {
                start_reclaimer();
                if (!http3_tls_) {
                    http3_tls_ = std::make_unique<http3_client_tls>(*this);
                }
                const auto index = free_connection_position(http3_connections_);
                http3_connections_[index] = std::make_unique<http3_client_connection>(*this,
                    *http3_tls_, index, ++next_connection_serial_, host, port, route.server_name_);
                ++open_connections_;
                selected = http3_connections_[index].get();
                selected->fresh_ = fresh;
            }
            if (selected != nullptr) {
                break;
            }
            co_await wait_for_admission(acquire_timeout);
        }
    }
    if (selected->idle_order_ != 0) {
        idle_multiplexed_ -= idle_multiplexed_ != 0 ? 1U : 0U;
        selected->idle_order_ = 0;
    }
    ++selected->users_;
    admission.admit();
    struct connection_use final {
        http_client_pool& pool_;
        http3_client_connection& connection_;
        bool fresh_;
        ~connection_use() {
            if (fresh_) {
                connection_.close();
            }
            pool_.release_http3_connection(connection_);
        }
    } use{*this, *selected, fresh};
    const auto used = co_await selected->execute(request, !required, timeout, acquire_timeout, stop, state);
    if (!used) {
        // RFC 7838 section 2.4: the request falls back to TCP, and the failed
        // alternative is not tried again for a while.
        for (auto& service : alternative_services_) {
            if (service.port_ == port && http_ascii_equals_ignore_case(service.host_, host) && service.route_port_ == route.dial_port_ &&
                http_ascii_equals_ignore_case(service.route_host_, route.dial_host_) &&
                http_ascii_equals_ignore_case(service.server_name_, route.server_name_)) {
                service.broken_until_ = std::chrono::steady_clock::now() + alternative_service_retry_delay;
            }
        }
    }
    co_return used;
}

http_client_pool::alternative_service* http_client_pool::find_alternative_service(
    const http_client_route& route, std::chrono::steady_clock::time_point now) noexcept {
    for (auto& service : alternative_services_) {
        if (service.broken_until_ <= now && service.route_port_ == route.dial_port_ &&
            http_ascii_equals_ignore_case(service.route_host_, route.dial_host_) &&
            http_ascii_equals_ignore_case(service.server_name_, route.server_name_)) {
            return &service;
        }
    }
    return nullptr;
}

task<std::optional<http_tunnel_input>> http_client_pool::read_http3_tunnel_input(
    http_client_response_state& state, operation_options operation) {
    auto lane = claim_operation_lane(state.body_operation_active_, "HTTP/3 tunnel input is already active");
    const http_client_operation_interruption interruption(state, http_client_operation_lane::read, operation);
    for (;;) {
        state.release_consumed_body_prefix();
        if (state.offset_ < state.buffered_.size()) {
            http_tunnel_input result{std::string(std::string_view(state.buffered_).substr(state.offset_)), false};
            state.offset_ = state.buffered_.size();
            co_return result;
        }
        if (!state.pending_.empty()) {
            http_tunnel_input result{{}, false};
            result.bytes_.swap(state.pending_);
            release_response_data(state);
            co_return result;
        }
        if (state.http3_datagrams_ && !state.http3_datagrams_->empty()) {
            http_tunnel_input result{std::move(state.http3_datagrams_->front()), true};
            state.http3_datagram_bytes_ -= result.bytes_.size();
            state.http3_datagrams_->pop_front();
            co_return result;
        }
        state.rethrow_response_failure();
        if (state.pool_closed_ || state.abandoned_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 tunnel input closed");
        }
        if (state.receive_complete()) {
            co_return std::nullopt;
        }
        state.throw_if_read_interrupted();
        co_await state.data_signal_.wait();
    }
}

task<bool> http_client_pool::send_http3_datagram(
    http_client_response_state& state, std::string_view payload, operation_options operation) {
    auto* const connection_value = state.tunnel_ && state.transport_ == http_client_response_transport::http3
                                       ? find_http3_connection(state)
                                       : nullptr;
    if (connection_value == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP/3 tunnel output closed");
    }
    auto lane = claim_operation_lane(state.tunnel_->output_.operation_active_, "HTTP/3 tunnel output is already active");
    const http_client_operation_interruption interruption(state, http_client_operation_lane::output, operation);
    try {
        const bool sent = co_await connection_value->send_datagram(state, std::as_bytes(std::span(payload.data(), payload.size())));
        state.throw_if_output_interrupted();
        co_return sent;
    } catch (...) {
        state.throw_if_output_interrupted();
        throw;
    }
}
#endif

task<void> http_client_pool::migrate_http3(std::string local_address, std::uint16_t port,
    operation_options options, stop_token host_stop) {
#ifdef RUVIA_ENABLE_HTTP3
    require_accepting();
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client pool is closed");
    }
    const auto timeout = operation_deadline(config_.connect_timeout_).constrained_by(options.timeout_);
    stop_group cancellation(host_stop, options.stop_token_);
    options.stop_token_ = cancellation.token();
    if (options.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP/3 migration cancelled");
    }
    // Connections may be created or reclaimed while one migrates: each is
    // found again by position and serial.
    std::vector<std::pair<std::size_t, std::uint64_t>> active;
    for (std::size_t index = 0; index != http3_connections_.size(); ++index) {
        if (http3_connections_[index] && http3_connections_[index]->active()) {
            active.emplace_back(index, http3_connections_[index]->serial());
        }
    }
    bool migrated = false;
    for (const auto& [index, serial] : active) {
        if (index < http3_connections_.size() && http3_connections_[index] &&
            http3_connections_[index]->serial() == serial && http3_connections_[index]->active()) {
            co_await http3_connections_[index]->migrate(local_address, port, timeout, options.stop_token_);
            migrated = true;
        }
    }
    if (!migrated) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "no active HTTP/3 connection to migrate");
    }
#else
    (void)local_address;
    (void)port;
    (void)options;
    (void)host_stop;
    throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 is disabled");
    co_return;
#endif
}
}  // namespace ruvia::detail

namespace ruvia {
http_client_push::http_client_push(http_client_response response) noexcept
    : response_(std::move(response)) {}
http_client_push::http_client_push(http_client_push&& other) noexcept
    : response_(std::move(other.response_)) {}
http_client_push& http_client_push::operator=(http_client_push&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
http_client_push::~http_client_push() {
    release();
}
void http_client_push::release() noexcept {
    if (auto* state_value = response_.state_; state_value != nullptr) {
        if (!state_value->memory_domain().worker().is_current()) {
            std::terminate();
        }
        state_value->push_response_.release(response_);
    }
    response_.release();
}
const http_push_request& http_client_push::request() const& {
    const auto* state_value = response_.state_;
    if (state_value == nullptr || !state_value->promised_request_) {
        throw std::logic_error("push owner has moved");
    }
    if (!state_value->memory_domain().worker().is_current()) {
        throw std::logic_error("push requires its owner worker");
    }
    return *state_value->promised_request_;
}
task<http_client_response> http_client_push::response(operation_options operation) & {
    auto* state_value = response_.state_;
    (void)request();
    // Unlike an exchange, a push admits no second receive, even a cold one.
    if (state_value->push_response_.taken() || state_value->push_response_.has_pending_receive()) {
        throw std::logic_error("push response has already been transferred or is active");
    }
    return state_value->push_response_.receive(
        http_client_response(state_value), "HTTP push failed before its response head", std::move(operation));
}
}  // namespace ruvia

namespace ruvia::detail {

bool is_tls_error(const std::error_code& error) noexcept {
    return error.category() == asio::error::get_ssl_category() || error == asio::ssl::error::stream_truncated;
}

client_tcp_transport::client_tcp_transport(asio::io_context& io, asio::ssl::context& tls,
    const worker_ref& worker_value, client_tcp_endpoint endpoint, client_wire_counters& counters)
    : worker_(worker_value),
      tls_(tls),
      endpoint_(endpoint),
      literal_endpoint_([&]() -> std::optional<asio::ip::tcp::endpoint> {
          if (const auto address = parse_ip_address(endpoint_.host_)) {
              return asio::ip::tcp::endpoint(*address, endpoint_.port_);
          }
          return std::nullopt;
      }()),
      counters_(counters),
      resolver_(worker_value),
      stream_(io.get_executor(), tls) {
    // Owners build their transports eagerly, before any member that must be
    // retired, so a host this worker cannot look up rejects the client itself.
    require_client_host_lookup(endpoint_.host_, worker_);
}

client_tcp_transport::~client_tcp_transport() = default;

void client_tcp_transport::abort(client_abort_reason reason) noexcept {
    // The latest owner decision wins: a pool shutdown reports closing even if
    // a request cancellation reached the transport first.
    if (reason != client_abort_reason::none) {
        abort_reason_ = reason;
    }
    if (writing_ || reason == client_abort_reason::timeout || reason == client_abort_reason::cancelled) {
        tls_close_ready_ = false;
    }
    resolver_.cancel();
    std::error_code ignored;
    if (connecting_ != nullptr) {
        connecting_->cancel();
    } else {
        (void)stream_.lowest_layer().cancel(ignored);
    }
}

void client_tcp_transport::close() noexcept {
    read_.timer_.cancel();
    read_.deadline_.reset();
    write_.timer_.cancel();
    write_.deadline_.reset();
    resolver_.cancel();
    if (connecting_ != nullptr) {
        connecting_->cancel();
    }
    send_close_notify();
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

void client_tcp_transport::send_close_notify() noexcept {
    if (!tls_close_ready_ || writing_) {
        return;
    }
    tls_close_ready_ = false;
    auto& layer = stream_.next_layer();
    auto& socket = layer.socket();
    SSL* const native = stream_.native_handle();
    // One worker runs every engine call, so the engine orders the alert after
    // the records of the pending read and write. On the socket it must follow
    // them whole too: while a record write waits for the socket or ended
    // inside a record (from a write, or from a read answering a post-handshake
    // message) the alert would land inside that record, and after a failed or
    // cancelled write it would follow a cut or lost one, so the close goes
    // without the alert. A post-handshake message still being read keeps the
    // engine in init.
    if (!layer.records_whole() || !socket.is_open() || SSL_is_init_finished(native) != 1 ||
        (SSL_get_shutdown(native) & SSL_SENT_SHUTDOWN) != 0) {
        return;
    }
    // Send our alert without waiting for the peer's: recording its alert as
    // received makes SSL_shutdown complete after the one write on the
    // non-blocking socket, which first flushes engine output an operation
    // left unsent. The engine is never read again; a full socket buffer fails
    // the write and the close proceeds without the alert.
    SSL_set_shutdown(native, SSL_get_shutdown(native) | SSL_RECEIVED_SHUTDOWN);
    std::error_code error;
    (void)stream_.shutdown(error);
}

// An idle pooled connection is reusable only while the peer has sent nothing:
// unsolicited bytes are a stray response or the start of a close. The probe is
// a constant-time inspection that never runs the TLS engine: a decrypting read
// on the worker keeps processing records for as long as the peer keeps sending
// post-handshake control records (TLS 1.3 NewSessionTicket, KeyUpdate), and
// nonblocking mode does not bound that loop. Without decrypting, those records
// cannot be told from a close_notify or a stray response, so any record
// OpenSSL already buffers (decrypted or not) or any byte waiting on the socket
// retires the connection. Servers send their tickets right after the
// handshake, ahead of the first response, so the response read consumes them
// and an idle connection normally carries none; a later control record only
// costs one reconnect. Raw bytes Asio holds in its own input buffer stay
// invisible here, like a close racing the probe: the response read reports them.
client_idle_transport_status client_tcp_transport::idle_status() noexcept {
    auto& socket = stream_.next_layer().socket();
    if (!socket.is_open()) {
        return client_idle_transport_status::closed;
    }
    if (secure()) {
        SSL* const native = stream_.native_handle();
        if (SSL_has_pending(native) != 0 || BIO_ctrl_pending(SSL_get_rbio(native)) != 0) {
            return client_idle_transport_status::pending_data;
        }
    }
    std::error_code error;
    char byte{};
    const auto bytes = socket.receive(
        asio::buffer(&byte, 1), asio::socket_base::message_peek, error);
    if (error == asio::error::would_block) {
        return client_idle_transport_status::reusable;
    }
    return !error && bytes != 0 ? client_idle_transport_status::pending_data
                                : client_idle_transport_status::closed;
}

task<void> client_tcp_transport::wait_readable() {
    // A readiness wait consumes nothing, so a readiness that races the next
    // exchange leaves its bytes to that exchange's read.
    (void)co_await async_asio([this](auto handler) mutable {
        stream_.next_layer().socket().async_wait(asio::socket_base::wait_read, std::move(handler));
    });
}

void client_tcp_transport::cancel_wait() noexcept {
    std::error_code ignored;
    (void)stream_.next_layer().socket().cancel(ignored);
}

bool client_tcp_transport::arm(lane_deadline& lane, const operation_deadline& timeout) {
    lane.timer_.cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
        lane.deadline_.reset();
        return true;
    }
    if (remaining->count() == 0) {
        lane.deadline_.reset();
        return false;
    }
    const auto deadline_value = *timeout.deadline();
    lane.deadline_.arm(deadline_value);
    (void)worker_.schedule_timer(lane.timer_, deadline_value, [this, &lane](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired && lane.deadline_.expire(std::chrono::steady_clock::now())) {
            abort(client_abort_reason::timeout);
        }
    });
    return true;
}

bool client_tcp_transport::clear(lane_deadline& lane) noexcept {
    lane.timer_.cancel();
    return lane.deadline_.clear();
}

client_transport_result client_tcp_transport::aborted() const noexcept {
    return {.status_ = client_transport_status::aborted,
        .abort_ = abort_reason_ == client_abort_reason::none ? client_abort_reason::timeout : abort_reason_};
}

client_transport_result client_tcp_transport::finish(lane_deadline& lane, const operation_deadline& timeout,
    const std::error_code& error, std::size_t bytes, client_transport_status failure) noexcept {
    if ((clear(lane) || timeout.expired()) && abort_reason_ == client_abort_reason::none) {
        abort_reason_ = client_abort_reason::timeout;
    }
    if (abort_reason_ != client_abort_reason::none) {
        tls_close_ready_ = false;
        return aborted();
    }
    if (error) {
        if (failure == client_transport_status::io_failed && is_stream_read_end(error)) {
            return {.status_ = client_transport_status::eof, .error_ = error};
        }
        const auto status = secure() && tls_started_ && is_tls_error(error) ? client_transport_status::tls_failed : failure;
        return {.status_ = status, .bytes_ = bytes, .error_ = error};
    }
    return {.bytes_ = bytes};
}

task<client_transport_result> client_tcp_transport::connect(const operation_deadline& timeout) {
    const auto connect_started = std::chrono::steady_clock::now();
    connect_duration_ = {};
    tls_duration_ = {};
    peer_endpoint_ = {};
    tls_close_ready_ = false;
    writing_ = false;
    if (tls_started_) {
        // Closing a socket leaves Asio's encrypted input cursor and BIO pair
        // alive. Swap through a fresh stream so the retired SSL engine is
        // destroyed without overwriting its native owner, and the socket
        // layer's record accounting starts over.
        asio::ssl::stream<client_tls_socket> fresh(stream_.get_executor(), tls_);
        std::swap(stream_, fresh);
        tls_started_ = false;
    } else {
        stream_.next_layer().reset();
    }
    if (!arm(read_, timeout)) {
        co_return aborted();
    }
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    client_resolve_result resolved;
    if (endpoint_.resolve_) {
        resolved = co_await endpoint_.resolve_(endpoint_.resolve_context_, resolver_, timeout, endpoint_.host_, endpoint_.port_);
    } else if (literal_endpoint_) {
        resolved.literal_ = literal_endpoint_;
    } else {
        auto pending = resolver_.resolve(endpoint_.host_, endpoint_.port_, timeout);
        if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
            resolved = std::move(*immediate);
        } else {
            resolved = co_await std::move(std::get<task<client_resolve_result>>(pending));
        }
    }
    if (abort_reason_ != client_abort_reason::none || read_.deadline_.expired() || timeout.expired()) {
        co_return finish(read_, timeout, {}, 0, client_transport_status::resolve_failed);
    }
    if (resolved.error_) {
        co_return finish(read_, timeout, resolved.error_, 0, client_transport_status::resolve_failed);
    }
    if (resolved.literal_) {
        // One numeric endpoint needs neither a resolver result allocation nor
        // the multi-address race. The same transport deadline and cancellation
        // own this socket as in the ordinary I/O paths.
        stream_.next_layer().replace_socket(asio::ip::tcp::socket(stream_.get_executor()));
        const auto connected = co_await async_asio([this, endpoint = *resolved.literal_](auto handler) mutable {
            stream_.next_layer().socket().async_connect(endpoint, std::move(handler));
        });
        if (connected.error_code() || abort_reason_ != client_abort_reason::none ||
            read_.deadline_.expired() || timeout.expired()) {
            co_return finish(read_, timeout, connected.error_code(), 0, client_transport_status::connect_failed);
        }
    } else {
        client_happy_eyeballs connector(worker_, stream_.get_executor(), resolved.addresses_);
        resolved = {};
        connecting_ = &connector;
        struct clear_connector final {
            client_happy_eyeballs*& value_;
            ~clear_connector() {
                value_ = nullptr;
            }
        } connector_guard{connecting_};
        auto connected = co_await connector.connect();
        connecting_ = nullptr;
        if (connected.error_ || abort_reason_ != client_abort_reason::none || read_.deadline_.expired() ||
            timeout.expired()) {
            co_return finish(read_, timeout, connected.error_, 0, client_transport_status::connect_failed);
        }
        if (!connected.socket_) {
            std::terminate();
        }
        stream_.next_layer().replace_socket(std::move(*connected.socket_));
    }
    connect_duration_ = std::chrono::steady_clock::now() - connect_started;
    std::error_code peer_error;
    peer_endpoint_ = stream_.next_layer().socket().remote_endpoint(peer_error);
    if (peer_error) {
        co_return finish(read_, timeout, peer_error, 0, client_transport_status::connect_failed);
    }
    auto mode_error = apply_tcp_socket_policies(stream_.next_layer().socket(), endpoint_.tcp_);
    // Every socket operation is asynchronous or a bounded probe, and the TLS
    // socket layer writes records directly.
    if (!mode_error) {
        stream_.next_layer().socket().non_blocking(true, mode_error);
    }
    if (mode_error || !secure()) {
        co_return finish(read_, timeout, mode_error, 0, client_transport_status::connect_failed);
    }
    tls_started_ = true;
    if (const auto prepared =
            prepare_client_tls_stream(*stream_.native_handle(), endpoint_.identity_host_.empty() ? endpoint_.tls_->tls_host(endpoint_.host_) : endpoint_.identity_host_, endpoint_.alpn_);
        prepared != client_tls_setup_error::none) {
        (void)clear(read_);
        co_return client_transport_result{.status_ = client_transport_status::tls_failed,
            .error_ = make_client_tls_setup_error_code(prepared)};
    }
    const auto tls_started = std::chrono::steady_clock::now();
    const auto handshake = co_await async_asio([this](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    tls_duration_ = std::chrono::steady_clock::now() - tls_started;
    auto result_value = finish(read_, timeout, handshake.error_code(), 0, client_transport_status::tls_failed);
    // RFC 9113 section 9.2: even a noncompliant peer selecting h2 over a
    // legacy TLS connection must never cause HTTP/2 preface/application I/O.
    // Enforce this here as raw TCP clients can also offer h2.
    if (result_value.ok() && alpn() == "h2" && SSL_version(stream_.native_handle()) < TLS1_2_VERSION) {
        result_value.status_ = client_transport_status::tls_failed;
        result_value.error_ = make_client_tls_setup_error_code(client_tls_setup_error::protocol_failed);
    }
    tls_close_ready_ = result_value.ok();
    co_return result_value;
}

std::string_view client_tcp_transport::alpn() noexcept {
    return tls_started_ ? tls_selected_alpn(*stream_.native_handle()) : std::string_view{};
}

task<client_transport_result> client_tcp_transport::read_some(std::span<char> bytes_value, const operation_deadline& timeout) {
    if (abort_reason_ != client_abort_reason::none || !arm(read_, timeout)) {
        co_return aborted();
    }
    const auto buffer = asio::buffer(bytes_value);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.async_read_some(buffer, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffer](auto handler) mutable {
                                      stream_.next_layer().socket().async_read_some(buffer, std::move(handler));
                                  });
    // After a TLS failure the engine owes the peer no further records.
    if (is_tls_error(completion.error_code())) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.received_ += transferred;
    auto result_value = finish(read_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.ok() && result_value.bytes_ == 0) {
        result_value.status_ = client_transport_status::eof;
    }
    co_return result_value;
}

template <typename buffers_type>
task<client_transport_result> client_tcp_transport::write_buffers(buffers_type buffers, const operation_deadline& timeout) {
    if (abort_reason_ != client_abort_reason::none) {
        co_return aborted();
    }
    if (asio::buffer_size(buffers) == 0) {
        co_return client_transport_result{};
    }
    if (!arm(write_, timeout)) {
        tls_close_ready_ = false;
        co_return aborted();
    }
    struct write_guard final {
        bool& writing_;
        explicit write_guard(bool& writing) noexcept
            : writing_(writing) {
            writing_ = true;
        }
        ~write_guard() noexcept {
            writing_ = false;
        }
    } guard(writing_);
    const auto completion = secure()
                                ? co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_, buffers, std::move(handler));
                                  })
                                : co_await async_asio<std::size_t>([this, buffers](auto handler) mutable {
                                      asio::async_write(stream_.next_layer().socket(), buffers, std::move(handler));
                                  });
    if (completion.error_code()) {
        tls_close_ready_ = false;
    }
    const auto transferred = completion.result();
    counters_.sent_ += transferred;
    auto result_value = finish(write_, timeout, completion.error_code(),
        completion.error_code() ? 0 : transferred, client_transport_status::io_failed);
    if (result_value.status_ == client_transport_status::eof) {
        result_value.status_ = client_transport_status::io_failed;
    }
    co_return result_value;
}

task<client_transport_result> client_tcp_transport::write(std::string_view bytes_value, const operation_deadline& timeout) {
    return write_buffers(asio::buffer(bytes_value), timeout);
}

task<client_transport_result> client_tcp_transport::write(
    std::span<const asio::const_buffer> buffers, const operation_deadline& timeout) {
    return write_buffers(buffers, timeout);
}

namespace {

struct client_tls_preparation final {
    explicit client_tls_preparation(client_tls_config_storage config)
        : config_(std::move(config)),
          password_() {}
    ~client_tls_preparation() {
        if (!password_.empty()) {
            OPENSSL_cleanse(password_.data(), password_.size());
        }
    }
    client_tls_config_storage config_;
    std::string password_;
};

task<client_tls_config_storage> prepare_client_tls_owned(worker_ref worker,
    std::unique_ptr<client_tls_preparation> input,
    stop_token stop) {
    auto result = co_await run_blocking(std::move(worker),
        [input = std::move(input), stop = std::move(stop)]() mutable {
            if (stop.stop_requested()) {
                throw std::runtime_error("client TLS preparation cancelled");
            }
            input->config_.parse_files(input->password_);
            return std::move(input->config_);
        });
    co_return std::move(result).take_value();
}

constexpr std::array<unsigned char, 9> http11_alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> http2_alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> negotiated_http_alpn = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] bool configure_client_tls_peer_identity(SSL& connection, const char* host, bool ip_address) noexcept {
    X509_VERIFY_PARAM* const parameters = SSL_get0_param(&connection);
    if (ip_address) {
        return X509_VERIFY_PARAM_set1_host(parameters, nullptr, 0) == 1 &&
               X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1;
    }
    // RFC 9110 §4.3.4 / RFC 9525 §6.3: DNS-ID only, never the subject Common
    // Name, and a wildcard only as the complete left-most label.
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) == 1 &&
           X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1;
}

// Prepares SNI and the expected peer identity; TLS always authenticates the peer.
[[nodiscard]] client_tls_setup_error prepare_client_tls_session(SSL& ssl, std::string_view host) noexcept {
    const bool is_ip_address = is_client_ip_address(host);
    if (!is_ip_address && host.ends_with('.')) {
        host.remove_suffix(1);
    }
    std::array<char, 256> tls_host_buffer;
    if (host.size() >= tls_host_buffer.size()) {
        return client_tls_setup_error::sni_failed;
    }
    std::ranges::copy(host, tls_host_buffer.begin());
    tls_host_buffer[host.size()] = '\0';
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!is_ip_address && SSL_set_tlsext_host_name(&ssl, tls_host_buffer.data()) != 1) {
        return client_tls_setup_error::sni_failed;
    }
    if (!configure_client_tls_peer_identity(ssl, tls_host_buffer.data(), is_ip_address)) {
        return client_tls_setup_error::peer_identity_failed;
    }
    return client_tls_setup_error::none;
}

[[nodiscard]] std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept {
    switch (error) {
        case client_tls_setup_error::none:
            return {};
        case client_tls_setup_error::reset_failed:
            return "failed to reset TLS stream";
        case client_tls_setup_error::sni_failed:
            return "failed to set TLS SNI host";
        case client_tls_setup_error::alpn_failed:
            return "failed to configure TLS ALPN";
        case client_tls_setup_error::peer_identity_failed:
            return "failed to configure TLS peer identity";
        case client_tls_setup_error::protocol_failed:
            return "TLS protocol requirements were not met";
    }
    std::terminate();
}

class client_tls_setup_category_type final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "client TLS setup";
    }
    [[nodiscard]] std::string message(int value) const override {
        return std::string(client_tls_setup_error_message(static_cast<client_tls_setup_error>(value)));
    }
};

}  // namespace
[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn mode) noexcept {
    switch (mode) {
        case client_alpn::none:
            return {};
        case client_alpn::http11:
            return http11_alpn;
        case client_alpn::http2:
            return http2_alpn;
        case client_alpn::negotiate:
            return negotiated_http_alpn;
    }
    std::terminate();
}

client_tls_config_storage::client_tls_config_storage(
    const client_tls_config& source_value, client_tls_files files)
    : mode_(source_value.mode_),
      minimum_version_(source_value.minimum_version_),
      max_outgoing_records_(source_value.max_outgoing_records_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(make_tls_file_name(*source_value.ca_file_));
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(make_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
    if (files != client_tls_files::parse || mode_ == client_tls_mode::disabled) {
        return;
    }
    parse_files(source_value.identity_ ? std::string_view(source_value.identity_->private_key_password_) : std::string_view{});
}

void client_tls_config_storage::parse_files(std::string_view private_key_password) {
    // An owner may supply its shared system trust store; it is never re-read here.
    if (!trust_store_.has_value()) {
        trust_store_ = ca_file_.has_value() ? load_tls_trust_store(*ca_file_) : tls_trust_store::load_default();
    }
    if (identity_.has_value()) {
        certificate_ = load_tls_certificate(*identity_, private_key_password);
    }
}

task<client_tls_config_storage> prepare_client_tls_config(
    worker_ref worker, client_tls_config_storage config,
    std::string_view private_key_password, stop_token stop) {
    auto input = std::make_unique<client_tls_preparation>(std::move(config));
    // Keep the secret at one stable address: moving an SSO string can leave
    // uncleansed bytes behind in an abandoned coroutine or callable frame.
    input->password_.assign(private_key_password);
    return prepare_client_tls_owned(std::move(worker), std::move(input), std::move(stop));
}

client_tls_config_storage::client_tls_config_storage(
    const client_tls_config_storage& source_value)
    : mode_(source_value.mode_),
      minimum_version_(source_value.minimum_version_),
      max_outgoing_records_(source_value.max_outgoing_records_),
      trust_store_(source_value.trust_store_),
      certificate_(source_value.certificate_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(*source_value.ca_file_);
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(copy_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
}

void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config) {
    if (config.mode_ != client_tls_mode::verify_identity) {
        throw std::invalid_argument("client TLS context requires TLS to be enabled");
    }
    apply_tls_protocol_policy(context_value, config.max_outgoing_records_);
    if (SSL_CTX_set_min_proto_version(&context_value, static_cast<int>(config.minimum_version_)) != 1) {
        throw std::runtime_error("failed to configure client TLS minimum version");
    }
    SSL_CTX_set_verify(&context_value, SSL_VERIFY_PEER, nullptr);
    if (!config.trust_store_.has_value()) {
        throw std::logic_error("client TLS trust store was not parsed");
    }
    config.trust_store_->install(context_value);
    if (!config.identity_.has_value()) {
        return;
    }
    if (!config.certificate_.has_value()) {
        throw std::logic_error("client TLS certificate was not parsed");
    }
    config.certificate_->install(context_value);
}

client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host, std::span<const unsigned char> protocols) {
    if (SSL_clear(&ssl) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    // Keep a stricter context minimum (in particular QUIC's TLS 1.3).
    // Mixed ALPN offers may negotiate HTTP/1.1 over TLS 1.1; the shared TCP
    // transport checks the selected protocol before exposing any byte I/O.
    if (std::ranges::equal(protocols, http2_alpn) &&
        SSL_get_min_proto_version(&ssl) < TLS1_2_VERSION &&
        SSL_set_min_proto_version(&ssl, TLS1_2_VERSION) != 1) {
        return client_tls_setup_error::protocol_failed;
    }
    if (const auto prepared = prepare_client_tls_session(ssl, host); prepared != client_tls_setup_error::none) {
        return prepared;
    }
    if (SSL_set_alpn_protos(&ssl, protocols.data(), static_cast<unsigned int>(protocols.size())) != 0) {
        return client_tls_setup_error::alpn_failed;
    }
    return client_tls_setup_error::none;
}

std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept {
    static const client_tls_setup_category_type category;
    return {static_cast<int>(error), category};
}

client_happy_eyeballs::client_happy_eyeballs(const worker_ref& worker,
    const asio::ip::tcp::socket::executor_type& executor,
    const asio::ip::tcp::resolver::results_type& addresses)
    : worker_(worker),
      changed_(worker),
      attempts_() {
    if (addresses.empty()) {
        return;
    }
    attempts_.reserve(addresses.size());
    client_address_sequence sequence(addresses);
    while (auto endpoint = sequence.next()) {
        attempts_.emplace_back(executor, *endpoint);
    }
}

client_happy_eyeballs::~client_happy_eyeballs() {
    delay_.cancel();
    if (active_ != 0) {
        std::terminate();
    }
}

void client_happy_eyeballs::close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    (void)socket.cancel(ignored);
    (void)socket.close(ignored);
}

void client_happy_eyeballs::close_active() noexcept {
    for (auto& current : attempts_) {
        if (current.active_) {
            close_socket(current.socket_);
        }
    }
}

void client_happy_eyeballs::cancel() noexcept {
    if (cancelled_) {
        return;
    }
    cancelled_ = true;
    if (winner_) {
        close_socket(attempts_[*winner_].socket_);
    }
    winner_.reset();
    delay_.cancel();
    close_active();
    changed_.notify();
}

void client_happy_eyeballs::schedule_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    try {
        const auto status = worker_.schedule_timer(delay_, deadline_after(client_connection_attempt_delay),
            [this](worker_timer_outcome outcome) noexcept { on_delay(outcome); });
        if (status != worker_timer_schedule_status::worker_stopping) {
            return;
        }
    } catch (...) {
        failure_ = std::current_exception();
    }
    cancel();
}

void client_happy_eyeballs::start_next() noexcept {
    if (next_ == attempts_.size() || cancelled_ || winner_) {
        return;
    }
    const auto index = next_++;
    auto& current = attempts_[index];
    current.active_ = true;
    ++active_;
    try {
        current.socket_.async_connect(current.endpoint_,
            [this, index](const std::error_code& error) noexcept { on_connect(index, error); });
    } catch (...) {
        current.active_ = false;
        --active_;
        close_socket(current.socket_);
        failure_ = std::current_exception();
        cancel();
        return;
    }
    schedule_next();
}

void client_happy_eyeballs::on_delay(worker_timer_outcome outcome) noexcept {
    if (outcome == worker_timer_outcome::cancelled) {
        cancel();
        return;
    }
    launch_ready_ = true;
    changed_.notify();
}

void client_happy_eyeballs::on_connect(std::size_t index, const std::error_code& error) noexcept {
    auto& current = attempts_[index];
    if (!current.active_ || active_ == 0) {
        std::terminate();
    }
    current.active_ = false;
    --active_;
    if (!error && !cancelled_ && !winner_) {
        winner_ = index;
        delay_.cancel();
        close_active();
    } else {
        if (error && error != asio::error::operation_aborted) {
            last_error_ = error;
            if (!cancelled_ && !winner_) {
                // RFC 8305 section 5: a fast failure must not leave the next
                // address waiting for the remainder of the attempt delay.
                delay_.cancel();
                launch_ready_ = true;
            }
        }
        close_socket(current.socket_);
    }
    changed_.notify();
}

task<client_happy_eyeballs_result> client_happy_eyeballs::connect() & {
    if (cancelled_) {
        co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
    }
    if (attempts_.empty()) {
        co_return client_happy_eyeballs_result{{}, asio::error::host_not_found};
    }
    start_next();
    for (;;) {
        if (launch_ready_) {
            launch_ready_ = false;
            start_next();
        }
        if (cancelled_) {
            close_active();
            if (active_ == 0) {
                if (failure_) {
                    std::rethrow_exception(failure_);
                }
                co_return client_happy_eyeballs_result{{}, asio::error::operation_aborted};
            }
        } else if (winner_) {
            if (active_ == 0) {
                auto socket = std::move(attempts_[*winner_].socket_);
                co_return client_happy_eyeballs_result{std::move(socket), {}};
            }
        } else if (active_ == 0 && next_ == attempts_.size()) {
            co_return client_happy_eyeballs_result{{},
                last_error_ ? last_error_ : std::error_code(asio::error::host_unreachable)};
        }
        co_await changed_.wait();
    }
}

struct client_host_resolver::query final {
    std::string host_;
    std::uint16_t port_;

    [[nodiscard]] client_resolve_result operator()() const {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        std::array<char, http_max_unsigned_decimal_size> digits;
        const auto service = http_format_unsigned(digits, port_);
        client_resolve_result result;
        result.addresses_ = resolver.resolve(host_, service, result.error_);
        return result;
    }
};

bool client_host_needs_lookup(std::string_view host) {
    return !parse_ip_address(host).has_value();
}

void require_client_host_lookup(std::string_view host, const worker_ref& worker) {
    if (worker.blocking_pool() == nullptr && client_host_needs_lookup(host)) {
        throw std::invalid_argument("a DNS host requires an event loop with a blocking pool");
    }
}

client_host_resolver::client_host_resolver(client_host_resolver&& other) noexcept
    : worker_(other.worker_) {
    if (other.stop_) {
        std::terminate();
    }
}

client_host_resolver& client_host_resolver::operator=(client_host_resolver&& other) noexcept {
    if (stop_ || other.stop_) {
        std::terminate();
    }
    worker_ = other.worker_;
    return *this;
}

client_resolve_operation client_host_resolver::resolve(std::string_view host, std::uint16_t port,
    const operation_deadline& timeout, stop_token stop) {
    if (stop.stop_requested()) {
        return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
    if (const auto address = parse_ip_address(host)) {
        return client_resolve_result{.literal_ = asio::ip::tcp::endpoint(*address, port)};
    }
    return resolve_query(query{std::string(host), port}, timeout, std::move(stop));
}

task<client_resolve_result> client_host_resolver::resolve_query(query input, operation_deadline timeout, stop_token stop) {
    if (stop_) {
        throw std::logic_error("host resolver already has a pending query");
    }
    stop_.emplace();
    struct reset_stop final {
        std::optional<stop_source>& stop_;
        ~reset_stop() {
            stop_.reset();
        }
    } guard{stop_};
    stop_group cancellation(stop_->token(), stop);
    auto result = co_await run_blocking(
        *worker_, {.timeout_ = timeout.timeout(), .stop_token_ = cancellation.token()}, std::move(input));
    switch (result.status()) {
        case blocking_status::completed:
            co_return std::move(result).take_value();
        case blocking_status::timed_out:
            co_return client_resolve_result{.error_ = asio::error::timed_out};
        case blocking_status::queue_full:
            co_return client_resolve_result{.error_ = asio::error::no_buffer_space};
        default:
            co_return client_resolve_result{.error_ = asio::error::operation_aborted};
    }
}

void client_host_resolver::cancel() noexcept {
    if (stop_) {
        stop_->request_stop();
    }
}

namespace {

[[nodiscard]] std::string owned_host(const tcp_client_config& config) {
    validate_client_origin_host(config.host_, "TCP host must not be empty", "TCP host is invalid");
    if (config.port_.value() == 0) {
        throw std::invalid_argument("TCP port must not be zero");
    }
    validate_client_tls_config(config.tls_);
    validate_client_tcp_config(config.tcp_);
    for (const auto& timeout : {config.connect_timeout_, config.read_timeout_, config.write_timeout_}) {
        if (timeout.has_value()) {
            // Each configured bound becomes an operation_deadline, which rejects non-positive values.
            static_cast<void>(operation_timeout(*timeout));
        }
    }
    return std::string(config.host_);
}

[[nodiscard]] std::vector<unsigned char> owned_alpn(const tcp_client_config& config) {
    if (!config.alpn_.empty() && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("TCP ALPN requires TLS");
    }
    std::size_t size = 0;
    for (const auto& protocol : config.alpn_) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("TLS ALPN names must contain 1..255 bytes");
        }
        if (65535 - size < protocol.size() + 1) {
            throw std::invalid_argument("TLS ALPN offer exceeds 65535 bytes");
        }
        size += protocol.size() + 1;
    }
    std::vector<unsigned char> wire;
    wire.reserve(size);
    for (const auto& protocol : config.alpn_) {
        wire.push_back(static_cast<unsigned char>(protocol.size()));
        const auto* bytes = reinterpret_cast<const unsigned char*>(protocol.data());
        wire.insert(wire.end(), bytes, bytes + protocol.size());
    }
    return wire;
}

[[nodiscard]] asio::ssl::context make_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context.native_handle(), config);
    }
    return context;
}

[[noreturn]] void throw_transport_error(const client_transport_result& result) {
    std::error_code error = result.error_;
    if (result.status_ == client_transport_status::aborted) {
        error = std::make_error_code(result.abort_ == client_abort_reason::timeout ? std::errc::timed_out
                                                                                   : std::errc::operation_canceled);
    }
    if (!error) {
        error = std::make_error_code(std::errc::io_error);
    }
    throw std::system_error(error, "TCP client operation failed");
}
}  // namespace

class tcp_client_backend final {
public:
    tcp_client_backend(event_loop& loop, const worker_ref& worker, const tcp_client_config& config)
        : host_(owned_host(config)),
          alpn_(owned_alpn(config)),
          tls_config_(config.tls_, client_tls_files::parse),
          tls_context_(make_tls_context(tls_config_)),
          transport_(loop.io_context(), tls_context_, worker,
              {host_, config.port_.value(), tls_config_.mode_ == client_tls_mode::disabled ? nullptr : &tls_config_, config.tcp_, alpn_}, counters_),
          cancellation_(make_worker_cancellation_target(*this, worker)),
          connect_timeout_(config.connect_timeout_),
          read_timeout_(config.read_timeout_),
          write_timeout_(config.write_timeout_) {}

    ~tcp_client_backend() {
        cancellation_->detach(*this);
    }

    [[nodiscard]] task<void> connect() {
        // The lifecycle admits one startup; snapshot options before suspending.
        return connect_transport(std::move(connect_options_));
    }

    [[nodiscard]] std::string_view alpn() noexcept {
        return transport_.alpn();
    }

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        auto lease = claim_operation_lane(read_active_, "TCP client read already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = read_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, read_id_);
        cancellation.arm(options.stop_token_);
        if (transport_.abort_reason() != client_abort_reason::none) {
            throw_transport_error({.status_ = client_transport_status::aborted, .abort_ = transport_.abort_reason()});
        }
        if (eof_) {
            co_return std::nullopt;
        }
        const auto result = co_await transport_.read_some(buffer_, timeout);
        if (result.status_ == client_transport_status::eof) {
            eof_ = true;
            co_return std::nullopt;
        }
        if (!result.ok()) {
            throw_transport_error(result);
        }
        co_return as_bytes(std::string_view(buffer_.data(), result.bytes_));
    }

    [[nodiscard]] task<void> write(std::string bytes, operation_options options) {
        auto lease = claim_operation_lane(write_active_, "TCP client write already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = write_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, write_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.write(bytes, timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    void close_now() noexcept {
        transport_.abort(client_abort_reason::closing);
        transport_.close();
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id != 0 && (id == connect_id_ || id == read_id_ || id == write_id_)) {
            transport_.abort(client_abort_reason::cancelled);
            // A composed plaintext write may have a successful completion
            // queued but no socket wait for cancel() to interrupt. Cancellation
            // retires this connection, so close it before that completion can
            // initiate another write, and cancel both lane deadlines as well.
            transport_.close();
        }
    }

    operation_options connect_options_;

private:
    [[nodiscard]] task<void> connect_transport(operation_options options) {
        operation_deadline timeout(override_operation_options({.timeout_ = connect_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, connect_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.connect(timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    std::string host_;
    std::vector<unsigned char> alpn_;
    client_tls_config_storage tls_config_;
    asio::ssl::context tls_context_;
    client_wire_counters counters_;
    client_tcp_transport transport_;
    std::shared_ptr<worker_cancellation_target<tcp_client_backend>> cancellation_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::array<char, 4096> buffer_{};
    std::uint64_t connect_id_{};
    std::uint64_t read_id_{};
    std::uint64_t write_id_{};
    bool read_active_{};
    bool write_active_{};
    bool eof_{};
};

class tcp_client_state final : public std::enable_shared_from_this<tcp_client_state> {
public:
    tcp_client_state(event_loop loop, const tcp_client_config& config)
        : loop_(client_lifecycle<tcp_client_state>::require_loop(std::move(loop))),
          worker_(loop_.worker()),
          backend_(loop_, worker_, config),
          lifecycle_(*this, loop_, worker_) {}

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect(operation_options options) {
        return connect_owned(shared_from_this(), std::move(options));
    }
    [[nodiscard]] std::string_view alpn() {
        lifecycle_.require_ready();
        return backend_.alpn();
    }
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), read_owned(shared_from_this(), std::move(options)));
    }
    [[nodiscard]] task<void> write(std::string bytes, operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), write_owned(shared_from_this(), std::move(bytes), std::move(options)));
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<tcp_client_state>;
    static constexpr std::string_view client_name{"TCP"};
    [[noreturn]] static void throw_not_ready() {
        throw std::system_error(std::make_error_code(std::errc::not_connected), "TCP client is not connected");
    }
    [[nodiscard]] tcp_client_backend& backend() noexcept {
        return backend_;
    }
    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<tcp_client_state> state, operation_options options) {
        if (!state->worker_.is_current()) {
            throw std::logic_error("TCP client must connect on its bound event loop");
        }
        auto lease = claim_operation_lane(state->connecting_, "TCP client connect already in progress");
        state->backend_.connect_options_ = std::move(options);
        co_await state->lifecycle_.connect();
    }
    [[nodiscard]] static task<std::optional<std::span<const std::byte>>> read_owned(
        std::shared_ptr<tcp_client_state> state, operation_options options) {
        state->lifecycle_.require_ready();
        // Lane admission errors are caller errors, not transport failures.
        try {
            co_return co_await state->backend_.read(std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }
    [[nodiscard]] static task<void> write_owned(std::shared_ptr<tcp_client_state> state,
        std::string bytes, operation_options options) {
        state->lifecycle_.require_ready();
        try {
            co_await state->backend_.write(std::move(bytes), std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }

    event_loop loop_;
    worker_ref worker_;
    tcp_client_backend backend_;
    bool connecting_{};
    client_lifecycle<tcp_client_state> lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia {

tcp_client::tcp_client(event_loop loop, const tcp_client_config& config)
    : state_(std::make_shared<detail::tcp_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

tcp_client::~tcp_client() {
    state_->request_close();
}

task<void> tcp_client::connect(operation_options options) & {
    return state_->connect(std::move(options));
}

std::string_view tcp_client::alpn() const& {
    return state_->alpn();
}
task<std::optional<std::span<const std::byte>>> tcp_client::read(operation_options options) & {
    return state_->read(std::move(options));
}

task<void> tcp_client::write(std::span<const std::byte> bytes, operation_options options) & {
    return write_owned(std::string(as_chars(bytes)), std::move(options));
}

task<void> tcp_client::write(std::string_view bytes, operation_options options) & {
    return write_owned(std::string(bytes), std::move(options));
}

task<void> tcp_client::write_owned(std::string bytes, operation_options options) {
    return state_->write(std::move(bytes), std::move(options));
}

void tcp_client::close() noexcept {
    state_->request_close();
}

task<void> tcp_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& tcp_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia

namespace ruvia::detail {

struct http_client_output_write_input final {
    http_client_response pin_;
    std::string bytes_;
    operation_options operation_;
};

struct in_flight_output_guard final {
    http_client_response_state& state_;
    http_client_output_queue& output_;
    bool active_{true};
    ~in_flight_output_guard() noexcept {
        if (!active_) {
            return;
        }
        output_.stop();
        if (state_.pool_ != nullptr && !state_.complete_ && !state_.abandoned_) {
            state_.pool_->abandon_response(state_);
        }
    }
};

// The input pin and owned payload live in this single cold frame. Policies
// handle upload validation/trailers and tunnel retirement without another task.
// The operation's own interruption aborts the exchange or tunnel and reports
// its timeout or cancellation, even if the driver had already taken the chunk.
template <typename policy>
[[nodiscard]] task<void> write_client_output(http_client_response_state* state_value, http_client_output_write_input input) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    const http_client_operation_interruption interruption(*state_value, http_client_operation_lane::output, input.operation_);
    policy::require_write(*state_value);
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    if (input.bytes_.empty()) {
        co_return;
    }
    output.chunk_ = std::move(input.bytes_);
    output.chunk_ready_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (output.chunk_ready_ && !output.stopped_) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    state_value->throw_if_output_interrupted();
    // Retirement may stop the queue after its driver acknowledged this chunk
    // but before this waiter resumes. It cannot revoke the completed write.
    if (output.chunk_ready_) {
        policy::throw_stopped(*state_value);
    }
}

template <typename policy, typename input_type>
[[nodiscard]] task<void> finish_client_output(http_client_response_state* state_value, input_type input, operation_options operation) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    const http_client_operation_interruption interruption(*state_value, http_client_operation_lane::output, operation);
    if (!policy::begin_finish(*state_value, input)) {
        co_return;
    }
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    output.end_requested_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (!output.ended_ && (!output.stopped_ || output.completion_pending_)) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    state_value->throw_if_output_interrupted();
    if (!output.ended_) {
        policy::throw_stopped(*state_value);
    }
}

struct http_client_upload_end_input final {
    http_client_response pin_;
    std::vector<http_header> fields_;
};

task<http_client_response> http_client_response_handoff::receive(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    return ::ruvia::make_scoped_operation(scope_, receive_owned(std::move(pin), head_message, std::move(operation)));
}

task<http_client_response> http_client_response_handoff::receive_owned(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    // `pin` keeps the response state, and with it this handoff, alive.
    auto activity = claim_operation_lane(active_, "HTTP response receive is already active");
    if (taken_) {
        throw std::logic_error("HTTP response has already been transferred");
    }
    const operation_deadline redirect_deadline(
        operation.timeout_.has_value()
            ? operation.timeout_
            : (pin.state_->upload_ && pin.state_->upload_->redirect_request_ &&
                          pin.state_->pool_ != nullptr
                      ? std::optional<operation_timeout>(
                            pin.state_->pool_->config().request_timeout_)
                      : std::nullopt));
    {
        const http_client_operation_interruption interruption(
            *pin.state_, http_client_operation_lane::read, operation);
        co_await pin.state_->await_head(head_message);
    }
    taken_ = true;
    pin.consumer_ = true;
    if (pin.state_->upload_ && pin.state_->upload_->redirect_request_) {
        auto redirect_request = std::move(*pin.state_->upload_->redirect_request_);
        pin.state_->upload_->redirect_request_.reset();
        co_return co_await http_client_request::follow_redirects(
            std::move(redirect_request), std::move(pin), redirect_deadline, true);
    }
    co_return std::move(pin);
}

void http_client_response_handoff::release(http_client_response& owner) noexcept {
    scope_.close();
    if (taken_) {
        owner.consumer_ = false;
    }
}
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void require_writable(detail::http_client_response_state& state_value) {
    state_value.require_owner_worker("HTTP exchange must run on its owner worker");
    if (!state_value.upload_ || state_value.upload_->output_.stopped_ || state_value.upload_->output_.ended_ || state_value.upload_->output_.end_requested_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload is no longer writable");
    }
}
struct upload_output_policy final {
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.upload_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, detail::http_client_upload_end_input& input) {
        require_write(state_value);
        state_value.upload_->trailers_ = std::move(input.fields_);
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.upload_->failure_) {
            std::rethrow_exception(state_value.upload_->failure_);
        }
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
    }
};
}  // namespace

http_client_exchange::http_client_exchange(http_client_response response) noexcept
    : response_(std::move(response)),
      body_(response_.state_) {}
http_client_exchange::http_client_exchange(http_client_exchange&& other) noexcept
    : response_(std::move(other.response_)),
      body_(response_.state_) {
    other.body_.state_ = nullptr;
}
http_client_exchange& http_client_exchange::operator=(http_client_exchange&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
        body_.state_ = response_.state_;
        other.body_.state_ = nullptr;
    }
    return *this;
}
http_client_exchange::~http_client_exchange() {
    release();
}
void http_client_exchange::release() noexcept {
    auto* state_value = response_.state_;
    if (state_value == nullptr) {
        return;
    }
    auto& upload = *state_value->upload_;
    const bool abandon_upload = !upload.output_.ended_ && !upload.output_.stopped_;
    upload.output_.output_scope_.close();
    upload.response_.release(response_);
    if (abandon_upload && upload.response_.taken() &&
        !state_value->complete_ && state_value->pool_ != nullptr) {
        // Keep the retained consumer's read scope open to observe cancellation.
        state_value->pool_->abandon_response(*state_value);
    } else if (!upload.output_.ended_) {
        upload.output_.stop();
    }
    response_.release();
    body_.state_ = nullptr;
}

task<void> http_client_request_body_writer::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), std::move(operation));
}
task<void> http_client_request_body_writer::write(std::string_view bytes_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::write_owned(std::string&& bytes_value, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::end(std::span<const http_header_view> fields_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    http_request_trailers validation;
    if (validation.append(fields_value).has_value()) {
        throw std::invalid_argument("invalid request trailer section");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{http_client_response(state_), std::move(validation).take_fields()}, std::move(operation)));
}
bool http_client_request_body_writer::complete() const noexcept {
    return state_ == nullptr || state_->upload_->output_.ended_;
}
bool http_client_request_body_writer::writable() const noexcept {
    if (state_ == nullptr || !state_->upload_) {
        return false;
    }
    const auto& output = state_->upload_->output_;
    return !output.stopped_ && !output.ended_ && !output.end_requested_;
}
void http_client_request_body_writer::abandon() & noexcept {
    if (state_ == nullptr || !state_->upload_ || state_->upload_->output_.ended_ || state_->upload_->output_.stopped_) {
        return;
    }
    state_->upload_->abandoned_ = true;
    state_->upload_->output_.stop();
}

task<http_client_response> http_client_exchange::response(operation_options operation) & {
    auto* state_value = response_.state_;
    if (state_value == nullptr || state_value->upload_->response_.taken()) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    state_value->require_owner_worker("HTTP exchange must run on its owner worker");
    return state_value->upload_->response_.receive(
        http_client_response(state_value), "HTTP request failed before response head", std::move(operation));
}

namespace {
void require_output(detail::http_client_response_state* state_value, bool finishing) {
    if (state_value == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    state_value->require_owner_worker("HTTP client tunnel requires its owner worker");
    // Normal retirement stops the queue after both directions complete, but
    // cannot invalidate a finish that has already succeeded. Writes and failed
    // or abandoned tunnels still reject terminal output operations.
    const bool completed_finish = finishing && state_value->tunnel_ && state_value->tunnel_->output_.ended_ && state_value->complete_;
    if (!state_value->tunnel_ || (state_value->pool_ == nullptr && !completed_finish) || (state_value->tunnel_->output_.stopped_ && !completed_finish) || !state_value->tunnel_->accepted_ ||
        state_value->abandoned_ || state_value->failure_ || state_value->pool_closed_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel is closed");
    }
    auto& output = state_value->tunnel_->output_;
    if (!finishing && (output.ended_ || output.end_requested_)) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel sending direction is closed");
    }
}
struct tunnel_output_policy final {
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.tunnel_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, http_client_response&) {
        auto& queue = output(state_value);
        if (queue.ended_) {
            return false;
        }
        if (queue.stopped_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        if (state_value.pool_closed_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client tunnel failed");
        }
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel output stopped");
    }
};
void abort_tunnel(detail::http_client_response_state* state, bool malformed) noexcept {
    if (state == nullptr || !state->tunnel_ || state->pool_ == nullptr) {
        return;
    }
    if (state->tunnel_->output_.stopped_ && !malformed) {
        return;
    }
    if (!state->memory_domain().worker().is_current()) {
        std::terminate();
    }
    state->tunnel_->output_.stop();
    if ((!state->complete_ || malformed) && !state->abandoned_) {
        state->pool_->abandon_response(*state, malformed);
    }
}
}  // namespace
http_client_tunnel::http_client_tunnel(http_client_response response) noexcept
    : response_(std::move(response)) {}
http_client_tunnel::http_client_tunnel(http_client_tunnel&& other) noexcept
    : response_(std::move(other.response_)) {}
http_client_tunnel& http_client_tunnel::operator=(http_client_tunnel&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
http_client_tunnel::~http_client_tunnel() {
    release();
}
void http_client_tunnel::release() noexcept {
    if (auto* state_value = response_.state_; state_value != nullptr) {
        state_value->tunnel_->output_.output_scope_.close();
        state_value->tunnel_->output_.stop();
    }
    response_.release();
}
void http_client_tunnel::abort() & noexcept {
    abort_tunnel(response_.state_, false);
}
void http_client_tunnel::abort_malformed() & noexcept {
    abort_tunnel(response_.state_, true);
}
http_udp_tunnel http_client_tunnel::udp(http_capsule_config config) && {
    return http_udp_tunnel(std::move(*this).datagrams(config));
}
task<std::optional<std::span<const std::byte>>> http_client_tunnel::read(operation_options operation) & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.stream().read(std::move(operation));
}
task<std::optional<std::string_view>> http_client_tunnel::text(operation_options operation) & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.stream().text(std::move(operation));
}
task<void> http_client_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), std::move(operation));
}
task<void> http_client_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::finish(operation_options operation) & {
    require_output(response_.state_, true);
    auto& state_value = *response_.state_;
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::finish_client_output<tunnel_output_policy>(&state_value, http_client_response(&state_value), std::move(operation)));
}
}  // namespace ruvia

namespace ruvia::detail {

// Only adapts tunnel I/O. All WebSocket framing, lanes, control traffic,
// interruption and close policy live in websocket_session_connection.
class http_client_websocket_transport final {
public:
    http_client_websocket_transport(http_client_tunnel tunnel, connection_scanner::entry& activity)
        : tunnel_(std::move(tunnel)),
          response_(*tunnel_.response_.state_),
          activity_(activity) {}

    template <typename check_type>
    task<bool> read_more(http_io_buffer& bytes, check_type check, asio::cancellation_slot slot) {
        struct cancellation_reset final {
            asio::cancellation_slot slot_;
            ~cancellation_reset() {
                if (slot_.is_connected()) {
                    slot_.clear();
                }
            }
        } reset{slot};
        if (slot.is_connected()) {
            slot.assign([this](asio::cancellation_type) { response_.data_signal_.notify(); });
        }
        while (response_.offset_ == response_.buffered_.size() && response_.pending_.empty() &&
               !response_.receive_complete() && !response_.failure_ && !response_.pool_closed_ && !response_.abandoned_) {
            check();
            co_await response_.data_signal_.wait();
        }
        check();
        try {
            const auto chunk = co_await tunnel_.read();
            if (!chunk) {
                co_return false;
            }
            bytes.append(as_chars(*chunk));
            co_return true;
        } catch (const http_client_error& error) {
            throw inbound_transport_error(transport_error(error), "WebSocket tunnel read");
        }
    }
    task<std::error_code> write_bytes(std::string_view bytes, http_stream_end end) {
        try {
            while (!bytes.empty()) {
                const auto chunk = bytes.substr(0, 16384);
                co_await tunnel_.write(chunk);
                activity_.note_write_progress();
                bytes.remove_prefix(chunk.size());
            }
            if (end == http_stream_end::end) {
                co_await tunnel_.finish();
            }
            co_return std::error_code{};
        } catch (const http_client_error& error) {
            co_return transport_error(error);
        }
    }
    void abort() noexcept {
        tunnel_.abort();
        response_.data_signal_.notify();
    }
    http_client_response_state& response() noexcept {
        return response_;
    }

private:
    static std::error_code transport_error(const http_client_error& error) noexcept {
        if (error.code() == std::errc::timed_out) {
            return std::make_error_code(std::errc::timed_out);
        }
        if (error.code() == std::errc::operation_canceled) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        return std::make_error_code(std::errc::io_error);
    }
    http_client_tunnel tunnel_;
    http_client_response_state& response_;
    connection_scanner::entry& activity_;
};

class client_websocket_session final : public std::enable_shared_from_this<client_websocket_session> {
public:
    client_websocket_session(http_client_pool& pool, http_client_tunnel tunnel,
        const http_client_websocket_handshake& handshake, websocket_client_deflate_offer deflate,
        websocket_lifecycle_options lifecycle, std::size_t message_limit)
        : worker_(pool.worker()),
          retire_signal_(worker_),
          scanner_(worker_, scanner_options(lifecycle)),
          connection_(http_client_websocket_transport(std::move(tunnel), entry_), worker_, entry_, normalize_websocket_lifecycle_options(lifecycle),
              protocol_byte_limit::limited(message_limit), {},
              {.compression_ = handshake.compression_, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_key, .deflater_ = deflate.deflater_},
              std::string(handshake.selected_subprotocol_)),
          scanner_guard_(scanner_, entry_, {&connection_, &abort_transport, &abort_transport}) {
        entry_.enter_long_lived();
        scanner_.start();
    }
    static void retire(void* target) noexcept {
        auto& self = *static_cast<client_websocket_session*>(target);
        self.retiring_ = true;
        self.retire_signal_.notify();
    }
    static task<void> run(std::shared_ptr<client_websocket_session> self, stop_token pool_stop) {
        auto stop_registration = pool_stop.register_callback([self = self.get()] {
            self->retire_signal_.notify();
        });
        auto& response = self->connection_.transport().response();
        while (!self->retiring_ && !response.pool_closed_ && !pool_stop.stop_requested()) {
            co_await self->retire_signal_.wait();
        }
        stop_registration.reset();
        self->scanner_.stop();
        co_await self->connection_.detach_and_drain_writes();
        if (const auto failure = self->connection_.take_background_failure()) {
            report_unhandled_failure("WebSocket client session", failure);
        }
    }

private:
    static connection_scanner_options scanner_options(const websocket_lifecycle_options& lifecycle) {
        auto interval = std::chrono::milliseconds(1000);
        const auto tighten = [&](const std::optional<timeout_duration>& timeout) {
            if (timeout) {
                interval = std::min(interval, std::max(std::chrono::milliseconds(1), timeout->value() / 2));
            }
        };
        tighten(lifecycle.heartbeat_.ping_interval_);
        tighten(lifecycle.heartbeat_.pong_timeout_);
        tighten(lifecycle.close_handshake_timeout_);
        return {.scan_interval_ = interval};
    }
    static bool mask_key(void*, websocket_mask_key_type& key) noexcept {
        return fill_random_bytes(std::span<unsigned char>(reinterpret_cast<unsigned char*>(key.data()), key.size()));
    }
    static void abort_transport(void* target) noexcept {
        static_cast<websocket_session_connection<http_client_websocket_transport>*>(target)->abort();
    }
    worker_ref worker_;
    worker_signal retire_signal_;
    connection_scanner scanner_;
    connection_scanner::entry entry_;

public:
    websocket_session_connection<http_client_websocket_transport> connection_;

private:
    connection_scanner::guard scanner_guard_;
    bool retiring_{false};
};

}  // namespace ruvia::detail

namespace ruvia {

http_client_websocket_request::http_client_websocket_request(const http_client_ref& client, std::string_view path)
    : client_(client),
      path_(path),
      headers_(),
      subprotocols_() {
    require_open();
    if (!is_valid_http_origin_form_target(path_)) {
        throw std::invalid_argument("WebSocket path must use origin-form");
    }
}
void http_client_websocket_request::require_open() const {
    client_.registration_.require_active();
    if (!client_.pool_->worker().is_current() || consumed_) {
        throw std::logic_error("WebSocket builder is consumed or used outside its worker");
    }
}
http_client_websocket_request& http_client_websocket_request::header(std::string_view name, std::string_view value) {
    require_open();
    headers_.emplace_back(name, value);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::headers(std::span<const http_header_view> fields) {
    require_open();
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_websocket_request& http_client_websocket_request::subprotocol(std::string_view value) {
    require_open();
    subprotocols_.emplace_back(value);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::deflate(websocket_client_deflate_offer offer) {
    require_open();
    deflate_ = offer;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::lifecycle(websocket_lifecycle_options options) {
    require_open();
    lifecycle_ = detail::normalize_websocket_lifecycle_options(options);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::max_message_bytes(std::size_t limit) {
    require_open();
    if (limit == 0) {
        throw std::invalid_argument("WebSocket message limit must be greater than zero");
    }
    max_message_bytes_ = limit;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::configure(http_client_request_options options) {
    require_open();
    detail::validate_http_client_request_options(options);
    request_options_ = std::move(options);
    return *this;
}
task<websocket> http_client_websocket_request::connect() {
    require_open();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), connect_owned(std::move(*this)));
}
task<websocket> http_client_websocket_request::connect_owned(http_client_websocket_request request) {
    request.client_.registration_.require_active();
    auto& pool = *request.client_.pool_;
    std::shared_ptr<detail::http_client_websocket_handshake> handshake;
    std::optional<detail::http_client_request_storage> stored;
    {
        const detail::inline_http_header_views headers(request.headers_);
        validate_http1_websocket_client_handshake_configuration(headers.view());
        std::vector<std::string_view> protocols;
        protocols.reserve(request.subprotocols_.size());
        for (const auto& protocol : request.subprotocols_) {
            protocols.emplace_back(protocol);
        }
        handshake = std::make_shared<detail::http_client_websocket_handshake>(websocket_client_negotiation_config_view{.subprotocols_ = std::span<const std::string_view>(protocols), .deflate_ = request.deflate_});
        stored.emplace("GET", request.path_, headers.view());
        stored->options_ = detail::copy_http_client_request_settings(request.request_options_);
        stored->informational_ = detail::share_http_client_informational(std::move(request.request_options_.informational_));
        stored->pool_owner_ = request.client_.pool_owner_;
        stored->set_host_stop(request.client_.host_stop_token_);
        stored->set_tunnel(pool.config().authority_, "websocket");
        stored->set_websocket(handshake);
    }
    std::string(request.path_.get_allocator()).swap(request.path_);
    std::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
    std::vector<std::string>(request.subprotocols_.get_allocator()).swap(request.subprotocols_);
    // The opening deadline covers admission, transport establishment and every
    // handshake byte, but must not become the established tunnel's lifetime.
    // After opening, user cancellation belongs to each operation's snapshot.
    stop_source opening_stop;
    auto user_stop = request.client_.options_.stop_token_.register_callback(
        [&opening_stop] { opening_stop.request_stop(); });
    bool opening_expired = false;
    worker_timer_registration opening_timer;
    const auto connect_timeout = request.request_options_.timeouts_.connect_.resolve(pool.config().connect_timeout_);
    if (connect_timeout) {
        (void)pool.worker().schedule_timer(opening_timer, deadline_after(connect_timeout->value()),
            [&opening_stop, &opening_expired](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    opening_expired = true;
                    opening_stop.request_stop();
                }
            });
    }
    auto opening_options = request.client_.options_;
    opening_options.stop_token_ = opening_stop.token();
    std::optional<http_client_tunnel_result> opened;
    try {
        opened.emplace(co_await pool.open_tunnel(std::move(*stored), {}, std::move(opening_options)));
    } catch (...) {
        opening_timer.cancel();
        if (opening_expired) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
        }
        throw;
    }
    opening_timer.cancel();
    user_stop.reset();
    if (opening_expired) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
    }
    auto* tunnel = opened->tunnel();
    if (!tunnel) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected),
            "WebSocket handshake rejected", opened->response()->status());
    }
    auto owner = std::make_shared<detail::client_websocket_session>(pool, std::move(*tunnel), *handshake, request.deflate_, request.lifecycle_, request.max_message_bytes_);
    auto result = detail::make_websocket_facade(owner->connection_);
    pool.tasks().spawn(detail::client_websocket_session::run(owner, pool.pool_stop_token()));
    result.state_->owner_ = owner;
    result.state_->retire_ = &detail::client_websocket_session::retire;
    result.state_->defaults_ = request.client_.options_;
    result.state_->read_timeout_ = request.request_options_.timeouts_.read_.resolve(pool.config().read_timeout_);
    co_return result;
}
http_client_websocket_request http_client_ref::websocket(std::string_view path) const {
    registration_.require_active();
    return http_client_websocket_request(*this, path);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// Reject invalid aliases before any pool or alias-index storage is allocated.
template <typename config_type>
void validate_http_client_configs(std::span<const config_type> definitions) {
    validate_capability_aliases(definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
}

}  // namespace

struct http_client_registry::origin_preparation final {
    origin_preparation(const http_client_config& source, const worker_ref& worker,
        stop_token owner_stop, stop_token user_stop, const std::optional<tls_trust_store>& system_trust)
        : alias_(source.alias_.view()),
          config_(source, client_tls_files::keep_paths),
          cancellation_(owner_stop, user_stop) {
        if (config_.scheme_ == http_scheme::https) {
            if (!config_.tls_.ca_file_.has_value()) {
                config_.tls_.trust_store_ = system_trust;
            }
            const auto password = source.tls_.identity_
                                      ? std::string_view(source.tls_.identity_->private_key_password_)
                                      : std::string_view{};
            tls_.emplace(prepare_client_tls_config(worker, std::move(config_.tls_), password, cancellation_.token()));
        }
    }

    std::string alias_;
    http_client_config_storage config_;
    stop_group cancellation_;
    std::optional<task<client_tls_config_storage>> tls_;
};

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const http_client_definition_type> definitions,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      retirements_(worker_) {
    validate_http_client_configs(definitions);
    initialize(definitions);
    preparations_.bind_worker(worker_);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const http_client_config> configs,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      retirements_(worker_) {
    validate_http_client_configs(configs);
    initialize(configs);
    preparations_.bind_worker(worker_);
}

template <typename config_type>
void http_client_registry::initialize(std::span<const config_type> configs) {
    aliases_.reserve(configs.size());
    for (const auto& config : configs) {
        auto pool = std::make_shared<http_client_pool>(io_context_, worker_, startup_config(config), services_);
        const std::string_view alias_view(config.alias_);
        const auto pos = std::ranges::lower_bound(aliases_, alias_view, std::less<>{}, &alias_binding::alias_);
        aliases_.insert(pos, alias_binding{std::string(alias_view), std::move(pool), next_origin_order_++});
    }
}

http_client_registry::~http_client_registry() = default;

http_client_config_storage http_client_registry::startup_config(const http_client_definition_type& definition) {
    // Application definitions were parsed once for every worker.
    return http_client_config_storage(definition.config_);
}

http_client_config_storage http_client_registry::startup_config(const http_client_config& config) {
    http_client_config_storage storage(config, client_tls_files::keep_paths);
    if (storage.scheme_ == http_scheme::https) {
        if (!storage.tls_.ca_file_.has_value()) {
            if (!system_trust_.has_value()) {
                system_trust_ = tls_trust_store::load_default();
            }
            storage.tls_.trust_store_ = system_trust_;
        }
        storage.tls_.parse_files(config.tls_.identity_ ? std::string_view(config.tls_.identity_->private_key_password_)
                                                       : std::string_view{});
    }
    return storage;
}

void http_client_registry::require_mutable() const {
    if (!worker_.is_current()) {
        throw std::logic_error("HTTP client origins must be changed on their bound worker");
    }
    if (closing_ || !worker_.accepting()) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::upsert(const http_client_config& config, operation_options options) {
    require_mutable();
    validate_capability_alias(config.alias_.view(), "HTTP client alias must not be empty");
    auto input = std::make_unique<origin_preparation>(config, worker_, preparation_stop_.token(), options.stop_token_, system_trust_);
    return make_scoped_operation(preparations_, prepare_origin(std::move(input), std::move(options)));
}

void http_client_registry::require_prepared(stop_token stop, const operation_deadline& timeout) const {
    require_mutable();
    if (stop.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP origin update cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP origin update deadline expired", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::prepare_origin(preparation_owner_type input, operation_options options) {
    const operation_deadline timeout(options.timeout_);
    const auto stop = input->cancellation_.token();
    require_prepared(stop, timeout);
    if (input->tls_) {
        std::exception_ptr failure;
        try {
            input->config_.tls_ = co_await std::move(*input->tls_);
        } catch (...) {
            failure = std::current_exception();
        }
        input->tls_.reset();
        // Cancellation cannot interrupt an OpenSSL file read. Join it first,
        // then decide whether the completed candidate may still be published.
        require_prepared(stop, timeout);
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!input->config_.tls_.ca_file_.has_value() && !system_trust_.has_value()) {
            system_trust_ = input->config_.tls_.trust_store_;
        }
    }
    auto replacement = std::make_shared<http_client_pool>(io_context_, worker_, std::move(input->config_), services_);
    require_prepared(stop, timeout);
    publish(std::move(input->alias_), std::move(replacement));
}

void http_client_registry::publish(std::string alias, pool_owner_type replacement) {
    const auto position = std::ranges::lower_bound(aliases_, std::string_view(alias), std::less<>{}, &alias_binding::alias_);
    if (position != aliases_.end() && position->alias_ == alias) {
        auto joining = position->pool_->join();
        retirements_.spawn(retire_origin(position, std::move(replacement), std::move(joining)));
        return;
    }
    if (next_origin_order_ == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("HTTP client origin registration order is exhausted");
    }
    aliases_.insert(position, alias_binding{std::move(alias), std::move(replacement), next_origin_order_});
    ++next_origin_order_;
}

bool http_client_registry::remove(std::string_view alias) {
    require_mutable();
    validate_capability_alias(alias, "HTTP client alias must not be empty");
    const auto position = std::ranges::lower_bound(aliases_, alias, std::less<>{}, &alias_binding::alias_);
    if (position == aliases_.end() || position->alias_ != alias) {
        return false;
    }
    auto joining = position->pool_->join();
    retirements_.spawn(retire_origin(position, {}, std::move(joining)));
    return true;
}

task<void> http_client_registry::retire_origin(alias_map_type::iterator position,
    pool_owner_type replacement, task<void> joining) {
    // task_scope allocates its child before starting this coroutine. Everything
    // through publication is then noexcept: a failed admission changes nothing.
    auto retired = std::exchange(position->pool_, std::move(replacement));
    if (!position->pool_) {
        aliases_.erase(position);
    }
    retired->close_now();
    co_await std::move(joining);
}

void http_client_registry::close_now() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    preparation_stop_.request_stop();
    for (auto& binding : aliases_) {
        binding.pool_->close_now();
    }
}

task<void> http_client_registry::join() {
    close_now();
    std::exception_ptr failure;
    co_await preparations_.close_and_join();
    // Removed origins are no longer in aliases_; their retirement children own
    // the old pools until all transport drivers finish.
    try {
        co_await retirements_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& binding : aliases_) {
        try {
            co_await binding.pool_->join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_ref http_client_registry::get_single_or_default(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return aliases_.size() == 1 ? get(aliases_.front().alias_, scope, std::move(host_stop)) : get(scope, std::move(host_stop));
}

http_client_ref http_client_registry::get(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return get(default_capability_alias, scope, std::move(host_stop));
}

http_client_ref http_client_registry::get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "http client registry is closing", std::nullopt, nullptr, false);
    }
    const auto found = std::ranges::lower_bound(aliases_, alias, std::less<>{}, &alias_binding::alias_);
    if (found == aliases_.end() || found->alias_ != alias) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "named HTTP client is not configured", std::nullopt, nullptr, false);
    }
    http_client_ref result(*found->pool_, scope);
    result.pool_owner_ = found->pool_;
    result.registry_ = this;
    result.host_stop_token_ = std::move(host_stop);
    return result;
}

std::shared_ptr<http_client_pool> http_client_registry::find_origin(http_scheme scheme, std::string_view authority) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
    const alias_binding* found = nullptr;
    // Aliases sharing one origin retain registration precedence, independent
    // of lexical lookup order or a replacement pool's storage position.
    for (const auto& binding : aliases_) {
        const auto& pool = binding.pool_;
        if ((found == nullptr || binding.order_ < found->order_) && pool->scheme() == scheme &&
            http_authorities_equal(pool->config().authority_, authority, scheme == http_scheme::https ? 443 : 80)) {
            found = &binding;
        }
    }
    return found == nullptr ? pool_owner_type{} : found->pool_;
}

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, std::span<const http_client_config> configs,
        const http_client_worker_config& worker_config);

    http_client_state(const http_client_state&) = delete;
    http_client_state& operator=(const http_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }

    [[nodiscard]] http_client_ref client();
    [[nodiscard]] http_client_ref client(std::string_view alias);
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options) {
        lifecycle_.require_ready();
        return registry_.upsert(config, std::move(options));
    }
    [[nodiscard]] bool remove(std::string_view alias) {
        lifecycle_.require_ready();
        return registry_.remove(alias);
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<http_client_state>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] http_client_registry& backend() noexcept {
        return registry_;
    }

    event_loop loop_;
    worker_ref worker_;
    http_client_registry registry_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

http_client_state::http_client_state(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker_config)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      // Responses may outlive this client: the process resource backs them.
      registry_(loop_.io_context(), worker_, configs, make_http_client_worker_services(worker_config)),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client is closing", std::nullopt, nullptr, false);
}

http_client_ref http_client_state::client() {
    lifecycle_.require_ready();
    return registry_.get_single_or_default(lifecycle_.operation_scope(), lifecycle_.token());
}

http_client_ref http_client_state::client(std::string_view alias) {
    lifecycle_.require_ready();
    return registry_.get(alias, lifecycle_.operation_scope(), lifecycle_.token());
}
}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    const http_client_worker_config& worker)
    : http_client(std::move(loop), std::span<const http_client_config>(&config, 1), worker) {}

http_client::http_client(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker)
    : state_(std::make_shared<detail::http_client_state>(std::move(loop), configs, worker)) {
    state_->bind_stop();
}

http_client::~http_client() {
    state_->request_close();
}

http_client_ref http_client::client() const& {
    return state_->client();
}

http_client_ref http_client::client(std::string_view alias) const& {
    return state_->client(alias);
}

task<void> http_client::upsert(const http_client_config& config, operation_options options) & {
    return state_->upsert(config, std::move(options));
}

bool http_client::remove(std::string_view alias) & {
    return state_->remove(alias);
}

void http_client::close() noexcept {
    state_->request_close();
}

task<void> http_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& http_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia

namespace ruvia::detail {

void http_client_response_memory_domain::charge_in_flight(std::size_t bytes_value) {
    if (bytes_value != 0 && !result_budget_domain_->reserve_in_flight(bytes_value)) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded),
            "HTTP client in-flight response memory budget is exhausted");
    }
}

void http_client_response_memory_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value != 0) {
        result_budget_domain_->release_in_flight(bytes_value);
    }
}

http_client_response_memory_domain::http_client_response_memory_domain(const worker_ref& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain)
    : worker_(worker_value),
      result_budget_domain_(result_budget_domain) {
    if (!worker_.valid()) {
        throw std::invalid_argument("HTTP client response memory requires a worker");
    }
}

http_client_response_memory_domain::~http_client_response_memory_domain() {
    if (phase_ != phase_type::prepared) {
        require_current();
    }
    if (state_head_ != nullptr || references_ != 0) {
        std::terminate();
    }
}

http_client_response_memory_domain::owner http_client_response_memory_domain::create(
    const worker_ref& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    return owner(new http_client_response_memory_domain(worker_value, result_budget_domain), deleter_type{});
}

void http_client_response_memory_domain::destroy() noexcept {
    delete this;
}

void http_client_response_memory_domain::deleter_type::operator()(
    http_client_response_memory_domain* domain) const noexcept {
    if (domain != nullptr) {
        domain->release_root();
    }
}

void http_client_response_memory_domain::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}

void http_client_response_memory_domain::retain() noexcept {
    require_current();
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void http_client_response_memory_domain::release() noexcept {
    require_current();
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        destroy();
    }
}

void http_client_response_memory_domain::release_root() noexcept {
    // Startup rollback has never published worker-affine response storage.
    // No worker scheduling is necessary (or possible after failed preparation).
    if (phase_ == phase_type::prepared) {
        if (references_ != 1 || state_head_ != nullptr) {
            std::terminate();
        }
        references_ = 0;
        destroy();
        return;
    }
    // Active storage is retired explicitly by the pool on its owning worker;
    // do not conceal a missing join with unstructured deferred destruction.
    release();
}

http_client_response_state* http_client_response_memory_domain::create_state(http_client_pool& pool) {
    require_current();
    if (phase_ == phase_type::retired) {
        throw std::logic_error("HTTP client response transport is retired");
    }
    phase_ = phase_type::active;
    charge_in_flight(sizeof(http_client_response_state));
    http_client_response_state* state_value = nullptr;
    try {
        state_value = new http_client_response_state(*this);
    } catch (...) {
        release_in_flight(sizeof(http_client_response_state));
        throw;
    }
    state_value->pool_ = &pool;
    attach_state(*state_value);
    return state_value;
}

void http_client_response_memory_domain::attach_state(http_client_response_state& state_value) noexcept {
    require_current();
    state_value.next_memory_state_ = state_head_;
    if (state_head_ != nullptr) {
        state_head_->previous_memory_state_ = &state_value;
    }
    state_head_ = &state_value;
    retain();
}

void http_client_response_memory_domain::destroy_state(http_client_response_state* state_value) noexcept {
    require_current();
    if (state_value == nullptr || &state_value->memory_domain_ != this) {
        std::terminate();
    }
    if (state_value->pool_ != nullptr) {
        unlink_state(*state_value);
    } else if (state_value->previous_memory_state_ != nullptr || state_value->next_memory_state_ != nullptr) {
        std::terminate();
    }
    delete state_value;
    release_in_flight(sizeof(http_client_response_state));
    release();
}

void http_client_response_memory_domain::unlink_state(http_client_response_state& state_value) noexcept {
    if (state_value.previous_memory_state_ != nullptr) {
        state_value.previous_memory_state_->next_memory_state_ = state_value.next_memory_state_;
    } else if (state_head_ == &state_value) {
        state_head_ = state_value.next_memory_state_;
    } else {
        std::terminate();
    }
    if (state_value.next_memory_state_ != nullptr) {
        state_value.next_memory_state_->previous_memory_state_ = state_value.previous_memory_state_;
    }
    state_value.previous_memory_state_ = nullptr;
    state_value.next_memory_state_ = nullptr;
}

void http_client_response_memory_domain::detach_transport_bindings(http_client_pool& pool) noexcept {
    require_current();
    phase_ = phase_type::retired;
    while (state_head_ != nullptr) {
        auto* state_value = state_head_;
        if (state_value->pool_ != &pool) {
            std::terminate();
        }
        // Revoke registration before notifying consumers. A continuation may
        // release this state or a sibling; never cache a next pointer across it.
        unlink_state(*state_value);
        state_value->detach_transport_bindings();
    }
}

http_client_response_state::http_client_response_state(http_client_response_memory_domain& memory_domain)
    : head_signal_(memory_domain.worker()),
      data_signal_(memory_domain.worker()),
      space_signal_(memory_domain.worker()),
      memory_domain_(memory_domain) {
    body_operation_scope_.bind_worker(memory_domain.worker());
}

void http_client_response_state::require_owner_worker(const char* message) const {
    if (!memory_domain_.worker().is_current()) {
        throw std::logic_error(message);
    }
}

void http_client_response_state::detach_transport_bindings() noexcept {
    if (tunnel_) {
        tunnel_->output_.stop();
    }
    if (upload_) {
        upload_->output_.stop();
    }
#ifdef RUVIA_ENABLE_HTTP2
    http2_data_credit_.reset();
#endif
#ifdef RUVIA_ENABLE_HTTP3
    http3_data_credit_.reset();
#endif
    connection_index_ = 0;
    connection_serial_ = 0;
    request_id_ = 0;
    cancellation_id_ = 0;
    stream_id_ = 0;
    pool_ = nullptr;
    transport_ = http_client_response_transport::unassigned;
    if (!complete_) {
        pool_closed_ = true;
        complete_ = true;
    }
    head_signal_.notify();
    data_signal_.notify();
    space_signal_.notify();
}

http_client_result_budget_domain::http_client_result_budget_domain(http_client_result_budget_config config)
    : max_retained_bytes_(config.max_retained_bytes_),
      max_in_flight_bytes_(config.max_in_flight_bytes_) {
    if (max_retained_bytes_ == 0 || max_in_flight_bytes_ == 0) {
        throw std::invalid_argument("HTTP client result byte budget must be greater than zero");
    }
}

bool http_client_result_budget_domain::reserve_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > max_in_flight_bytes_ - in_flight_bytes_) {
        return false;
    }
    in_flight_bytes_ += bytes_value;
    return true;
}

void http_client_result_budget_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > in_flight_bytes_) {
        std::terminate();
    }
    in_flight_bytes_ -= bytes_value;
}

bool http_client_result_budget_domain::try_reserve(std::size_t bytes_value) noexcept {
    auto retained = retained_bytes_.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes_value > max_retained_bytes_ - retained) {
            return false;
        }
        if (retained_bytes_.compare_exchange_weak(
                retained, retained + bytes_value, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void http_client_result_budget_domain::release(std::size_t bytes_value) noexcept {
    const auto previous = retained_bytes_.fetch_sub(bytes_value, std::memory_order_acq_rel);
    if (previous < bytes_value) {
        std::terminate();
    }
}

http_client_result_budget_lease::http_client_result_budget_lease(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept
    : domain_(domain),
      bytes_(bytes_value) {}

http_client_result_budget_lease::http_client_result_budget_lease(
    http_client_result_budget_lease&& other) noexcept
    : domain_(std::move(other.domain_)),
      bytes_(std::exchange(other.bytes_, 0)) {}

http_client_result_budget_lease::~http_client_result_budget_lease() {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
    }
}

std::optional<http_client_result_budget_lease> http_client_result_budget_lease::try_acquire(
    const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept {
    if (!domain->try_reserve(bytes_value)) {
        return std::nullopt;
    }
    return http_client_result_budget_lease(domain, bytes_value);
}

}  // namespace ruvia::detail

namespace ruvia {

http_client_response_bytes::http_client_response_bytes(
    std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value)
    : data_(reserve_bytes == 0 ? nullptr : std::make_unique_for_overwrite<std::byte[]>(reserve_bytes)) {
    auto [domain, reserved] = std::move(lease_value).release();
    budget_domain_ = std::move(domain);
    reserved_ = reserved;
}

http_client_response_bytes::http_client_response_bytes(http_client_response_bytes&& other) noexcept
    : budget_domain_(std::move(other.budget_domain_)),
      data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      reserved_(std::exchange(other.reserved_, 0)) {}

http_client_response_bytes& http_client_response_bytes::operator=(
    http_client_response_bytes&& other) noexcept {
    if (this != &other) {
        release();
        budget_domain_ = std::move(other.budget_domain_);
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
        reserved_ = std::exchange(other.reserved_, 0);
    }
    return *this;
}

http_client_response_bytes::~http_client_response_bytes() {
    release();
}

void http_client_response_bytes::release() noexcept {
    // Free the owned byte buffer before making its reservation available to another result.
    data_.reset();
    if (budget_domain_ != nullptr) {
        budget_domain_->release(reserved_);
        budget_domain_.reset();
    }
    size_ = 0;
    reserved_ = 0;
}

void http_client_response_bytes::append(std::span<const std::byte> bytes_value) noexcept {
    if (!bytes_value.empty()) {
        std::memcpy(data_.get() + size_, bytes_value.data(), bytes_value.size());
        size_ += bytes_value.size();
    }
}

}  // namespace ruvia

namespace ruvia::detail {

http_client_worker_limits::http_client_worker_limits(const http_client_worker_config& config)
    : max_idle_http1_(config.max_idle_http1_),
      max_idle_multiplexed_(config.max_idle_multiplexed_),
      max_dns_refreshes_(config.max_concurrent_dns_refreshes_) {
    if (max_dns_refreshes_ == 0) {
        throw std::invalid_argument("HTTP client DNS refresh concurrency must be greater than zero");
    }
}

void http_client_worker_limits::join(http_client_pool& pool) {
    if (std::ranges::find(pools_, &pool) == pools_.end()) {
        pools_.push_back(&pool);
    }
}

void http_client_worker_limits::leave(http_client_pool& pool) noexcept {
    std::erase(pools_, &pool);
}

void http_client_worker_limits::enforce_idle_limits() noexcept {
    enforce_idle_limit(false, max_idle_http1_);
    enforce_idle_limit(true, max_idle_multiplexed_);
}

void http_client_worker_limits::enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept {
    const auto total = [this, multiplexed] {
        std::size_t count = 0;
        for (const auto* pool : pools_) {
            count += pool->idle_count(multiplexed);
        }
        return count;
    };
    if (total() <= maximum) {
        return;
    }
    // Pool counters are upper bounds; recount before closing anything.
    for (auto* pool : pools_) {
        pool->recount_idle();
    }
    for (auto remaining = total(); remaining > maximum; --remaining) {
        http_client_pool* oldest = nullptr;
        std::optional<std::uint64_t> oldest_order;
        for (auto* pool : pools_) {
            const auto order = pool->least_recent_idle(multiplexed);
            if (order && (!oldest_order || *order < *oldest_order)) {
                oldest = pool;
                oldest_order = order;
            }
        }
        if (oldest == nullptr || !oldest->close_least_recent_idle(multiplexed)) {
            return;
        }
    }
}

bool http_client_worker_limits::try_begin_dns_refresh() noexcept {
    if (dns_refreshes_ == max_dns_refreshes_) {
        return false;
    }
    ++dns_refreshes_;
    return true;
}

void http_client_worker_limits::end_dns_refresh() noexcept {
    --dns_refreshes_;
}

http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config) {
    return {.result_budget_ = std::make_shared<http_client_result_budget_domain>(config.result_budget_),
        .limits_ = std::make_shared<http_client_worker_limits>(config)};
}

client_request_policy::client_request_policy(const http_client_config_storage& config)
    : config_(config),
      jar_(config.scheme_, config.host_,
          {.max_cookies_ = config.max_cookies_,
              .max_cookie_bytes_ = config.max_cookie_bytes_}) {
    for (const auto& cookie : config.cookies_) {
        jar_.add_configured_cookie(cookie.name(), cookie.value());
    }
}

bool client_request_policy::addresses_origin(const http_client_request_storage& request) const noexcept {
    return (!request.is_tunnel() || request.is_upgrade()) && request.route().alias_origin_;
}

http_client_request_view client_request_policy::prepare_request(
    const http_client_request_storage& request, client_prepared_request_scratch& scratch) {
    const auto& input_headers = request.headers();
    const auto max_prepared_headers = input_headers.size() + 2;
    http_header_view* prepared_begin = scratch.inline_headers_.data();
    if (max_prepared_headers > scratch.inline_headers_.size()) {
        scratch.overflow_headers_.resize(max_prepared_headers);
        prepared_begin = scratch.overflow_headers_.data();
    }
    std::size_t index = 0;
    bool add_user_agent = !config_.user_agent_.empty();
    scratch.overflow_cookie_header_.clear();
    std::size_t inline_cookie_size = 0;
    bool cookie_overflowed = false;
    // RFC 6265 section 4.2.1: cookie-pairs are joined by "; ".
    const auto ensure_cookie_space = [&](std::size_t count) -> char* {
        const auto current_size = cookie_overflowed ? scratch.overflow_cookie_header_.size() : inline_cookie_size;
        const auto separator_bytes = current_size == 0 ? std::size_t{0} : std::size_t{2};
        const auto total_needed = separator_bytes + count;
        if (total_needed == 0) {
            return scratch.inline_cookie_header_.data();
        }
        if (!cookie_overflowed && total_needed <= scratch.inline_cookie_header_.size() - inline_cookie_size) {
            char* cursor = scratch.inline_cookie_header_.data() + inline_cookie_size;
            if (separator_bytes != 0) {
                *cursor++ = ';';
                *cursor++ = ' ';
            }
            inline_cookie_size += total_needed;
            return cursor;
        }
        if (!cookie_overflowed) {
            scratch.overflow_cookie_header_.reserve(
                std::max(inline_cookie_size + total_needed, scratch.inline_cookie_header_.size() * 2));
            scratch.overflow_cookie_header_.append(scratch.inline_cookie_header_.data(), inline_cookie_size);
            cookie_overflowed = true;
        }
        const auto old_size = scratch.overflow_cookie_header_.size();
        if (old_size + total_needed > scratch.overflow_cookie_header_.capacity()) {
            scratch.overflow_cookie_header_.reserve(
                std::max(old_size + total_needed, scratch.overflow_cookie_header_.capacity() * 2));
        }
        scratch.overflow_cookie_header_.resize(old_size + total_needed);
        char* cursor = scratch.overflow_cookie_header_.data() + old_size;
        if (separator_bytes != 0) {
            *cursor++ = ';';
            *cursor++ = ' ';
        }
        return cursor;
    };
    // Request storage preserves caller field-name spelling.
    for (const auto& field : input_headers) {
        const auto name = field.name();
        if (add_user_agent && name.size() == 10U && http_ascii_equals_ignore_case(name, "user-agent")) {
            add_user_agent = false;
        }
        if (name.size() == 6U && http_ascii_equals_ignore_case(name, "cookie")) {
            const auto value = field.value();
            std::ranges::copy(value, ensure_cookie_space(value.size()));
            continue;
        }
        prepared_begin[index++] = http_header_view{name, field.value()};
    }

    // RFC 9110 §9.3.8: TRACE reflects the request, so stored credentials stay
    // out; caller-written Cookie fields above remain the caller's choice.
    // Stored cookies belong to the fixed origin (RFC 6265 §5.4): a tunnel to
    // another authority carries none. Empty jars have no expiry or path
    // constraints to inspect.
    if (!jar_.empty() && request.method() != "TRACE" && addresses_origin(request)) {
        const bool include_received =
            config_.received_cookies_ != http_client_received_cookie_policy::ignore;
        jar_.visit_matching_cookies(request.target(), include_received,
            std::chrono::system_clock::now(),
            [&](std::string_view name, std::string_view value) {
                const auto pair_size = cookie_request_pair_size(name, value);
                (void)write_cookie_request_pair(ensure_cookie_space(pair_size), name, value);
            });
    }
    const auto cookie_header_view =
        cookie_overflowed ? std::string_view(scratch.overflow_cookie_header_)
                          : std::string_view(scratch.inline_cookie_header_.data(), inline_cookie_size);
    if (add_user_agent) {
        prepared_begin[index++] = http_header_view{"user-agent", config_.user_agent_};
    }
    if (!cookie_header_view.empty()) {
        prepared_begin[index++] = http_header_view{"cookie", cookie_header_view};
    }
    http_client_request_view result;
    result.method_ = request.method();
    result.target_ = request.target();
    result.headers_ = std::span<const http_header_view>(prepared_begin, index);
    result.content_ = request.has_body() ? http_client_request_content_view::bytes(request.body())
                                         : http_client_request_content_view::none();
    return result;
}

void client_request_policy::retain_response_cookies(
    std::string_view request_target, std::span<const http_header> headers) {
    if (config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
        return;
    }
    jar_.retain_response_cookies(request_target, headers);
}

namespace {

// An encoded body must be collected to FIN before any bytes can be returned
// to the consumer.
void configure_http_client_response_decoding(http_client_response_state& state_value) {
    state_value.body_decode_required_ = false;
    state_value.content_coding_.reset();
    const auto* plan = state_value.response_body_plan_ ? &*state_value.response_body_plan_ : nullptr;
    // A 206 body (single range or multipart/byteranges) holds slices of the
    // encoded representation that cannot be decoded alone (RFC 9110 §14,
    // §15.3.7); it is exposed as received, with Content-Encoding kept.
    if (!state_value.decode_content_ || plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() ||
        plan->content_semantics() != http_response_content_semantics::with_content ||
        state_value.status_.status() == http_status::partial_content) {
        return;
    }
    // Parsed once with the final head; decoding reuses this result at FIN.
    auto& parsed_coding = state_value.content_coding_.emplace(
        parse_http_content_coding_headers(state_value.headers_));
    // Content codings are an open registry (RFC 9110 §8.4.1, §16.6.1), and a
    // request without Accept-Encoding accepts every coding (§12.5.3). A
    // syntactically valid stack this client cannot fully decode is therefore
    // exposed as received, with Content-Encoding kept, like a 206 body.
    if (const auto* unsupported = parsed_coding.unsupported()) {
        if (!unsupported->depth_exceeded()) {
            state_value.content_coding_.reset();
            return;
        }
    }
    const bool transforms_body = std::ranges::any_of(parsed_coding.codings(), [](http_content_coding coding) {
        return coding != http_content_coding::identity;
    });
    state_value.body_decode_required_ =
        parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr || transforms_body;
    if (state_value.body_decode_required_) {
        state_value.collect_all_ = true;
    } else {
        state_value.content_coding_.reset();
    }
}

// Uses the parser-computed body plan and keeps decoded output bounded by the
// response's configured byte limit.
void decode_http_client_response_body(http_client_response_state& state_value,
    bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (!content_semantics_present || !state_value.body_decode_required_) {
        return;
    }
    try {
        const auto& parsed_coding = *state_value.content_coding_;
        if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error),
                "invalid HTTP response Content-Encoding");
        }
        // Encoded bytes are never exposed, so the unread part of buffered_ and
        // pending_ together are the whole body. Decode in place when one holds it.
        const auto buffered_bytes = state_value.buffered_.size() - state_value.offset_;
        std::string joined;
        std::string_view encoded;
        if (state_value.pending_.empty()) {
            encoded = std::string_view(state_value.buffered_).substr(state_value.offset_);
        } else if (buffered_bytes == 0) {
            encoded = state_value.pending_;
        } else {
            if (state_value.pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_bytes) {
                throw http_client_error(make_error_code(http_client_errc::response_too_large),
                    "HTTP response exceeds configured byte limit");
            }
            joined.reserve(buffered_bytes + state_value.pending_.size());
            joined.append(state_value.buffered_.data() + state_value.offset_, buffered_bytes);
            joined.append(state_value.pending_);
            encoded = joined;
        }

        const auto decoded_limit = std::min(max_decoded_bytes, state_value.buffered_limit_);
        auto decoded = decode_http_content(
            parsed_coding.codings(), encoded, {.max_decoded_bytes_ = decoded_limit});
        if (auto* decoded_body = std::get_if<std::string>(&decoded)) {
            std::string empty_buffered;
            std::string empty_pending;
            state_value.buffered_.swap(empty_buffered);
            state_value.pending_.swap(empty_pending);
            state_value.buffered_.swap(*decoded_body);
            state_value.offset_ = 0;
            state_value.body_decode_required_ = false;
            state_value.content_coding_.reset();
            std::erase_if(state_value.headers_, [](const http_header& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), "Content-Encoding") ||
                       http_ascii_equals_ignore_case(field.name(), "Content-Length");
            });
            state_value.notify_producer_space();
            return;
        }
        const auto* failure = std::get_if<http_content_decode_error>(&decoded);
        if (failure != nullptr && *failure == http_content_decode_error::decoded_size_exceeded) {
            throw http_client_error(make_error_code(http_client_errc::response_too_large),
                "HTTP response exceeds configured byte limit");
        }
        throw http_client_error(make_error_code(http_client_errc::protocol_error),
            "invalid HTTP response Content-Encoding");
    } catch (...) {
        // Encoded bytes were never exposed: the head selected collection before
        // waking any consumer. Release that retained input on every decode error.
        state_value.content_coding_.reset();
        state_value.discard_response_body();
        throw;
    }
}

[[noreturn]] void throw_body_decoding_timeout() {
    throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP response body decoding timed out");
}

}  // namespace

void accept_extended_connect_websocket_response(http_client_websocket_handshake& websocket,
    http_status_code status, std::span<const http_header> headers, bool stream_ended,
    std::string_view error_message) {
    if (!status.is_successful() || stream_ended) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected), error_message, status);
    }
    const auto negotiated = websocket.negotiation_.validate_fields(headers);
    const auto* selected = std::get_if<websocket_client_negotiation_result_view>(&negotiated);
    if (selected == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected), error_message, status);
    }
    websocket.selected_subprotocol_.assign(selected->selected_subprotocol_);
    websocket.compression_ = selected->compression_;
}

void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head) {
    const auto status = head.status();
    state_value.content_length_ = head.content_length();
    auto fields = std::move(head).take_headers();
    state_value.headers_ = std::move(fields);
    const auto body_plan = plan_http_response_body(state_value.request_method_, status);
    state_value.framing_ = body_plan.content_semantics() == http_response_content_semantics::connect_tunnel
                               ? http_client_response_framing::tunnel
                           : body_plan.body_suppressed() ? http_client_response_framing::none
                                                         : http_client_response_framing::stream;
}

void publish_http_client_final_head(
    http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head) {
    if (state_value.upload_ && !state_value.upload_->output_.ended_) {
        const auto status = head.status_.status();
        const bool rewrite_upload_redirect =
            state_value.upload_->redirect_request_ != nullptr &&
            (status == http_status::see_other ||
                ((status == http_status::moved_permanently || status == http_status::found) &&
                    state_value.request_method_ == http_known_method::post));
        if (head.request_content_signal_ == http_client_request_content_signal::exchange_complete ||
            rewrite_upload_redirect) {
            if (rewrite_upload_redirect) {
                state_value.upload_->abandoned_ = true;
            }
            state_value.upload_->output_.stop();
        }
    }
    state_value.status_ = head.status_;
    state_value.protocol_version_ = head.protocol_version_;
    state_value.response_body_plan_ = plan_http_response_body(state_value.request_method_, state_value.status_.status());
    // A received final head changes the jar before consumers can issue
    // another request, regardless of body completion or abandonment.
    if (head.origin_target_) {
        policy.retain_response_cookies(*head.origin_target_, state_value.headers_);
    }
#ifdef RUVIA_ENABLE_HTTP3
    if (state_value.route_ != nullptr && state_value.pool_ != nullptr) {
        state_value.pool_->observe_alt_svc(*state_value.route_, state_value.headers_);
    }
#endif
    auto* const tunnel = state_value.tunnel_ ? &*state_value.tunnel_ : nullptr;
    if (tunnel != nullptr) {
        if (head.opens_tunnel_ && tunnel->udp_ &&
            validate_http_connect_udp_response(
                state_value.protocol_version_, state_value.status_.status().value(), state_value.headers_)
                .has_value()) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid CONNECT-UDP response head");
        }
        tunnel->accepted_ = head.opens_tunnel_;
        if (!tunnel->accepted_) {
            tunnel->output_.stop();
        }
    }
    if (tunnel == nullptr || !tunnel->accepted_) {
        configure_http_client_response_decoding(state_value);
    }
    state_value.head_ready_ = true;
    state_value.head_signal_.notify();
}

void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout,
    bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
    decode_http_client_response_body(state_value, content_semantics_present, max_decoded_bytes);
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
}

class http_client_advertisement_memory final {
public:
    explicit http_client_advertisement_memory(const worker_ref& worker_value)
        : worker_(worker_value) {}

    void retain(std::size_t bytes_value) noexcept {
        require_current();
        if (references_ == std::numeric_limits<std::size_t>::max() || bytes_value > std::numeric_limits<std::size_t>::max() - retained_bytes_) {
            std::terminate();
        }
        ++references_;
        retained_bytes_ += bytes_value;
        active_ = true;
    }
    void release(std::size_t bytes_value) noexcept {
        require_current();
        if (bytes_value > retained_bytes_ || references_ <= 1) {
            std::terminate();
        }
        retained_bytes_ -= bytes_value;
        --references_;
        if (retired_ && references_ == 1) {
            destroy();
        }
    }
    void retire() noexcept {
        if (active_) {
            require_current();
        }
        if (retired_) {
            std::terminate();
        }
        retired_ = true;
        if (references_ == 1) {
            destroy();
        }
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    void require_current() const noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
    }

private:
    void destroy() noexcept {
        if (retained_bytes_ != 0 || references_ != 1) {
            std::terminate();
        }
        delete (this);
    }
    worker_ref worker_;
    std::size_t references_{1};
    std::size_t retained_bytes_{};
    bool active_{};
    bool retired_{};
};

struct http_client_advertisement_state final {
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot,
        http_protocol_version protocol, const http_origin_advertisement& origins, std::size_t bytes_value)
        : memory_(owner_value),
          slot_(slot),
          protocol_(protocol),
          bytes_(bytes_value),
          value_(std::in_place_type<http_origin_advertisement>) {
        auto& retained = std::get<http_origin_advertisement>(value_);
        retained.origins_.reserve(origins.origins_.size());
        for (const auto& origin : origins.origins_) {
            retained.origins_.emplace_back(origin);
        }
        memory_.retain(bytes_value);
    }
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot,
        const http_alternative_service_advertisement& service, std::size_t bytes_value)
        : memory_(owner_value),
          slot_(slot),
          protocol_(http_protocol_version::http2),
          bytes_(bytes_value),
          value_(std::in_place_type<http_alternative_service_advertisement>) {
        auto& retained = std::get<http_alternative_service_advertisement>(value_);
        retained.stream_id_ = service.stream_id_;
        retained.origin_.assign(service.origin_);
        retained.field_value_.assign(service.field_value_);
        memory_.retain(bytes_value);
    }
    http_client_advertisement_memory& memory_;
    std::size_t slot_;
    http_protocol_version protocol_;
    std::size_t bytes_;
    std::variant<http_origin_advertisement, http_alternative_service_advertisement> value_;
};

namespace {
void release_advertisement(http_client_advertisement_state* state_value) noexcept {
    if (state_value == nullptr) {
        return;
    }
    auto& memory = state_value->memory_;
    memory.require_current();
    const auto bytes_value = state_value->bytes_;
    delete (state_value);
    memory.release(bytes_value);
}
}  // namespace

http_client_advertisement_queue::http_client_advertisement_queue(const worker_ref& worker_value,
    http_client_advertisement_config config)
    : worker_(worker_value),
      config_(config),
      pending_() {
    if (!worker_value.valid() || config.max_queued_advertisements_ == 0 || config.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement queue requires worker and positive bounds");
    }
    if (config.receive_origins_ || config.receive_alternative_services_) {
        memory_ = new http_client_advertisement_memory(worker_value);
    }
}
http_client_advertisement_queue::~http_client_advertisement_queue() {
    pending_.clear();
    if (memory_ != nullptr) {
        memory_->retire();
    }
}
void http_client_advertisement_queue::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}
bool http_client_advertisement_queue::admit(std::size_t bytes_value) noexcept {
    if (memory_ == nullptr || pending_.size() >= config_.max_queued_advertisements_ ||
        bytes_value > config_.max_retained_bytes_ - std::min(config_.max_retained_bytes_, memory_->retained_bytes())) {
        ++dropped_;
        return false;
    }
    return true;
}
void http_client_advertisement_queue::retain(std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins) {
    require_current();
    if (!config_.receive_origins_) {
        return;
    }
    if (origins.origins_.size() > (config_.max_retained_bytes_ / sizeof(std::string))) {
        ++dropped_;
        return;
    }
    auto bytes_value = sizeof(http_client_advertisement_state) + origins.origins_.size() * sizeof(std::string);
    for (const auto& origin : origins.origins_) {
        if (origin.size() > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return;
        }
        bytes_value += origin.size();
    }
    if (!admit(bytes_value)) {
        return;
    }
    auto owned = std::make_unique<http_client_advertisement_state>(*memory_, slot, protocol, origins, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
}
void http_client_advertisement_queue::retain(std::size_t slot, const http_alternative_service_advertisement& service) {
    require_current();
    if (!config_.receive_alternative_services_) {
        return;
    }
    auto bytes_value = sizeof(http_client_advertisement_state);
    for (const auto size : {service.origin_.size(), service.field_value_.size()}) {
        if (size > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return;
        }
        bytes_value += size;
    }
    if (!admit(bytes_value)) {
        return;
    }
    auto owned = std::make_unique<http_client_advertisement_state>(*memory_, slot, service, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
}
std::optional<http_client_advertisement> http_client_advertisement_queue::next() {
    require_current();
    if (pending_.empty()) {
        return std::nullopt;
    }
    std::optional<http_client_advertisement> advertisement(std::in_place, std::move(pending_.front()));
    pending_.pop_front();
    return advertisement;
}
void http_client_advertisement_queue::retire() noexcept {
    require_current();
    pending_.clear();
    if (auto* memory = std::exchange(memory_, nullptr)) {
        memory->retire();
    }
}

}  // namespace ruvia::detail

namespace ruvia {
http_client_advertisement::http_client_advertisement(http_client_advertisement&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
http_client_advertisement& http_client_advertisement::operator=(http_client_advertisement&& other) noexcept {
    if (this != &other) {
        detail::release_advertisement(state_);
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_client_advertisement::~http_client_advertisement() {
    detail::release_advertisement(state_);
}
http_protocol_version http_client_advertisement::protocol_version() const noexcept {
    return state_ == nullptr ? http_protocol_version::http11 : state_->protocol_;
}
std::size_t http_client_advertisement::connection_slot() const noexcept {
    return state_ == nullptr ? 0 : state_->slot_;
}
const http_origin_advertisement* http_client_advertisement::origins() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_origin_advertisement>(&state_->value_);
}
const http_alternative_service_advertisement* http_client_advertisement::alternative_service() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_alternative_service_advertisement>(&state_->value_);
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

// One transport read: what an idle keep-alive connection keeps for the next
// response. Larger growth (long heads, a busy tunnel) goes back to the worker.
constexpr std::size_t connection_read_buffer_retained_bytes = std::size_t{16} * 1024;

[[nodiscard]] bool alpn_allows_http1(client_alpn alpn) noexcept {
    return alpn == client_alpn::http11 || alpn == client_alpn::negotiate;
}

[[nodiscard]] bool alpn_allows_http2(client_alpn alpn) noexcept {
    return alpn == client_alpn::http2 || alpn == client_alpn::negotiate;
}

[[nodiscard]] client_tcp_endpoint http_client_endpoint(http_client_pool& pool, std::string_view dial_host,
    std::uint16_t dial_port, std::string_view server_name, client_alpn alpn) noexcept {
    const auto& config = pool.config();
    const bool https = config.scheme_ == http_scheme::https;
    return {.host_ = dial_host,
        .port_ = dial_port,
        .tls_ = https ? &config.tls_ : nullptr,
        .tcp_ = config.tcp_,
        .alpn_ = https ? client_alpn_bytes(alpn) : std::span<const unsigned char>{},
        .identity_host_ = server_name,
        .resolve_context_ = &pool,
        .resolve_ = [](void* context, client_host_resolver& resolver, const operation_deadline& timeout, std::string_view host, std::uint16_t port) -> task<client_resolve_result> {
            return static_cast<http_client_pool*>(context)->resolve_addresses(host, port, resolver, timeout);
        }};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial,
    std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route,
    client_alpn alpn)
    : dial_host_(dial_host),
      server_name_(server_name),
      transport_(pool.io_context(), pool.tls_context(), pool.worker(),
          http_client_endpoint(pool, dial_host_, dial_port, server_name_, alpn), pool.wire_counters()),
      read_buffer_(),
      write_buffer_(),
#ifdef RUVIA_ENABLE_HTTP2
      http2_(*this, transport_, pool.tasks(), pool.worker(), pool.config().http2_ping_interval_),
#endif
      connect_scheduler_(1, pool.worker()),
      http1_scheduler_(1, pool.worker()),
      alpn_(alpn),
      pool_(pool),
      index_(index),
      serial_(serial),
      alias_route_(alias_route),
#ifdef RUVIA_ENABLE_HTTP2
      http2_streams_(),
#endif
      idle_watch_signal_(pool.worker()) {
}

http_client_connection::~http_client_connection() = default;

bool http_client_connection::serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name,
    client_alpn alpn) const noexcept {
    if (fresh_ || retired_ || address_retired_ || transport_.endpoint().port_ != dial_port ||
        !http_ascii_equals_ignore_case(dial_host_, dial_host) || !http_ascii_equals_ignore_case(server_name_, server_name)) {
        return false;
    }
    // Another offer also reuses a connection whose negotiated protocol it
    // permits; an unsettled one only serves its own offer.
    return alpn_ == alpn || (protocol_ == protocol_type::http1 && alpn_allows_http1(alpn)) ||
           (protocol_ == protocol_type::http2 && alpn_allows_http2(alpn));
}

bool http_client_connection::idle() const noexcept {
    if (!connected_ || retired_) {
        return false;
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (protocol_ == protocol_type::http2) {
        return http2_.usable() && http2_streams_.empty();
    }
#endif
    return protocol_ == protocol_type::http1 && http1_operations_ == 0 && idle_watch_wanted_;
}

bool http_client_connection::reclaimable() const noexcept {
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running() || !http2_streams_.empty() || state_waiters_ != nullptr) {
        return false;
    }
#endif
    return users_ == 0 && !connected_ && http1_operations_ == 0 && !idle_watcher_started_;
}

#ifdef RUVIA_ENABLE_HTTP2
void http_client_connection::on_http2_capacity() noexcept {
    pool_.admission_changed();
}

void http_client_connection::on_http2_stopped() noexcept {
    pool_.connection_retired();
}
#endif

void http_client_connection::capture_connection_info(http_client_response_state& response, bool reused) const {
    if (response.connection_info_.id_ != connection_id_) {
        response.connection_info_.peer_ip_ = peer_ip_;
        response.connection_info_.peer_port_ = transport_.peer_endpoint().port();
        response.connection_info_.id_ = connection_id_;
    }
    response.connection_info_.reused_ = reused;
    response.connection_info_.connect_duration_ = reused ? std::chrono::steady_clock::duration{} : transport_.connect_duration();
    response.connection_info_.tls_duration_ = reused ? std::chrono::steady_clock::duration{} : transport_.tls_duration();
}

void http_client_connection::retire_addresses() noexcept {
    address_retired_ = true;
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running()) {
        http2_.mark_draining();
        for (auto* stream : http2_streams_) {
            if (stream->state_->tunnel_) {
                cancel_http2_stream(stream->request_id_, client_abort_reason::closing);
            }
        }
        if (http2_streams_.empty()) {
            close_idle();
        }
        return;
    }
#endif
    if (http1_response_ == nullptr || http1_response_->tunnel_) {
        close();
    }
}

#ifdef RUVIA_ENABLE_HTTP2
std::size_t http_client_connection::available_http2_request_streams() const noexcept {
    return protocol_ == protocol_type::http2
               ? http2_.available_request_streams(
                     pool_.config().max_concurrent_http2_streams_per_connection_)
               : 0;
}
#endif

void http_client_connection::throw_if_aborted() const {
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        throw_http_client_abort(reason);
    }
}

void http_client_connection::abort_http1_response() noexcept {
    if (http1_response_ == nullptr) {
        return;
    }
    http1_response_->request_informational_stop();
    if (auto* output = http1_response_->output()) {
        output->stop();
    }
    http1_response_->space_signal_.notify();
}

void http_client_connection::close() noexcept {
    end_idle();
    abort_http1_response();
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running()) {
        http2_.fail({.status_ = client_transport_status::io_failed,
            .error_ = std::make_error_code(std::errc::operation_canceled)});
        return;
    }
#endif
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_streams_.empty()) {
        // Registered streams may still borrow the retired connection object.
        http2_.reset();
    }
#endif
    read_buffer_.clear();
    write_buffer_.clear();
    // A parked idle watcher ends with the connection.
    idle_watch_signal_.notify();
    pool_.connection_retired();
}

void http_client_connection::begin_idle() noexcept {
    if (!connected_) {
        return;
    }
    if (address_retired_ || fresh_) {
        close_idle();
        return;
    }
    if (protocol_ == protocol_type::http1 && !idle_watch_wanted_) {
        idle_watch_wanted_ = true;
        if (idle_watcher_started_) {
            idle_watch_signal_.notify();
        } else {
            try {
                pool_.tasks().spawn(watch_idle_http1());
                idle_watcher_started_ = true;
            } catch (...) {
                // An unwatched idle connection is not kept.
                close();
                return;
            }
        }
    }
    const auto& idle_timeout = pool_.config().idle_timeout_;
    if (!idle_timeout || idle_timer_.registered()) {
        return;
    }
    try {
        (void)pool_.worker().schedule_timer(idle_timer_, deadline_after(idle_timeout->value()),
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    close_idle();
                }
            });
    } catch (...) {
        // An idle connection without its bound is not kept.
        close();
    }
}

void http_client_connection::end_idle() noexcept {
    idle_order_ = 0;
    idle_timer_.cancel();
    if (std::exchange(idle_watch_wanted_, false)) {
        ++idle_period_;
        // An idle HTTP/1 connection has no other pending operation, and the
        // next one starts only after this returns.
        if (std::exchange(idle_wait_pending_, false)) {
            transport_.cancel_wait();
        }
    }
}

void http_client_connection::close_idle() noexcept {
    // Every use of the connection ends the idle bound first; these hold only
    // for a connection nothing reused.
    if (!connected_ || http1_response_ != nullptr) {
        return;
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (!http2_streams_.empty()) {
        return;
    }
    if (protocol_ == protocol_type::http2 && http2_.running()) {
        end_idle();
        close_http2_session();
        return;
    }
#endif
    close();
}

#ifdef RUVIA_ENABLE_HTTP2
void http_client_connection::close_http2_session() noexcept {
    const client_transport_result closed{
        .status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::operation_canceled)};
    // Without a running generation that can send GOAWAY it fails at once.
    (void)http2_.begin_close(closed);
}
#endif

task<void> http_client_connection::watch_idle_http1() {
    // Pingora's idle_poll: an HTTP/1 peer sends nothing between exchanges, so
    // readiness of an idle connection is its EOF, a reset or unsolicited bytes,
    // and the connection closes at once instead of waiting for reuse or
    // idle_timeout_. Readiness is awaited on the raw socket without running
    // the TLS engine; idle_status() then classifies it.
    // Parked between idle periods while connected; a closed connection's
    // watcher ends so the pool can reclaim the owner.
    while (!retired_ && connected_) {
        if (!idle_watch_wanted_) {
            co_await idle_watch_signal_.wait();
            continue;
        }
        const auto period = idle_period_;
        idle_wait_pending_ = true;
        co_await transport_.wait_readable();
        if (period != idle_period_ || retired_) {
            // end_idle() cancelled the wait: the connection is in use or closed.
            continue;
        }
        idle_wait_pending_ = false;
        const auto status = transport_.idle_status();
        if (status == client_idle_transport_status::reusable) {
            continue;
        }
        if (status == client_idle_transport_status::pending_data) {
            pool_.record_idle_connection_with_pending_data();
        }
        close();
    }
    idle_watcher_started_ = false;
    pool_.connection_retired();
}

void http_client_connection::retain_idle_buffers() noexcept {
    // Unread bytes belong to the transport; only an empty buffer is trimmed.
    if (read_buffer_.empty()) {
        read_buffer_.clear_retaining_small(connection_read_buffer_retained_bytes);
    }
    write_buffer_.clear_retaining_small();
}

void http_client_connection::shutdown() noexcept {
    retired_ = true;
    (void)connect_scheduler_.close();
    (void)http1_scheduler_.close();
    idle_watch_signal_.notify();
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running() && !http2_.failed() && http2_streams_.empty()) {
        // No connect or HTTP/1 exchange runs beside a live session.
        end_idle();
        close_http2_session();
        return;
    }
#endif
    transport_.abort(client_abort_reason::closing);
    abort_http1_response();
    close();
}

void http_client_connection::cancel(std::uint64_t generation, client_abort_reason reason) noexcept {
    if (generation_ != generation) {
        return;
    }
    transport_.abort(reason);
    abort_http1_response();
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
#ifdef RUVIA_ENABLE_HTTP2
    http2_.wake_writer();
    http2_.state_signal().notify();
#endif
}

bool http_client_connection::cancel_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id_ == cancellation_id) {
        cancellation_id_ = 0;
        cancel(generation_, client_abort_reason::cancelled);
        return true;
    }
#ifdef RUVIA_ENABLE_HTTP2
    for (auto* waiter = state_waiters_; waiter; waiter = waiter->next_) {
        if (waiter->cancellation_id_ == cancellation_id) {
            http2_.state_signal().notify();
            return true;
        }
    }
    const auto stream = std::ranges::find_if(http2_streams_,
        [cancellation_id](const http2_stream* registered) { return registered->cancellation_id_ == cancellation_id; });
    if (stream == http2_streams_.end()) {
        return false;
    }
    (*stream)->cancellation_id_ = 0;
    cancel_http2_stream((*stream)->request_id_, client_abort_reason::cancelled);
    return true;
#else
    return false;
#endif
}

task<bool> http_client_connection::ensure_connected(const operation_deadline& operation_timeout_value,
    const operation_deadline& acquire_timeout, stop_token stop_token_value,
    http_client_response_state& response) {
    const auto acquired = co_await connect_scheduler_.acquire({.timeout_ = acquire_timeout.timeout(), .stop_token_ = stop_token_value});
    if (!acquired.has_value()) {
        throw_http_client_wait_failure(acquired.status(), "HTTP client connect wait");
    }
    // However the connect ends, requests waiting for this route's protocol re-check.
    struct admission_publication final {
        http_client_pool& pool_;
        ~admission_publication() {
            pool_.admission_changed();
        }
    } publication{pool_};
    const auto connect_slot = acquired.value();
    struct connect_lease final {
        pool_lease_scheduler& scheduler_;
        std::size_t slot_;
        ~connect_lease() {
            scheduler_.release(slot_);
        }
    } connect_lease_value{connect_scheduler_, connect_slot};
    if (response.abandoned_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP response was abandoned");
    }
    end_idle();
    if (address_retired_ && protocol_ != protocol_type::http2) {
        close();
    }
    if (retired_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client connection is retired");
    }
    if (connected_) {
#ifdef RUVIA_ENABLE_HTTP2
        if (protocol_ == protocol_type::http2) {
            if (http2_.usable()) {
                co_return true;
            }
            // A draining session retires once its last stream left.
        } else
#endif
        {
            const auto idle_status = transport_.idle_status();
            if (idle_status == client_idle_transport_status::reusable) {
                co_return true;
            }
            if (idle_status == client_idle_transport_status::pending_data) {
                pool_.record_idle_connection_with_pending_data();
            }
            close();
        }
    }
    if (stop_token_value.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client connect cancelled");
    }
#ifdef RUVIA_ENABLE_HTTP2
    // A previous HTTP/2 generation may still be draining or unwinding its
    // drivers and streams; they borrow the transport that connect() replaces.
    // Waiting for them is part of acquiring this slot, and must not abort
    // their transport, so it precedes this connect's cancellation binding.
    if (http2_.running() || !http2_streams_.empty()) {
        if (http2_streams_.empty()) {
            // A session that admits no request and has no stream left.
            close_http2_session();
        }
        co_await wait_for_http2_session_stop(acquire_timeout, stop_token_value, response);
        if (retired_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client connection is retired");
        }
    }
#endif
    transport_.reset_abort();
    next_nonzero_id(generation_);
    struct connect_generation final {
        http_client_connection& connection_;
        ~connect_generation() {
            next_nonzero_id(connection_.generation_);
        }
    } connect_generation_value{*this};
    http_client_response_cancellation cancellation(pool_.cancellation_target(), cancellation_id_, response);
    cancellation.arm(stop_token_value);
    throw_if_aborted();
#ifdef RUVIA_ENABLE_HTTP2
    http2_.reset();
#endif
    if (pool_.prefers_http11() && alpn_ == client_alpn::negotiate) {
        // The origin required HTTP/1.1 (RFC 9113 section 7): later connects
        // offer only http/1.1, like Pingora's prefer_h1.
        transport_.offer_alpn(client_alpn::http11);
    }
    const auto timeout = operation_timeout_value.constrained_by(response.effective_connect_timeout_);
    struct connect_attempt final {
        http_client_connection& connection_;
        bool committed_{false};
        ~connect_attempt() {
            if (!committed_) {
                connection_.close();
            }
        }
    } attempt{*this};
    const auto connected = co_await transport_.connect(timeout);
    if (transport_.peer_endpoint().port() != 0) {
        connection_id_ = pool_.next_connection_id();
        peer_ip_ = to_ip_address(transport_.peer_endpoint().address()).normalized_ipv4();
        capture_connection_info(response, false);
    }
    if (!connected.ok()) {
        throw_http_client_transport_error(connected, "upstream closed while connecting");
    }
#ifdef RUVIA_ENABLE_HTTP2
    const auto& config = pool_.config();
    if (transport_.secure()) {
        const auto alpn = transport_.alpn();
        if (alpn_ == client_alpn::http2 && alpn != "h2") {
            throw http_client_error(
                make_error_code(http_client_errc::protocol_unavailable), "upstream did not negotiate HTTP/2");
        }
        protocol_ = alpn == "h2" ? protocol_type::http2 : protocol_type::http1;
    } else {
        // Plaintext HTTP/2 is prior knowledge (RFC 9113 section 3.3).
        protocol_ = alpn_ == client_alpn::http2 ? protocol_type::http2 : protocol_type::http1;
    }
#else
    protocol_ = protocol_type::http1;
#endif
    connected_ = true;
    address_retired_ = false;
#ifdef RUVIA_ENABLE_HTTP2
    if (protocol_ != protocol_type::http2) {
        attempt.committed_ = true;
        co_return false;
    }
    const auto started = co_await http2_.start({.enable_push_ = config.push_.enabled_,
                                                   .receive_origin_advertisements_ = config.advertisements_.receive_origins_},
        timeout);
    switch (started.status_) {
        case http2_client_session_start_status::started:
            http2_session_requests_ = 0;
            attempt.committed_ = true;
            co_return false;
        case http2_client_session_start_status::transport_failed:
            throw_http_client_transport_error(started.transport_, "upstream closed during HTTP/2 preface");
        case http2_client_session_start_status::protocol_failed:
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP/2 connection preface");
    }
    std::terminate();
#else
    attempt.committed_ = true;
    co_return false;
#endif
}

}  // namespace ruvia::detail

namespace ruvia {
namespace detail {

http_client_websocket_handshake::http_client_websocket_handshake(
    websocket_client_negotiation_config_view config)
    : negotiation_(config) {
    std::array<std::uint8_t, 16> nonce;
    if (!fill_random_bytes(nonce)) {
        throw std::runtime_error("failed to generate WebSocket handshake key");
    }
    http_base64_encode(key_.data(), nonce);
}

http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : upload_ != nullptr ? &upload_->output_
                                                                       : nullptr;
}

http_client_request_storage::http_client_request_storage(std::string_view method,
    std::string_view target, std::span<const http_header_view> fields)
    : http_client_request_storage(method, target) {
    if (fields.empty()) {
        return;
    }
    headers_.reserve(fields.size());
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value());
    }
}

http_client_request_storage::http_client_request_storage(
    std::string_view method, std::string_view target)
    : method_(method),
      target_(target),
      route_authority_(method_) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept
    : options_(std::move(other.options_)),
      informational_(std::move(other.informational_)),
      pool_owner_(std::move(other.pool_owner_)),
      method_(std::move(other.method_)),
      target_(std::move(other.target_)),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)),
      tunnel_authority_(std::move(other.tunnel_authority_)),
      tunnel_protocol_(std::move(other.tunnel_protocol_)),
      websocket_(std::move(other.websocket_)),
      host_stop_(std::move(other.host_stop_)),
      is_tunnel_(other.is_tunnel_),
      is_upgrade_(other.is_upgrade_),
      tunnel_(std::exchange(other.tunnel_, nullptr)),
      has_body_(std::exchange(other.has_body_, false)),
      upload_(std::exchange(other.upload_, nullptr)),
      route_authority_(other.route_authority_) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(method_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(target_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(body_)>);
}

void http_client_request_storage::set_tunnel(std::string_view authority, std::string_view protocol) {
    tunnel_authority_.assign(authority);
    tunnel_protocol_.assign(protocol);
    is_tunnel_ = true;
}

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

namespace {
// host[:port] without userinfo, like the authority of base_url_.
[[nodiscard]] http_authority_view parse_route_authority(std::string_view authority) {
    const auto parsed = parse_http_authority(borrowed_text(authority));
    if (!parsed || authority.find_first_of("@/?#") != std::string_view::npos || parsed->host().empty() ||
        parsed->port_kind() == http_authority_port_kind::empty || parsed->port() == 0) {
        throw std::invalid_argument("HTTP request authority must be host[:port]");
    }
    auto host = parsed->host();
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    validate_client_origin_host(host, "HTTP request authority host must not be empty", "HTTP request authority host is invalid");
    return *parsed;
}
}  // namespace

void validate_http_client_request_options(const http_client_request_options& options) {
    if (options.field_source_ != http_request_field_source::generated &&
        options.field_source_ != http_request_field_source::forwarded) {
        throw std::invalid_argument("invalid HTTP request field source");
    }
    validate_http_client_route_options(options);
}

std::shared_ptr<http_client_informational_callback> share_http_client_informational(
    http_client_informational_callback&& callback) {
    if (!callback) {
        return nullptr;
    }
    return std::make_shared<http_client_informational_callback>(std::move(callback));
}

void validate_http_client_route_options(const http_client_request_options& options) {
    if (options.authority_) {
        (void)parse_route_authority(*options.authority_);
    }
    if (options.dial_host_) {
        validate_client_origin_host(*options.dial_host_, "HTTP request dial host must not be empty", "HTTP request dial host is invalid");
    }
    if (options.dial_port_ && options.dial_port_->value() == 0) {
        throw std::invalid_argument("HTTP request dial port must be positive");
    }
    if (options.server_name_) {
        validate_client_origin_host(*options.server_name_, "HTTP request TLS server name must not be empty", "HTTP request TLS server name is invalid");
    }
    if (options.versions_) {
        // The alias scheme is checked with the route.
        validate_http_client_versions(*options.versions_, http_scheme::https);
    }
}

http_client_request_options copy_http_client_request_settings(const http_client_request_options& options) {
    // Binding every member keeps this copy exhaustive: a new option fails to compile here.
    const auto& [decode_content, field_source, fresh_connection, timeouts, informational, authority, dial_host, dial_port,
        server_name, versions] = options;
    (void)informational;
    return {.decode_content_ = decode_content,
        .field_source_ = field_source,
        .fresh_connection_ = fresh_connection,
        .timeouts_ = timeouts,
        .informational_ = {},
        .authority_ = authority,
        .dial_host_ = dial_host,
        .dial_port_ = dial_port,
        .server_name_ = server_name,
        .versions_ = versions};
}

void http_client_request_storage::resolve_route(const http_client_config_storage& config) {
    validate_http_client_route_options(options_);
    const bool https = config.scheme_ == http_scheme::https;
    if (options_.server_name_ && !https) {
        throw std::invalid_argument("HTTP request TLS server name requires an https origin");
    }
    if (!config.fixed_origin_ && !options_.authority_) {
        throw std::invalid_argument("HTTP client alias without a fixed origin requires authority_ on each request");
    }
    const port_number default_port = https ? 443 : 80;
    std::optional<http_origin_view> authority_origin;
    std::string_view authority_host;
    if (options_.authority_) {
        const auto parsed = parse_route_authority(*options_.authority_);
        const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(default_port)};
        authority_origin = https ? http_origin_view::https(origin) : http_origin_view::http(origin);
        route_authority_ = make_http_origin_authority(*authority_origin);
        authority_host = parsed.host();
        if (authority_host.starts_with('[')) {
            authority_host = authority_host.substr(1, authority_host.size() - 2);
        }
    }
    const auto origin_value = authority_origin ? *authority_origin : config.origin();
    http_client_route route{.origin_ = origin_value,
        .authority_ = authority_origin ? std::string_view(route_authority_) : std::string_view(config.authority_),
        // An alias without a fixed origin (or dial target) dials the authority.
        .dial_host_ = config.dial_host_.empty() ? authority_host : std::string_view(config.dial_host_),
        .dial_port_ = config.dial_port_ != 0 ? config.dial_port_ : origin_value.port(),
        .server_name_ = {},
        .alias_origin_ = true,
        .alias_transport_ = true,
        .versions_ = options_.versions_.value_or(config.versions_)};
    if (options_.versions_) {
        validate_http_client_versions(*options_.versions_, config.scheme_);
    }
    if (authority_origin) {
        route.alias_origin_ = config.fixed_origin_ && http_authorities_equal(borrowed_text(route.authority_),
                                                          borrowed_text(std::string_view(config.authority_)), default_port);
    }
    if (is_tunnel_ && !is_upgrade_ && options_.authority_) {
        if (websocket_ != nullptr) {
            // The alias authority named by the WebSocket handshake follows the override.
            tunnel_authority_.assign(route.authority_);
        } else if (tunnel_protocol_.empty() || !http_authorities_equal(borrowed_text(std::string_view(tunnel_authority_)), borrowed_text(route.authority_), default_port)) {
            throw std::invalid_argument("a CONNECT request names its own authority");
        }
    }
    if (options_.dial_host_) {
        route.dial_host_ = *options_.dial_host_;
    }
    if (options_.dial_port_) {
        route.dial_port_ = options_.dial_port_->value();
    }
    const auto alias_server_name = https ? config.tls_.tls_host(config.host_) : std::string_view{};
    route.server_name_ = options_.server_name_           ? std::string_view(*options_.server_name_)
                         : !route.alias_origin_ && https ? authority_host
                                                         : alias_server_name;
    route.alias_transport_ = config.fixed_origin_ && http_ascii_equals_ignore_case(route.dial_host_, config.dial_host_) &&
                             route.dial_port_ == config.dial_port_ && http_ascii_equals_ignore_case(route.server_name_, alias_server_name);
    route_.emplace(route);
}

}  // namespace detail

namespace {
void append_query_component(std::string& output, std::string_view value) {
    for (const unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            output.push_back(static_cast<char>(byte));
        } else {
            http_append_percent_byte(output, byte);
        }
    }
}

void replace_field(std::vector<http_header>& fields, std::string_view name, std::string_view value) {
    http_header replacement(name, value);
    const auto found = std::ranges::find_if(fields, [name](const http_header& field) {
        return http_ascii_equals_ignore_case(field.name(), name);
    });
    if (found == fields.end()) {
        fields.push_back(std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

bool hop_field(std::string_view name) noexcept {
    return http_ascii_equals_ignore_case(name, "connection") || http_ascii_equals_ignore_case(name, "keep-alive") ||
           http_ascii_equals_ignore_case(name, "proxy-connection") || http_ascii_equals_ignore_case(name, "transfer-encoding") ||
           http_ascii_equals_ignore_case(name, "proxy-authenticate") || http_ascii_equals_ignore_case(name, "proxy-authorization") ||
           http_ascii_equals_ignore_case(name, "te") || http_ascii_equals_ignore_case(name, "trailer") ||
           http_ascii_equals_ignore_case(name, "upgrade");
}

bool connection_named_field(std::span<const http_header> fields, std::string_view name) {
    for (const auto& field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "connection")) {
            continue;
        }
        auto tokens = field.value();
        while (!tokens.empty()) {
            const auto comma = tokens.find(',');
            auto token = tokens.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
                token.remove_prefix(1);
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
                token.remove_suffix(1);
            }
            if (http_ascii_equals_ignore_case(token, name)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            tokens.remove_prefix(comma + 1);
        }
    }
    return false;
}

// One bound for the whole forward: commit, body and end share the deadline
// started here and the caller's stop token.
task<void> pipe_response(http_client_response response, detail::http_client_response_state& state,
    response_stream_writer& output, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client operation cancelled", std::nullopt, &state.connection_info_, state.request_written_);
    }
    const detail::operation_steps steps(std::move(operation));
    stream_commit_result committed{};
    try {
        committed = co_await output.commit(state.downstream_step(steps));
    } catch (...) {
        state.rethrow_downstream_failure(steps);
    }
    if (committed == stream_commit_result::body) {
        co_await state.consume_body<void>(state.downstream_step(steps), &output);
        std::vector<http_header_view> trailers;
        trailers.reserve(response.trailers().size());
        for (const auto& field : response.trailers()) {
            if (!hop_field(field.name()) && !connection_named_field(response.headers(), field.name())) {
                trailers.emplace_back(field.name(), field.value());
            }
        }
        try {
            co_await output.end(trailers, state.downstream_step(steps));
        } catch (...) {
            state.rethrow_downstream_failure(steps);
        }
    }
}

// The origin a request addresses: its authority override, else the alias.
// Redirect targets resolve against it.
[[nodiscard]] http_origin_view request_origin(const http_client_ref& client, const http_client_request_options& options) {
    if (!options.authority_) {
        return client.origin();
    }
    const auto parsed = detail::parse_route_authority(*options.authority_);
    const bool https = client.scheme() == http_scheme::https;
    const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(https ? 443 : 80)};
    return https ? http_origin_view::https(origin) : http_origin_view::http(origin);
}

void clear_route_overrides(http_client_request_options& options) noexcept {
    options.authority_.reset();
    options.dial_host_.reset();
    options.dial_port_.reset();
    options.server_name_.reset();
}

// A task frame may bind exactly one scope. This outer frame binds the
// destination while its child remains bound to the source response.
task<void> pipe_response_in_destination(task<void> source) {
    co_await std::move(source);
}
}  // namespace

http_client_request::http_client_request(const http_client_ref& client, std::string_view method, std::string_view path)
    : client_(client),
      method_(method),
      target_(path),
      headers_(),
      body_() {
    client_.registration_.require_active();
    if (!is_valid_http_method_token(method) ||
        (!is_valid_http_origin_form_target(path) && !(method == "OPTIONS" && path == "*"))) {
        throw std::invalid_argument("HTTP request requires a valid method and relative origin-form target");
    }
}

http_client_request http_client_ref::request(std::string_view method, std::string_view path) const {
    registration_.require_active();
    return http_client_request(*this, method, path);
}
http_client_request http_client_ref::get(std::string_view path) const {
    return request("GET", path);
}
http_client_request http_client_ref::post(std::string_view path) const {
    return request("POST", path);
}
http_client_request http_client_ref::put(std::string_view path) const {
    return request("PUT", path);
}
http_client_request http_client_ref::patch(std::string_view path) const {
    return request("PATCH", path);
}
http_client_request http_client_ref::del(std::string_view path) const {
    return request("DELETE", path);
}
http_client_request http_client_ref::head(std::string_view path) const {
    return request("HEAD", path);
}
http_client_request http_client_ref::options(std::string_view path) const {
    return request("OPTIONS", path);
}

http_client_request& http_client_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_request& http_client_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_request& http_client_request::query(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (target_ == "*") {
        throw std::invalid_argument("asterisk target cannot contain query parameters");
    }
    const auto old_size = target_.size();
    const char separator = target_.find('?') == std::string_view::npos ? '?' : '&';
    try {
        target_.push_back(separator);
        append_query_component(target_, name);
        target_.push_back('=');
        append_query_component(target_, value);
    } catch (...) {
        target_.resize(old_size);
        throw;
    }
    return *this;
}
http_client_request& http_client_request::body(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    std::string owned(value);
    body_.swap(owned);
    has_body_ = true;
    return *this;
}
http_client_request& http_client_request::body(std::span<const std::byte> value) {
    return body(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
}
http_client_request& http_client_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_request& http_client_request::on_redirect(http_client_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::on_redirect(
    http_client_async_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    async_redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::redirects(std::size_t maximum) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    max_redirects_ = maximum;
    return *this;
}

task<http_client_tunnel_result> http_client_request::upgrade(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid HTTP Upgrade tunnel policy");
    }
    const detail::inline_http_header_views<16> fields(headers_);
    detail::http_client_request_storage request(method_, target_, fields.view());
    request.options_ = detail::copy_http_client_request_settings(request_options_);
    request.informational_ = informational_callback_;
    request.pool_owner_ = client_.pool_owner_;
    request.set_host_stop(client_.host_stop_token_);
    request.set_upgrade();
    if (has_body_) {
        request.set_body(body_);
    }
    auto operation = make_scoped_operation(client_.registration_.scope(),
        client_.pool_->open_tunnel(std::move(request), config, client_.options_));
    consumed_ = true;
    std::string(method_.get_allocator()).swap(method_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    std::string(body_.get_allocator()).swap(body_);
    return operation;
}

std::vector<http_header_view> http_client_request::field_views() const {
    std::vector<http_header_view> fields;
    fields.reserve(headers_.size());
    for (const auto& field : headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    return fields;
}

task<http_client_response> http_client_request::send() {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), send_owned(std::move(*this)));
}

namespace {

[[nodiscard]] std::string format_origin_target_url(
    const http_origin_view& origin, std::string_view target) {
    const auto authority = make_http_origin_authority(origin);
    const std::string_view scheme_prefix =
        origin.scheme() == http_scheme::https ? "https://" : "http://";
    std::string url;
    url.reserve(scheme_prefix.size() + authority.size() + target.size());
    url.append(scheme_prefix);
    url.append(authority);
    if (!target.empty() && target.front() != '/' && target != "*") {
        url.push_back('/');
    }
    url.append(target);
    return url;
}

}  // namespace

task<http_client_response> http_client_request::send_owned(http_client_request request) {
    // One deadline spans every redirect hop; each hop receives it as an absolute bound.
    const operation_deadline total(
        request.client_.options_.timeout_.has_value()
            ? request.client_.options_.timeout_
            : std::optional<operation_timeout>(request.client_.pool_->config().request_timeout_));
    co_return co_await follow_redirects(std::move(request), std::nullopt, total, false);
}

task<http_client_response> http_client_request::follow_redirects(http_client_request request,
    std::optional<http_client_response> initial_response, const operation_deadline& total,
    bool upload_initial_hop) {
    for (std::size_t redirects = 0;; ++redirects) {
        std::optional<http_client_response> hop_response;
        if (redirects == 0 && initial_response.has_value()) {
            hop_response.emplace(std::move(*initial_response));
            initial_response.reset();
        } else {
            request.client_.registration_.require_active();
            if (total.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out),
                    "HTTP redirect deadline expired", std::nullopt, nullptr, redirects != 0,
                    redirects);
            }
            request.client_.options_.timeout_ = total.timeout();
            std::optional<task<http_client_response>> dispatched;
            {
                const detail::inline_http_header_views<16> fields(request.headers_);
                const http_client_request_view view{
                    .method_ = request.method_,
                    .target_ = request.target_,
                    .headers_ = fields.view(),
                    .content_ = request.has_body_
                                    ? http_client_request_content_view::bytes(request.body_)
                                    : http_client_request_content_view::none()};
                dispatched.emplace(request.client_.dispatch(
                    view, request.request_options_, request.informational_callback_));
            }
            if (redirects == request.max_redirects_) {
                std::string(request.method_.get_allocator()).swap(request.method_);
                std::string(request.target_.get_allocator()).swap(request.target_);
                std::vector<http_header>(request.headers_.get_allocator())
                    .swap(request.headers_);
                std::string(request.body_.get_allocator()).swap(request.body_);
                try {
                    co_return co_await std::move(*dispatched);
                } catch (const http_client_error& error) {
                    if (redirects != 0 && error.redirect_hops() == 0) {
                        throw http_client_error(error.code(), error.what(), error.status(),
                            error.connection_info(), error.request_written(), redirects);
                    }
                    throw;
                }
            }
            try {
                hop_response.emplace(co_await std::move(*dispatched));
            } catch (const http_client_error& error) {
                if (redirects != 0 && error.redirect_hops() == 0) {
                    throw http_client_error(error.code(), error.what(), error.status(),
                        error.connection_info(), error.request_written(), redirects);
                }
                throw;
            }
        }

        auto response = std::move(*hop_response);
        if (redirects == request.max_redirects_) {
            co_return response;
        }
        const auto status = response.status().value();
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            co_return response;
        }
        const http_client_request_view current_view{
            .method_ = request.method_,
            .target_ = request.target_,
            .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_)
                                          : http_client_request_content_view::none()};
        const auto plan = plan_http_client_redirect_request(
            current_view, {.status_ = response.status()});
        if (upload_initial_hop && redirects == 0 &&
            plan.content_disposition() != http_client_redirect_content_disposition::drop) {
            co_return response;
        }

        std::optional<std::string_view> location;
        for (const auto& field : response.headers()) {
            if (http_ascii_equals_ignore_case(field.name(), "location")) {
                if (location) {
                    throw http_client_error(make_error_code(http_client_errc::protocol_error),
                        "repeated redirect Location", std::nullopt, &response.connection_info(),
                        true, redirects);
                }
                location = field.value();
            }
        }
        if (!location) {
            co_return response;
        }
        const auto current_origin = request_origin(request.client_, request.request_options_);
        auto resolved = resolve_http_client_redirect_target(current_origin,
            {.current_target_ = request.target_,
                .location_ = *location});
        auto* next = std::get_if<http_client_resolved_redirect>(&resolved);
        if (next == nullptr) {
            co_return response;
        }

        if (request.redirect_observer_ || request.async_redirect_observer_) {
            const auto source_url = format_origin_target_url(
                current_origin, request.target_);
            const auto target_url = format_origin_target_url(
                next->origin(), next->target());
            const http_client_redirect_hop hop_event(redirects, current_origin, request.target_,
                source_url, response.connection_info(), response.status(), *location,
                next->origin(), next->target(), target_url, next->cross_origin());
            if (request.redirect_observer_) {
                if (!request.redirect_observer_(hop_event)) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
            if (request.async_redirect_observer_) {
                stop_source deadline_stop;
                stop_group cancellation{request.client_.options_.stop_token_,
                    request.client_.host_stop_token_, deadline_stop.token()};
                worker_timer_registration timer;
                if (const auto deadline_value = total.deadline()) {
                    (void)request.client_.pool_->worker().schedule_timer(timer, *deadline_value,
                        [&deadline_stop](worker_timer_outcome outcome) noexcept {
                            if (outcome == worker_timer_outcome::expired) {
                                deadline_stop.request_stop();
                            }
                        });
                }
                const auto require_live_observer = [&]() {
                    if (total.expired()) {
                        throw http_client_error(std::make_error_code(std::errc::timed_out),
                            "HTTP redirect policy deadline expired", std::nullopt,
                            &response.connection_info(), true, redirects);
                    }
                    if (cancellation.token().stop_requested()) {
                        throw http_client_error(
                            std::make_error_code(std::errc::operation_canceled),
                            "HTTP redirect policy cancelled", std::nullopt,
                            &response.connection_info(), true, redirects);
                    }
                };
                require_live_observer();
                const auto accepted =
                    co_await request.async_redirect_observer_(hop_event, cancellation.token());
                require_live_observer();
                if (!accepted) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
        }

        const bool has_custom_policy = static_cast<bool>(request.redirect_observer_) ||
                                       static_cast<bool>(request.async_redirect_observer_);
        auto next_pool = request.client_.pool_owner_;
        if (next->cross_origin()) {
            if (!has_custom_policy && request.client_.scheme() == http_scheme::https &&
                next->scheme() != http_scheme::https) {
                co_return response;
            }
            auto authority =
                make_http_origin_authority(next->origin());
            // Route overrides belong to the origin that set them.
            clear_route_overrides(request.request_options_);
            if (!request.client_.pool_->config().fixed_origin_ && has_custom_policy &&
                next->scheme() == request.client_.scheme()) {
                // An alias without a fixed origin reaches the accepted origin itself.
                request.request_options_.authority_.emplace(std::string_view(authority));
            } else {
                if (request.client_.registry_ == nullptr) {
                    co_return response;
                }
                next_pool = request.client_.registry_->find_origin(next->scheme(), authority);
                if (next_pool == nullptr) {
                    co_return response;
                }
            }
        }

        if (upload_initial_hop && redirects == 0 && response.state_ != nullptr &&
            response.state_->upload_) {
            response.state_->upload_->abandoned_ = true;
            response.state_->upload_->output_.stop();
        }

        request.method_.assign(plan.method());
        request.target_.assign(next->target());
        const bool drop =
            plan.content_disposition() == http_client_redirect_content_disposition::drop;
        std::erase_if(request.headers_, [&](const http_header& field) {
            const auto name = field.name();
            return http_ascii_equals_ignore_case(name, "host") ||
                   (next->cross_origin() &&
                       (http_ascii_equals_ignore_case(name, "authorization") ||
                           http_ascii_equals_ignore_case(name, "proxy-authorization") ||
                           http_ascii_equals_ignore_case(name, "cookie"))) ||
                   (drop && (http_ascii_equals_ignore_case(name.substr(0, 8), "content-") ||
                                http_ascii_equals_ignore_case(name, "digest") ||
                                http_ascii_equals_ignore_case(name, "repr-digest") ||
                                http_ascii_equals_ignore_case(name, "transfer-encoding") ||
                                http_ascii_equals_ignore_case(name, "trailer") ||
                                http_ascii_equals_ignore_case(name, "expect")));
        });
        if (drop) {
            request.has_body_ = false;
            std::string(request.body_.get_allocator()).swap(request.body_);
        }
        request.client_.pool_owner_ = std::move(next_pool);
        request.client_.pool_ = request.client_.pool_owner_.get();
    }
}

task<http_client_exchange> http_client_request::upload(http_client_upload_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (has_body_) {
        throw std::invalid_argument("streamed upload cannot also have a buffered body");
    }
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 ||
        config.continue_timeout_.value() <= std::chrono::milliseconds::zero() ||
        (config.expectation_ != http_client_request_expectation::none &&
            config.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    consumed_ = true;
    return make_scoped_operation(
        client_.registration_.scope(), upload_owned(std::move(*this), config));
}

task<http_client_exchange> http_client_request::upload_owned(
    http_client_request request, http_client_upload_config config) {
    std::optional<task<http_client_exchange>> dispatched;
    {
        const detail::inline_http_header_views<16> fields(request.headers_);
        detail::http_client_request_storage storage(
            request.method_, request.target_, fields.view());
        storage.options_ = detail::copy_http_client_request_settings(request.request_options_);
        storage.informational_ = request.informational_callback_;
        storage.pool_owner_ = request.client_.pool_owner_;
        storage.set_host_stop(request.client_.host_stop_token_);
        dispatched.emplace(
            request.client_.pool_->open_request(std::move(storage), config, request.client_.options_));
    }
    auto exchange = co_await std::move(*dispatched);
    if (request.max_redirects_ != 0 && exchange.body().state_ != nullptr &&
        exchange.body().state_->upload_) {
        exchange.body().state_->upload_->redirect_request_ =
            std::make_unique<http_client_request>(std::move(request));
    }
    co_return exchange;
}

http_client_connect_request::http_client_connect_request(const http_client_ref& client,
    std::string_view authority, std::string_view target, bool udp)
    : client_(client),
      authority_(authority),
      protocol_(),
      target_(target),
      headers_(),
      udp_(udp) {}
http_client_connect_request http_client_ref::connect(std::string_view authority) const {
    registration_.require_active();
    return http_client_connect_request(*this, authority, {}, false);
}
http_client_connect_request http_client_ref::udp(std::string_view path) const {
    registration_.require_active();
    return http_client_connect_request(*this, pool_->config().authority_, path, true);
}
http_client_connect_request& http_client_connect_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_connect_request& http_client_connect_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_connect_request& http_client_connect_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_connect_request& http_client_connect_request::protocol(std::string_view value) {
    if (consumed_ || udp_) {
        throw std::logic_error("CONNECT protocol cannot be changed");
    }
    protocol_.assign(value);
    return *this;
}
http_client_connect_request& http_client_connect_request::target(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    target_.assign(value);
    return *this;
}
task<http_client_tunnel_result> http_client_connect_request::open(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const detail::inline_http_header_views<16> fields(headers_);
    auto dispatched = udp_
                          ? client_.dispatch_udp({.target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_)
                          : client_.dispatch_connect(
                                {.authority_ = authority_, .protocol_ = protocol_, .target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_);
    consumed_ = true;
    std::string(authority_.get_allocator()).swap(authority_);
    std::string(protocol_.get_allocator()).swap(protocol_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    return dispatched;
}

task<void> http_client_response::pipe_to(context& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.stream(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to(request_stream& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.response(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to_destination(context_response_editor draft, response_stream_writer& writer,
    ::ruvia::operation_scope& destination, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    state_->require_owner_worker("HTTP response requires its owner worker");
    if (!state_->body_operation_scope_.active() || !destination.active()) {
        throw std::logic_error("HTTP response pipe_to scope is closed");
    }
    draft.status(status());
    const auto fields = headers();
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!hop_field(field->name()) && !connection_named_field(fields, field->name()) &&
            !http_ascii_equals_ignore_case(field->name(), "content-length")) {
            const bool repeated = std::any_of(fields.begin(), field, [&](const http_header& previous) {
                return http_ascii_equals_ignore_case(previous.name(), field->name());
            });
            draft.header(field->name(), field->value(),
                {.mode_ = repeated ? http_response_header_mode::append : http_response_header_mode::replace});
        }
    }
    auto source = make_scoped_operation(state_->body_operation_scope_,
        pipe_response(http_client_response(state_), *state_, writer, std::move(operation)));
    return make_scoped_operation(destination, pipe_response_in_destination(std::move(source)));
}

#ifdef RUVIA_ENABLE_HTTP3
namespace {
task<void> migrate_client_pool(std::shared_ptr<detail::http_client_pool> pool,
    std::string address, std::uint16_t port, operation_options options, stop_token host_stop) {
    co_await pool->migrate_http3(std::move(address), port, std::move(options), std::move(host_stop));
}
}  // namespace
#endif

task<void> http_client_ref::migrate(std::string_view local_address, std::uint16_t port) const {
    registration_.require_active();
#ifdef RUVIA_ENABLE_HTTP3
    return make_scoped_operation(registration_.scope(),
        migrate_client_pool(pool_owner_, std::string(local_address), port, options_, host_stop_token_));
#else
    (void)local_address;
    (void)port;
    throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 support is disabled");
#endif
}

const std::error_category& http_client_category() noexcept {
    class category final : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "ruvia.http_client";
        }
        std::string message(int value) const override {
            switch (static_cast<http_client_errc>(value)) {
                case http_client_errc::not_configured:
                    return "HTTP client is not configured";
                case http_client_errc::invalid_request:
                    return "invalid HTTP request";
                case http_client_errc::resolve_failed:
                    return "HTTP name resolution failed";
                case http_client_errc::connect_failed:
                    return "HTTP connection failed";
                case http_client_errc::tls_failed:
                    return "HTTP TLS handshake failed";
                case http_client_errc::protocol_unavailable:
                    return "HTTP protocol unavailable";
                case http_client_errc::io_error:
                    return "HTTP I/O failed";
                case http_client_errc::protocol_error:
                    return "HTTP protocol error";
                case http_client_errc::response_too_large:
                    return "HTTP response too large";
                case http_client_errc::queue_full:
                    return "HTTP request queue full";
                case http_client_errc::closing:
                    return "HTTP client closing";
                case http_client_errc::result_budget_exceeded:
                    return "HTTP result budget exceeded";
                case http_client_errc::handshake_rejected:
                    return "HTTP handshake rejected";
                case http_client_errc::message_too_large:
                    return "WebSocket message too large";
            }
            return "unknown HTTP client error";
        }
    };
    static const category instance;
    return instance;
}

namespace {

constexpr std::size_t response_body_read_chunk_bytes = std::size_t{16} * 1024;

void check_response_operation_affinity(void* target) noexcept {
    const auto& state_value = *static_cast<const detail::http_client_response_state*>(target);
    if (!state_value.memory_domain().worker().is_current()) {
        std::terminate();
    }
}

detail::http_client_response_state& require_response_state(
    detail::http_client_response_state* state_value) {
    if (state_value == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    return *state_value;
}

detail::http_client_response_state& require_response_body_state(
    detail::http_client_response_state* state_value) {
    auto& state = require_response_state(state_value);
    check_response_operation_affinity(&state);
    return state;
}

task<void> refresh_client_dns(std::shared_ptr<detail::http_client_pool> pool, operation_options options, stop_token host_stop) {
    stop_group cancellation(host_stop, options.stop_token_);
    options.stop_token_ = cancellation.token();
    co_await pool->refresh_dns(std::move(options));
}

[[nodiscard]] ip_address to_ruvia_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()))
        .normalized_ipv4();
}

[[nodiscard]] task<std::vector<ip_address>> resolve_dns_owned(worker_ref worker,
    std::string host, operation_options options,
    std::shared_ptr<const http_client_address_validator> validator = nullptr,
    std::uint16_t validator_port = 80) {
    const operation_deadline timeout(options.timeout_);
    if (options.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled),
            "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out),
            "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    detail::client_host_resolver resolver(worker);
    auto pending = resolver.resolve(host, validator_port, timeout, options.stop_token_);
    detail::client_resolve_result result;
    if (auto* immediate = std::get_if<detail::client_resolve_result>(&pending)) {
        result = std::move(*immediate);
    } else {
        result = co_await std::move(std::get<task<detail::client_resolve_result>>(pending));
    }
    if (options.stop_token_.stop_requested() || result.error_ == asio::error::operation_aborted) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled),
            "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired() || result.error_ == asio::error::timed_out) {
        throw http_client_error(std::make_error_code(std::errc::timed_out),
            "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    if (result.error_ == asio::error::no_buffer_space) {
        throw http_client_error(make_error_code(http_client_errc::queue_full),
            "HTTP DNS resolution blocking pool queue full", std::nullopt, nullptr, false);
    }
    if (result.error_ || (!result.literal_ && result.addresses_.empty())) {
        throw http_client_error(make_error_code(http_client_errc::resolve_failed),
            "HTTP DNS resolution failed", std::nullopt, nullptr, false);
    }
    std::vector<ip_address> addresses;
    const auto append_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        const auto resolved_ip = to_ruvia_ip_address(endpoint.address());
        if (validator && !(*validator)(resolved_ip, endpoint.port())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed),
                "HTTP dial address rejected", std::nullopt, nullptr, false);
        }
        addresses.push_back(resolved_ip);
    };
    if (result.literal_) {
        append_endpoint(*result.literal_);
    } else {
        addresses.reserve(result.addresses_.size());
        for (const auto& entry : result.addresses_) {
            append_endpoint(entry.endpoint());
        }
    }
    std::ranges::sort(addresses);
    const auto unique_range = std::ranges::unique(addresses);
    addresses.erase(unique_range.begin(), unique_range.end());
    co_return addresses;
}

[[nodiscard]] task<std::vector<ip_address>> resolve_client_handle_dns(
    std::shared_ptr<detail::http_client_pool> pool, std::string host,
    operation_options handle_options, operation_options call_options, stop_token host_stop) {
    stop_group cancellation{host_stop, handle_options.stop_token_, call_options.stop_token_};
    std::optional<operation_timeout> effective_timeout = handle_options.timeout_;
    if (call_options.timeout_) {
        if (!effective_timeout ||
            call_options.timeout_->duration() < effective_timeout->duration()) {
            effective_timeout = call_options.timeout_;
        }
    }
    if (!effective_timeout) {
        effective_timeout = operation_timeout(pool->config().connect_timeout_);
    }
    co_return co_await resolve_dns_owned(pool->worker(), std::move(host),
        {.timeout_ = effective_timeout, .stop_token_ = cancellation.token()},
        pool->config().address_validator_, pool->config().dial_port_);
}
}  // namespace

http_client_response::http_client_response(detail::http_client_pool& pool)
    : state_(pool.response_memory().create_state(pool)),
      body_(state_) {
    const auto& config = pool.config();
    state_->effective_connect_timeout_ = config.connect_timeout_;
    state_->effective_read_timeout_ = config.read_timeout_;
    state_->effective_write_timeout_ = config.write_timeout_;
    state_->effective_acquire_timeout_ = config.acquire_timeout_;
    state_->decode_content_ = config.decode_content_;
}

http_client_response::http_client_response(detail::http_client_response_state* state_value) noexcept
    : state_(state_value),
      body_(state_value),
      consumer_(false) {
    if (state_ != nullptr) {
        state_->retain_reference();
    }
}

http_client_response::http_client_response(http_client_response&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      body_(state_),
      consumer_(other.consumer_) {
    other.body_.state_ = nullptr;
}

http_client_response& http_client_response::operator=(http_client_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    state_ = std::exchange(other.state_, nullptr);
    consumer_ = other.consumer_;
    body_.state_ = state_;
    other.body_.state_ = nullptr;
    other.consumer_ = false;
    return *this;
}

http_client_response::~http_client_response() {
    release();
}

void http_client_response::release() noexcept {
    if (state_ == nullptr) {
        return;
    }
    check_response_operation_affinity(state_);
    auto* state_value = std::exchange(state_, nullptr);
    body_.state_ = nullptr;
    if (consumer_) {
        state_value->body_operation_scope_.close();
    }
    if (consumer_ && !state_value->complete_ && !state_value->abandoned_ && state_value->pool_ != nullptr) {
        state_value->pool_->abandon_response(*state_value);
    }
    state_value->release_reference();
}

std::span<const http_client_informational_response> http_client_response::informational_responses() const& {
    return require_response_state(state_).informational_;
}

http_status_code http_client_response::status() const {
    return require_response_state(state_).status_.status();
}
http_received_status http_client_response::received_status() const {
    return require_response_state(state_).status_;
}
http_protocol_version http_client_response::protocol_version() const {
    return require_response_state(state_).protocol_version_;
}
http_client_response_framing http_client_response::framing() const {
    return require_response_state(state_).framing_;
}
std::optional<std::uint64_t> http_client_response::content_length() const {
    return require_response_state(state_).content_length_;
}
const http_client_connection_info& http_client_response::connection_info() const& {
    return require_response_state(state_).connection_info_;
}
void http_client_response::reprioritize(http_priority priority) & {
    auto& state_value = require_response_state(state_);
    check_response_operation_affinity(&state_value);
    if (state_value.complete_) {
        // The peer already finished the stream; there is nothing left to schedule.
        return;
    }
    if (state_value.abandoned_ || state_value.pool_ == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::closing), "response transport is retired", std::nullopt,
            state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    state_value.pool_->reprioritize(state_value, priority);
}
std::span<const http_header> http_client_response::headers() const& {
    return require_response_state(state_).headers_;
}
std::span<const http_header> http_client_response::trailers() const& {
    return require_response_state(state_).trailers_;
}

void detail::http_client_response_state::retain_reference() noexcept {
    check_response_operation_affinity(this);
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void detail::http_client_response_state::release_reference() noexcept {
    check_response_operation_affinity(this);
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        memory_domain_.destroy_state(this);
    }
}

void detail::http_client_response_state::notify_producer_space() noexcept {
    space_signal_.notify();
}

void detail::http_client_response_state::rethrow_response_failure() const {
    if (failure_) {
        try {
            std::rethrow_exception(failure_);
        } catch (const http_client_error& error) {
            if (error.connection_info() == nullptr && connection_info_.id_ != 0) {
                throw http_client_error(error.code(), error.what(), error.status(), &connection_info_, request_written_);
            }
            throw;
        }
    }
}

task<void> detail::http_client_response_state::dispatch_informational(const operation_deadline& timeout) {
    while (informational_delivered_ < informational_.size()) {
        const auto index = informational_delivered_++;
        auto callback = informational_callback_;
        if (!callback || !*callback) {
            continue;
        }
        if (!informational_abort_) {
            informational_abort_.emplace();
        }
        if (informational_stop_requested_) {
            informational_abort_->request_stop();
        }
        stop_source deadline_stop;
        stop_group cancellation{informational_stop_, informational_abort_->token(), deadline_stop.token()};
        worker_timer_registration timer;
        if (const auto deadline = timeout.deadline()) {
            (void)memory_domain_.worker().schedule_timer(timer, *deadline,
                [&deadline_stop](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        deadline_stop.request_stop();
                    }
                });
        }
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "informational callback deadline expired");
        }
        if (cancellation.token().stop_requested()) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "informational callback cancelled");
        }
        // 每头字段独立拥有；外层 vector 的移动不改变字段 span 的地址。
        co_await (*callback)(informational_[index].status(), informational_[index].headers(), cancellation.token());
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "informational callback deadline expired");
        }
        if (cancellation.token().stop_requested()) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "informational callback cancelled");
        }
    }
}

task<void> detail::http_client_response_state::await_head(std::string_view head_message) {
    while ((!head_ready_ || informational_active_) && !failure_ && !pool_closed_) {
        throw_if_read_interrupted();
        co_await head_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), head_message, std::nullopt,
            connection_info_.id_ != 0 ? &connection_info_ : nullptr);
    }
    if ((tunnel_ && tunnel_->accepted_) || !body_decode_required_) {
        co_return;
    }
    if (pool_ != nullptr) {
        pool_->release_response_data(*this);
    }
    notify_producer_space();
    while (!complete_) {
        throw_if_read_interrupted();
        co_await data_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client encoded response failed", std::nullopt,
            connection_info_.id_ != 0 ? &connection_info_ : nullptr);
    }
}

namespace {
[[noreturn]] void throw_interrupted(const detail::http_client_response_state& state_value,
    detail::operation_interrupt_cause cause, std::string_view timed_out, std::string_view cancelled) {
    const bool timeout = cause == detail::operation_interrupt_cause::timed_out;
    throw http_client_error(timeout ? std::make_error_code(std::errc::timed_out) : std::make_error_code(std::errc::operation_canceled),
        timeout ? timed_out : cancelled, std::nullopt,
        state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
}
}  // namespace

void detail::http_client_response_state::throw_if_read_interrupted() const {
    if (read_interrupted()) {
        throw_interrupted(*this, *read_operation_->cause(), "HTTP client read timed out", "HTTP client read cancelled");
    }
}

operation_options detail::http_client_response_state::downstream_step(const operation_steps& steps) const {
    auto step = steps.try_next();
    if (!step.has_value()) {
        throw_interrupted(*this, operation_interrupt_cause::timed_out, "HTTP client read timed out", "HTTP client read cancelled");
    }
    return std::move(*step);
}

void detail::http_client_response_state::rethrow_downstream_failure(const operation_steps& steps) const {
    try {
        throw;
    } catch (const std::system_error& error) {
        if (steps.interrupted(error)) {
            throw_interrupted(*this,
                error.code() == std::errc::timed_out ? operation_interrupt_cause::timed_out : operation_interrupt_cause::cancelled,
                "HTTP client read timed out", "HTTP client read cancelled");
        }
        throw;
    }
}

void detail::http_client_response_state::throw_if_output_interrupted() const {
    if (output_operation_ != nullptr && output_operation_->interrupted()) {
        throw_interrupted(*this, *output_operation_->cause(), "HTTP client write timed out", "HTTP client write cancelled");
    }
}

detail::http_client_operation_interruption::http_client_operation_interruption(http_client_response_state& state,
    http_client_operation_lane lane, const operation_options& operation) try
    : state_(state),
      lane_(lane),
      interrupt_(state.interrupt_source_, state.memory_domain().worker(), operation, &state,
          lane == http_client_operation_lane::read ? &wake_reader : &abort_output,
          lane == http_client_operation_lane::read ? "HTTP client read" : "HTTP client write") {
    (lane == http_client_operation_lane::read ? state.read_operation_ : state.output_operation_) = &interrupt_;
} catch (const std::system_error&) {
    // The only system_error at construction is an already-stopped token.
    throw_interrupted(state, operation_interrupt_cause::cancelled, {}, "HTTP client operation cancelled");
}

detail::http_client_operation_interruption::~http_client_operation_interruption() {
    (lane_ == http_client_operation_lane::read ? state_.read_operation_ : state_.output_operation_) = nullptr;
}

void detail::http_client_operation_interruption::wake_reader(void* state) noexcept {
    auto& state_value = *static_cast<http_client_response_state*>(state);
    state_value.head_signal_.notify();
    state_value.data_signal_.notify();
}

void detail::http_client_operation_interruption::abort_output(void* state) noexcept {
    auto& state_value = *static_cast<http_client_response_state*>(state);
    if (auto* queue = state_value.output()) {
        queue->stop();
    }
    if (state_value.pool_ != nullptr && !state_value.complete_ && !state_value.abandoned_) {
        state_value.pool_->abandon_response(state_value);
    }
}

void detail::http_client_response_state::discard_response_body() noexcept {
    {
        std::string empty_buffered;
        std::string empty_pending;
        buffered_.swap(empty_buffered);
        pending_.swap(empty_pending);
    }
    offset_ = 0;
    notify_producer_space();
}

void detail::http_client_response_state::discard_failed_encoded_response() noexcept {
    if (!complete_ && body_decode_required_) {
        // Encoded bytes are withheld until the complete representation decodes.
        content_coding_.reset();
        discard_response_body();
    }
}

void detail::http_client_response_state::release_consumed_body_prefix() {
    if (offset_ > buffered_.size()) {
        std::terminate();
    }
    // A partly consumed buffer keeps its prefix behind the cursor: compacting
    // it on every chunk would move the unread tail once per read. Producers
    // only append to pending_, so the retained prefix never limits them.
    if (offset_ == 0 || offset_ != buffered_.size()) {
        return;
    }
    http_clear_string_retaining_small(buffered_, response_body_read_chunk_bytes);
    offset_ = 0;
    if (pending_.empty() && buffered_.capacity() > pending_.capacity()) {
        pending_.swap(buffered_);
    }
    notify_producer_space();
}

void detail::http_client_response_state::promote_pending_data() {
    auto& state_value = *this;
    if (state_value.offset_ != state_value.buffered_.size() || state_value.pending_.empty()) {
        return;
    }
    if (state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    http_clear_string_retaining_small(state_value.buffered_, response_body_read_chunk_bytes);
    state_value.offset_ = 0;
    state_value.buffered_.swap(state_value.pending_);
    state_value.notify_producer_space();
}

bool http_client_response_body::complete() const {
    if (state_ == nullptr) {
        return true;
    }
    if (!state_->receive_complete() || state_->offset_ != state_->buffered_.size() ||
        !state_->pending_.empty()) {
        return false;
    }
    state_->rethrow_response_failure();
    if (state_->pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt,
            state_->connection_info_.id_ != 0 ? &state_->connection_info_ : nullptr);
    }
    return true;
}

task<std::optional<std::span<const std::byte>>> http_client_response_body::read(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(
        state_value.body_operation_scope_, state_value.consume_body<std::span<const std::byte>>(std::move(operation)));
}

task<std::optional<std::string_view>> http_client_response_body::text(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(
        state_value.body_operation_scope_, state_value.consume_body<std::string_view>(std::move(operation)));
}

template <typename view>
task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::http_client_response_state::consume_body(operation_options operation, response_stream_writer* output) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    [[maybe_unused]] const operation_steps steps(operation);
    const http_client_operation_interruption interruption(state_value, http_client_operation_lane::read, operation);
    for (;;) {
        // A forward is bounded as a whole: its bound also ends it between
        // chunks that are already buffered.
        if constexpr (std::is_void_v<view>) {
            state_value.throw_if_read_interrupted();
        }
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state_value.release_consumed_body_prefix();
        while ((state_value.body_decode_required_ && !state_value.receive_complete()) ||
               (state_value.buffered_.empty() && state_value.pending_.empty() && !state_value.receive_complete())) {
            state_value.throw_if_read_interrupted();
            co_await state_value.data_signal_.wait();
        }
        promote_pending_data();
        if (state_value.offset_ == state_value.buffered_.size()) {
            state_value.rethrow_response_failure();
            if (state_value.pool_closed_) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw http_client_error(make_error_code(http_client_errc::closing), message, std::nullopt,
                    state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
            }
            state_value.discard_response_body();
            if constexpr (std::is_void_v<view>) {
                co_return;
            } else {
                co_return std::nullopt;
            }
        }
        const auto count = std::min(response_body_read_chunk_bytes, state_value.buffered_.size() - state_value.offset_);
        const auto chunk = std::string_view(state_value.buffered_).substr(state_value.offset_, count);
        if constexpr (std::is_void_v<view>) {
            try {
                co_await output->write(std::as_bytes(std::span(chunk.data(), chunk.size())), state_value.downstream_step(steps));
            } catch (...) {
                state_value.rethrow_downstream_failure(steps);
            }
            state_value.offset_ += count;
        } else {
            state_value.offset_ += count;
            if constexpr (std::same_as<view, std::string_view>) {
                co_return chunk;
            } else {
                co_return std::as_bytes(std::span(chunk.data(), chunk.size()));
            }
        }
    }
}

template task<std::optional<std::span<const std::byte>>> detail::http_client_response_state::consume_body<std::span<const std::byte>>(operation_options, response_stream_writer*);
template task<std::optional<std::string_view>> detail::http_client_response_state::consume_body<std::string_view>(operation_options, response_stream_writer*);
template task<void> detail::http_client_response_state::consume_body<void>(operation_options, response_stream_writer*);

task<http_client_response_bytes> http_client_response_body::read_all(std::size_t max_bytes, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(
        state_value.body_operation_scope_, state_value.read_all(max_bytes, std::move(operation)));
}

task<http_client_response_bytes> detail::http_client_response_state::read_all(std::size_t max_bytes, operation_options operation) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    const http_client_operation_interruption interruption(state_value, http_client_operation_lane::read, operation);
    state_value.release_consumed_body_prefix();
    state_value.collect_all_ = true;
    if (state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    state_value.notify_producer_space();
    while (!state_value.receive_complete()) {
        if (state_value.read_interrupted()) {
            // The collected prefix stays buffered for the next body operation,
            // which restores ordinary backpressure.
            state_value.collect_all_ = false;
            state_value.throw_if_read_interrupted();
        }
        co_await state_value.data_signal_.wait();
    }
    state_value.rethrow_response_failure();
    if (state_value.pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt,
            state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto remaining = state_value.buffered_.size() - state_value.offset_;
    const auto effective_limit = std::min(max_bytes, state_value.buffered_limit_);
    if (state_value.pending_.size() > effective_limit ||
        remaining > effective_limit - state_value.pending_.size()) {
        throw http_client_error(make_error_code(http_client_errc::response_too_large),
            "HTTP response body exceeds read_all byte limit", std::nullopt,
            state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto total_remaining = remaining + state_value.pending_.size();
    if (total_remaining == 0) {
        state_value.discard_response_body();
        co_return http_client_response_bytes{};
    }
    auto reservation = detail::http_client_result_budget_lease::try_acquire(
        state_value.memory_domain_.result_budget_domain_, total_remaining);
    if (!reservation) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded),
            "HTTP client retained result byte budget is exhausted", std::nullopt,
            state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    http_client_response_bytes result_value(total_remaining, std::move(*reservation));
    if (remaining != 0) {
        result_value.append(std::as_bytes(std::span(state_value.buffered_).subspan(state_value.offset_)));
    }
    if (!state_value.pending_.empty()) {
        result_value.append(std::as_bytes(std::span(state_value.pending_)));
    }
    // The result uses independent thread-safe storage. Only release the
    // worker-owned response buffers after the complete copy succeeds.
    state_value.discard_response_body();
    co_return std::move(result_value);
}

task<void> http_client_response_body::pipe_to(response_stream_writer& output, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(
        state_value.body_operation_scope_, state_value.consume_body<void>(std::move(operation), &output));
}

std::optional<std::string_view> http_client_response::header(std::string_view name) const& {
    const auto& headers_value = require_response_state(state_).headers_;
    const auto match = std::ranges::find_if(headers_value,
        [name](const auto& header_value) { return http_ascii_equals_ignore_case(header_value.name(), name); });
    return match == headers_value.end() ? std::nullopt
                                        : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> http_client_response::trailer(std::string_view name) const& {
    const auto& trailers_value = require_response_state(state_).trailers_;
    const auto match = std::ranges::find_if(trailers_value,
        [name](const auto& header_value) { return http_ascii_equals_ignore_case(header_value.name(), name); });
    return match == trailers_value.end() ? std::nullopt
                                         : std::optional<std::string_view>(match->value());
}

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope) noexcept
    : pool_(&pool),
      registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope,
    operation_options options) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(const http_client_ref& other)
    : pool_(other.pool_),
      pool_owner_(other.pool_owner_),
      options_(other.options_),
      host_stop_token_(other.host_stop_token_),
      registration_(other.registration_, this),
      registry_(other.registry_) {}

void http_client_ref::expire_capability(void* target) noexcept {
    auto& self = *static_cast<http_client_ref*>(target);
    self.pool_ = nullptr;
    self.pool_owner_.reset();
}

http_client_ref http_client_ref::with_options(operation_options options) const {
    registration_.require_active();
    http_client_ref copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

http_client_ref http_client_ref::bind_caller_scope(
    ::ruvia::operation_scope& scope, const stop_token& host_stop) && noexcept {
    http_client_ref bound(*pool_, scope, std::move(options_));
    bound.pool_owner_ = std::move(pool_owner_);
    bound.registry_ = registry_;
    bound.host_stop_token_ = host_stop;
    return bound;
}

task<http_client_response> http_client_ref::dispatch(
    const http_client_request_view& view, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    detail::http_client_request_storage request(view.method_.view(), view.target_.view(), view.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    if (const auto* bytes = view.content_.borrowed_bytes()) {
        request.set_body(bytes->value());
    }
    return ::ruvia::make_scoped_operation(
        registration_.scope(), pool_->execute(std::move(request), options_));
}

task<http_client_exchange> http_client_ref::dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_.value() <= std::chrono::milliseconds::zero() ||
        (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

task<http_client_tunnel_result> http_client_ref::dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (!is_valid_http_origin_form_target(head.target_) || config.max_chunk_bytes_ == 0 ||
        config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid CONNECT-UDP request or tunnel policy");
    }
    for (const auto& field : head.headers_) {
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            throw std::invalid_argument("CONNECT-UDP Capsule-Protocol is driver-owned");
        }
    }
    detail::inline_http_header_views<16> fields(head.headers_.size() + 1U);
    for (const auto& field : head.headers_) {
        fields.push_back(field);
    }
    fields.push_back(http_header_view{"Capsule-Protocol", "?1"});
    // The dispatch owns fields before temporary preparation storage dies.
    const std::string_view authority = request_options.authority_ ? std::string_view(*request_options.authority_) : std::string_view(pool_->config().authority_);
    return dispatch_connect({.authority_ = authority, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields.view()}, config, request_options, informational);
}

task<http_client_tunnel_result> http_client_ref::dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes ||
        (head.protocol_.empty() ? (!is_valid_http_connect_authority(head.authority_) || !head.target_.empty()) : (!is_valid_http_method_token(head.protocol_) || !is_valid_http_origin_form_target(head.target_) || !parse_http_authority(head.authority_)))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    if (head.protocol_.view() == "connect-udp" && validate_http_connect_udp_request_to_send({.version_ = http_protocol_version::http2,
                                                                                                .method_ = "CONNECT",
                                                                                                .scheme_ = scheme() == http_scheme::https ? "https" : "http",
                                                                                                .authority_ = head.authority_,
                                                                                                .path_ = head.target_,
                                                                                                .headers_ = head.headers_})
                                                      .has_value()) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    detail::http_client_request_storage request("CONNECT", head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_tunnel(head.authority_.view(), head.protocol_.view());
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
}

http_client_stats http_client_ref::stats() const {
    registration_.require_active();
    return pool_->stats();
}

std::vector<http_client_endpoint_address> http_client_ref::dns_snapshot() const {
    registration_.require_active();
    return pool_->dns_snapshot();
}
void http_client_ref::update_dns(std::span<const http_client_endpoint_address> addresses) const {
    registration_.require_active();
    pool_->update_dns(addresses);
}
task<void> http_client_ref::refresh_dns() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), refresh_client_dns(pool_owner_, options_, host_stop_token_));
}
task<std::vector<ip_address>> http_client_ref::resolve_dns(
    operation_options options) const {
    registration_.require_active();
    if (pool_->config().dial_host_.empty()) {
        throw std::logic_error(
            "HTTP client alias without a fixed origin or dial host has no default DNS target");
    }
    return resolve_dns(pool_->config().dial_host_, std::move(options));
}

task<std::vector<ip_address>> http_client_ref::resolve_dns(
    std::string_view host, operation_options options) const {
    registration_.require_active();
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, pool_->worker());
    return make_scoped_operation(registration_.scope(),
        resolve_client_handle_dns(pool_owner_, std::string(host),
            options_, std::move(options), host_stop_token_));
}
bool http_client_ref::close_connection(std::uint64_t id) const {
    registration_.require_active();
    return pool_->close_connection(id);
}
std::optional<http_client_push> http_client_ref::next_push() const {
    registration_.require_active();
    return pool_->next_push();
}

std::optional<http_client_advertisement> http_client_ref::next_advertisement() const {
    registration_.require_active();
    return pool_->next_advertisement();
}

std::string_view http_client_ref::host() const& {
    registration_.require_active();
    return pool_->host();
}

std::uint16_t http_client_ref::port() const {
    registration_.require_active();
    return pool_->port();
}

http_scheme http_client_ref::scheme() const {
    registration_.require_active();
    return pool_->scheme();
}

http_origin_view http_client_ref::origin() const& {
    registration_.require_active();
    return pool_->origin();
}

http_client_ref context::client() const {
    return services().client_registries().client(operation_scope_, capabilities_.stop_token());
}

http_client_ref context::client(std::string_view alias) const {
    return services().client_registries().client(alias, operation_scope_, capabilities_.stop_token());
}

task<std::vector<ip_address>> resolve_dns(
    const worker_ref& worker, std::string_view host, operation_options options) {
    if (!worker.valid()) {
        throw std::invalid_argument("DNS resolution requires a valid worker handle");
    }
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, worker);
    return resolve_dns_owned(
        worker, std::string(host), std::move(options));
}

task<std::vector<ip_address>> resolve_dns(
    event_loop loop, std::string_view host, operation_options options) {
    return resolve_dns(loop.worker(), host, std::move(options));
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

// The largest request head the client sends: the header limit plus the
// request line and generated fields.
constexpr std::size_t max_client_request_head_bytes = max_http_header_bytes + 1024;
constexpr std::size_t response_read_chunk_bytes = 16384;

// Prepares one HTTP/1 request head (`prepare(std::span<char>)`) into the
// uninitialized storage of `buffer`, first at its retained capacity. A larger
// head asks for exactly its required size once, up to `max_head_bytes`; the
// caller reports the remaining too-small result.
template <typename prepare_type>
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error>
prepare_http1_request_head(
    http_io_buffer& buffer, std::size_t max_head_bytes, const prepare_type& prepare) {
    auto first = prepare(buffer.prepare((std::max)(buffer.capacity(), http_retained_string_bytes)));
    const auto* too_small = std::get_if<http1_client_request_buffer_too_small>(&first);
    if (too_small == nullptr || too_small->required_head_bytes() > max_head_bytes) {
        return first;
    }
    return prepare(buffer.prepare(too_small->required_head_bytes()));
}

// The returned plan borrows only the request body and the output buffer, not
// the temporary header descriptors or the assembled Cookie field.
[[nodiscard]] auto prepare_client_head(http_client_pool& pool, const http_client_request_storage& request,
    const http_client_upload_state* upload,
    http_io_buffer& write_buffer) {
    client_prepared_request_scratch scratch;
    auto source = pool.policy().prepare_request(request, scratch);
    const auto headers = static_cast<std::span<const http_header_view>>(source.headers_);
    // Host comes from the request route (its authority override, else the alias).
    auto origin = request.route().origin_;
    const bool connect_udp = request.udp_tunnel();
    if (connect_udp) {
        const auto authority = parse_http_authority(borrowed_text(request.tunnel_authority()));
        if (!authority) {
            throw http_client_error(make_error_code(http_client_errc::invalid_request), "invalid CONNECT-UDP authority");
        }
        const http_origin_options options{.host_ = authority->host(), .port_ = authority->port()};
        origin = pool.config().scheme_ == http_scheme::https ? http_origin_view::https(options)
                                                             : http_origin_view::http(options);
    }
    http1_client_request_writer writer;
    const auto prepare = [&](std::span<char> buffer) {
        if (const auto* websocket = request.websocket()) {
            return prepare_http1_websocket_client_request(origin, source.target_.view(), websocket->key_,
                websocket->negotiation_, headers, buffer, request.options_.field_source_);
        }
        if (connect_udp) {
            return writer.prepare_connect_udp(origin, source.target_, headers, buffer, {.field_source_ = request.options_.field_source_});
        }
        if (request.is_tunnel() && !request.is_upgrade()) {
            return writer.prepare_connect(borrowed_text(request.tunnel_authority()), headers, buffer, {.field_source_ = request.options_.field_source_});
        }
        if (upload != nullptr) {
            return writer.prepare_streaming(origin,
                {.method_ = source.method_,
                    .target_ = source.target_,
                    .headers_ = headers,
                    .content_length_ = upload->config_.content_length_},
                buffer, {.expectation_ = upload->config_.expectation_, .field_source_ = request.options_.field_source_});
        }
        return writer.prepare(origin, source, buffer, {.field_source_ = request.options_.field_source_});
    };
    return prepare_http1_request_head(write_buffer, max_client_request_head_bytes, prepare);
}

// One contiguous write() part. A temporary argument outlives the write: it is
// destroyed only at the end of the full-expression that awaits it.
[[nodiscard]] std::array<asio::const_buffer, 1> single_part(std::string_view bytes) noexcept {
    return {asio::buffer(bytes)};
}

[[nodiscard]] std::span<const asio::const_buffer> coalesce_tls_write_parts(
    bool secure, http_io_buffer& write_buffer, std::span<asio::const_buffer> parts) {
    std::size_t non_empty = 0;
    std::size_t total_bytes = 0;
    for (const auto& part : parts) {
        if (part.size() == 0) {
            continue;
        }
        ++non_empty;
        if (part.size() > http_retained_string_bytes - total_bytes) {
            return parts;
        }
        total_bytes += part.size();
    }
    if (!secure || non_empty <= 1) {
        return parts;
    }
    write_buffer.clear();
    const auto first = parts.front();
    const auto prepared = write_buffer.prepare(total_bytes);
    if (static_cast<const char*>(first.data()) == prepared.data()) {
        write_buffer.commit(first.size());
    } else if (first.size() != 0) {
        std::memcpy(prepared.data(), first.data(), first.size());
        write_buffer.commit(first.size());
    }
    for (const auto& part : parts.subspan(1)) {
        if (part.size() != 0) {
            write_buffer.append(std::string_view(static_cast<const char*>(part.data()), part.size()));
        }
    }
    parts[0] = asio::buffer(write_buffer.view());
    return parts.first(1);
}

[[noreturn]] void throw_body_timeout() {
    throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/1 response body decoding timed out");
}

// The parser validates the exact offered protocols and Connection options;
// every accepted Upgrade uses the same transparent byte tunnel as CONNECT.
[[nodiscard]] bool opens_tunnel(
    const http_client_request_storage& request, const http1_parsed_client_response_head& head) noexcept {
    return request.is_tunnel() && (head.plan().connect_tunnel() != nullptr ||
                                      ((request.is_upgrade() || request.udp_tunnel() || request.websocket() != nullptr) &&
                                          head.plan().protocol_upgrade() != nullptr));
}

// Publishes one decoder step's body bytes or trailers to the consumer, or
// throws its failure.
void publish_body_progress(http_client_response_state& state_value,
    const http1_client_response_body_decoder::result_type& decoded, std::size_t max_response_bytes) {
    if (const auto* body = decoded.output()) {
        if (!state_value.append_received_body(body->bytes(), max_response_bytes)) {
            throw http_client_error(
                make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
        }
    } else if (const auto* trailers = decoded.trailers()) {
        const auto fields = trailers->fields();
        state_value.trailers_.reserve(state_value.trailers_.size() + fields.size());
        for (const auto& field : fields) {
            state_value.trailers_.emplace_back(field.name(), field.value());
        }
    } else if (const auto* failure = decoded.protocol_failure()) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error),
            http1_client_response_body_error_message(failure->error()));
    } else if (decoded.decoder_failure() != nullptr) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/1 response body decoder failed");
    }
}

}  // namespace

task<void> http1_client_exchange::write(std::span<const asio::const_buffer> parts, const operation_deadline& timeout) {
    response_->request_written_ = true;
    const auto written =
        co_await connection_.transport_.write(parts, timeout.constrained_by(response_->effective_write_timeout_));
    if (!written.ok()) {
        throw_http_client_transport_error(written, "upstream closed while receiving the HTTP request");
    }
}

void http1_client_exchange::restart_inactivity() noexcept {
    inactivity_timer_.cancel();
    if (!inactivity_bound_) {
        return;
    }
    try {
        (void)pool_.worker().schedule_timer(inactivity_timer_, deadline_after(inactivity_bound_->value()),
            [this, generation = connection_.generation_](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    connection_.cancel(generation, client_abort_reason::timeout);
                }
            });
    } catch (...) {
        // An exchange without its inactivity bound is not continued.
        connection_.cancel(connection_.generation_, client_abort_reason::timeout);
    }
}

void http1_client_exchange::stop_inactivity() noexcept {
    inactivity_bound_.reset();
    inactivity_timer_.cancel();
}

task<client_transport_result> http1_client_exchange::read_append(
    http_io_buffer& target, std::size_t max_bytes, const operation_deadline& timeout, bool read_inactivity_bound) {
    auto read_space = target.prepare_spare();
    if (read_space.empty()) {
        read_space = target.prepare(max_bytes);
    } else {
        read_space = read_space.first(std::min(max_bytes, read_space.size()));
    }
    // Each read restarts the inactivity bound, inside the exchange deadline.
    auto read = co_await connection_.transport_.read_some(
        read_space, read_inactivity_bound ? timeout.constrained_by(response_->effective_read_timeout_) : timeout);
    // close() may clear the target while the read is pending; never publish
    // bytes of a closed connection into it.
    if (!connection_.connected_) {
        if (read.ok() || read.status_ == client_transport_status::eof) {
            read.status_ = client_transport_status::io_failed;
        }
        throw_http_client_transport_error(read, "upstream closed the HTTP connection");
    }
    target.commit(read.bytes_);
    if (read.ok() || read.status_ == client_transport_status::eof) {
        co_return read;
    }
    throw_http_client_transport_error(read, "upstream closed the HTTP connection");
}

task<void> http1_client_exchange::run(
    const http_client_request_storage& request, const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    if (!request.is_upgrade() && !request.tunnel_protocol().empty() && !request.udp_tunnel() && request.websocket() == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "Extended CONNECT requires HTTP/2");
    }
    response_ = &state_value;
    auto& connection = connection_;
    connection.end_idle();
    connection.transport_.reset_abort();
    next_nonzero_id(connection.generation_);
    struct exchange_generation final {
        http_client_connection& connection_;
        ~exchange_generation() {
            next_nonzero_id(connection_.generation_);
            connection_.http1_response_ = nullptr;
        }
    } exchange_generation_value{connection};
    http_client_response_cancellation cancellation(pool_.cancellation_target(), connection.cancellation_id_, state_value);
    // Bound for the exchange so a cancellation or shutdown stops the
    // response's output and wakes its producer waits.
    connection.http1_response_ = &state_value;
    state_value.transport_ = http_client_response_transport::http1;
    state_value.request_method_ = classify_http_method(request.method());
    cancellation.arm(stop_token_value);

    auto* const upload = state_value.upload_ ? &*state_value.upload_ : nullptr;
    // The head is written into the connection's retained, uninitialized buffer.
    auto& write_buffer = connection_.write_buffer_;
    write_buffer.clear();
    auto prepared_result = prepare_client_head(pool_, request, upload, write_buffer);
    auto* prepared = std::get_if<prepared_http1_client_request>(&prepared_result);
    if (!prepared) {
        const auto* failure = std::get_if<http1_client_request_prepare_error>(&prepared_result);
        throw http_client_error(make_error_code(http_client_errc::invalid_request),
            failure != nullptr
                ? http1_client_request_prepare_error_message(*failure)
                : std::string_view("HTTP request head is too large"));
    }
    http1_client_response_parser parser(std::move(*prepared).take_exchange_state());
    const auto* immediate = prepared->content_plan().immediate();
    const bool immediate_content = immediate != nullptr && !immediate->bytes().empty();
    if (!immediate_content) {
        // Streaming/continue-gated content sends its head before the upload.
        co_await write(single_part(prepared->head()), timeout);
        write_buffer.clear_retaining_small();
    }
    if (upload == nullptr && !immediate_content) {
        co_await receive(request, timeout, state_value, parser);
        connection_.retain_idle_buffers();
        co_return;
    }
    task_scope writers(pool_.worker());
    std::exception_ptr writer_failure;
    bool immediate_completed = false;
    // An origin may read the whole request before it answers: until the final
    // head, sending progress also restarts the read inactivity bound.
    inactivity_bound_ = state_value.effective_read_timeout_;
    restart_inactivity();
    std::array<asio::const_buffer, 2> parts;
    if (immediate_content) {
        parts = {asio::buffer(prepared->head()), asio::buffer(immediate->bytes())};
        // Preserve one scatter/gather write (and small TLS coalescing) while
        // receiving 1xx concurrently, without copying the owned request body.
        const auto write_parts = coalesce_tls_write_parts(connection_.transport_.secure(), write_buffer, parts);
        writers.spawn(write_immediate(write_parts, timeout, parser, immediate_completed, writer_failure));
    } else {
        writers.spawn(write_upload(state_value, timeout,
            http1_request_content_writer(*prepared->content_plan().streaming()), parser, writer_failure));
    }
    std::exception_ptr receive_failure;
    try {
        co_await receive(request, timeout, state_value, parser);
    } catch (...) {
        receive_failure = std::current_exception();
    }
    if (receive_failure) {
        if (upload != nullptr) {
            upload->output_.stop();
        }
        connection_.close();
    } else {
        // Response framing can finish before the request content. Publish that
        // boundary independently, but keep the connection leased until the
        // writer finishes the request or is explicitly stopped.
        state_value.complete_ = true;
        state_value.data_signal_.notify();
        if (immediate_content && !immediate_completed) {
            // A complete early response ends the buffered exchange. Never wait
            // for a peer that stopped consuming content, or pool its remainder.
            connection_.close();
        }
    }
    worker_timer_registration upload_completion_timer;
    if (upload != nullptr && !upload->output_.ended_ && !upload->output_.stopped_) {
        if (timeout.expired()) {
            connection_.cancel(connection_.generation_, client_abort_reason::timeout);
        } else if (const auto deadline = timeout.deadline()) {
            (void)pool_.worker().schedule_timer(upload_completion_timer, *deadline,
                [this, generation = connection_.generation_](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        connection_.cancel(generation, client_abort_reason::timeout);
                    }
                });
        }
    }
    co_await writers.join();
    upload_completion_timer.cancel();
    if ((upload != nullptr && !upload->output_.ended_) || (immediate_content && !immediate_completed)) {
        connection_.close();
    }
    connection_.retain_idle_buffers();
    // The receive side decides an ordinary rejected write, including our own
    // early-response close. Preserve its callback/protocol failure as well.
    // Timeout, cancellation, TLS and local write failures still fail both lanes.
    const bool receive_decides_write = immediate_content && writer_failure &&
                                       http_client_failure_code(writer_failure) == make_error_code(http_client_errc::io_error);
    if (writer_failure && !receive_decides_write) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
    connection_.throw_if_aborted();
}

task<void> http1_client_exchange::receive(const http_client_request_storage& request, const operation_deadline& timeout,
    http_client_response_state& state_value, http1_client_response_parser& parser) {
    auto& read_buffer = connection_.read_buffer_;
    // The parsed head is move-constructible only, so it is emplaced once.
    std::optional<http1_parsed_client_response_head> head;
    while (!head) {
        connection_.throw_if_aborted();
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP response head timed out");
        }
        auto parse_result = parser.parse(read_buffer.view());
        if (parse_result.failure()) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error),
                http1_client_response_parse_error_message(*parse_result.failure()));
        }
        if (parse_result.need_more()) {
            if (read_buffer.size() >= max_http_header_bytes) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP response head is too large");
            }
            if (!(co_await read_append(read_buffer, response_read_chunk_bytes, timeout, !inactivity_bound_)).ok()) {
                throw http_client_error(
                    make_error_code(http_client_errc::io_error), "upstream closed before the HTTP response head");
            }
            restart_inactivity();
            continue;
        }
        auto* parsed = parse_result.parsed();
        if (parsed == nullptr) {
            std::terminate();
        }
        if (parsed->plan().informational() == nullptr) {
            head.emplace(std::move(*parsed));
            break;
        }
        auto informational = std::move(*parsed).take_head();
        state_value.retain_informational(
            informational.status(), std::move(informational).take_headers(), parsed->plan().request_content_signal());
        read_buffer.consume(parsed->consumed_bytes());
        // No further transport input or parsing while the consumer is suspended.
        if (state_value.informational_callback_) {
            co_await state_value.dispatch_informational(timeout);
        }
    }
    if (auto* websocket = request.websocket()) {
        const auto negotiated = validate_http1_websocket_client_response(
            websocket->key_, websocket->negotiation_, *head);
        const auto* selected = std::get_if<websocket_client_negotiation_result_view>(&negotiated);
        if (selected == nullptr) {
            throw http_client_error(make_error_code(http_client_errc::handshake_rejected),
                "invalid HTTP/1 WebSocket handshake response", head->head().status());
        }
        websocket->selected_subprotocol_ = selected->selected_subprotocol_;
        websocket->compression_ = selected->compression_;
    }
    const auto& plan = head->plan();
    // The parser's message-length precedence is authoritative, including HEAD,
    // 205 framing, ignored HTTP/1.0 Transfer-Encoding and accepted tunnels.
    state_value.content_length_ = head->head().content_length();
    if (plan.known_length() != nullptr) {
        state_value.framing_ = http_client_response_framing::fixed_length;
    } else if (plan.chunked() != nullptr) {
        state_value.framing_ = http_client_response_framing::chunked;
    } else if (plan.close_delimited() != nullptr) {
        state_value.framing_ = http_client_response_framing::close_delimited;
    } else if (plan.connect_tunnel() != nullptr) {
        state_value.framing_ = http_client_response_framing::tunnel;
    } else if (plan.protocol_upgrade() != nullptr) {
        state_value.framing_ = http_client_response_framing::protocol_upgrade;
    } else {
        state_value.framing_ = http_client_response_framing::none;
    }
    stop_inactivity();
    const bool tunnel_opened = opens_tunnel(request, *head);
    if (!tunnel_opened && (plan.connect_tunnel() != nullptr || plan.protocol_upgrade() != nullptr)) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error),
            "HTTP tunnel and protocol upgrade responses require a dedicated API");
    }
    const http_client_final_head final_head{.status_ = head->head().received_status(),
        .protocol_version_ = head->head().protocol_version(),
        .request_content_signal_ = plan.request_content_signal(),
        .origin_target_ = pool_.policy().addresses_origin(request) ? std::optional(request.target()) : std::nullopt,
        .opens_tunnel_ = tunnel_opened};
    // Parsed fields already use the response resource: transfer, not copy.
    state_value.headers_ = std::move(*head).take_head().take_headers();
    read_buffer.consume(head->consumed_bytes());
    publish_http_client_final_head(state_value, pool_.policy(), final_head);
    if (tunnel_opened) {
        // The HTTP request deadline ends at a WebSocket's accepted Upgrade.
        // Its subsequent operations carry their own deadlines.
        const auto tunnel_timeout = request.websocket() ? operation_deadline() : timeout;
        co_await run_tunnel(state_value, tunnel_timeout, request.websocket() != nullptr);
        co_return;
    }
    http1_client_response_body_decoder body_decoder(plan);
    const bool content_semantics_present = plan.without_content() == nullptr;

    // The body loop stays in this frame: one exchange costs one receive frame.
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    // Empty framing still runs the decoder and boundary checks, but its
    // mandatory nonempty scratch can stay inline. Other responses borrow
    // uninitialized worker storage without inflating this coroutine frame.
    const auto* known_length = plan.known_length();
    const bool empty_body = !content_semantics_present ||
                            (known_length != nullptr && !known_length->requires_body_consumption());
    const bool needs_transfer_scratch =
        !empty_body && !plan.discards_content() &&
        ((plan.chunked() != nullptr && !plan.chunked()->transfer_codings().empty()) ||
            (plan.close_delimited() != nullptr && !plan.close_delimited()->transfer_codings().empty()));
    std::array<char, 1> empty_output{};
    http_io_buffer output_storage;
    const auto transfer_output = needs_transfer_scratch ? output_storage.prepare(http_body_buffer_bytes)
                                                        : std::span<char>{};
    bool eof = false;
    http1_close_policy persistence = http1_close_policy::close_after_response;
    for (;;) {
        // Buffered protocol input may still complete the response without I/O.
        // Drain it within the queue limit before waiting for the consumer, so
        // a complete retained response does not depend on a later body read.
        if (!state_value.collect_all_ && state_value.pending_.size() >= max_response_bytes) {
            co_await wait_for_body_space(state_value, timeout);
        }
        connection_.throw_if_aborted();
        const auto output = needs_transfer_scratch
                                ? transfer_output
                                : (!empty_body && !read_buffer.empty()
                                          ? std::span<char>(read_buffer.data(),
                                                std::min(read_buffer.size(), http_body_buffer_bytes))
                                          : std::span<char>(empty_output));
        const auto capacity = state_value.collect_all_
                                  ? output.size()
                                  : std::min(output.size(), max_response_bytes - state_value.pending_.size());
        const auto scratch = output.first(capacity);
        const auto decoded = eof ? body_decoder.finish_input(read_buffer.view(), scratch)
                                 : body_decoder.decode(read_buffer.view(), scratch);
        const auto consumed = decoded.consumed_bytes();
        publish_body_progress(state_value, decoded, max_response_bytes);
        // Output/trailers have been owned before their source is compacted.
        read_buffer.consume(consumed);
        if (const auto* complete = decoded.complete()) {
            persistence = complete->persistence();
            break;
        }
        if (decoded.need_input() == nullptr || (consumed != 0 && !read_buffer.empty())) {
            continue;
        }
        if (eof) {
            throw std::logic_error("HTTP body decoder requested input after EOF");
        }
        // Only a new network read spends inactivity budget. Outstanding delivery
        // (including the last borrowed chunk) postpones that read, not decoder
        // completion on bytes already received.
        if (!state_value.collect_all_ && (!state_value.pending_.empty() || !state_value.buffered_.empty())) {
            co_await wait_for_body_space(state_value, timeout);
        }
        const auto read = co_await read_append(read_buffer, response_read_chunk_bytes, timeout);
        // A TLS peer that drops TCP without close_notify leaves the message end
        // unauthenticated: an attacker could cut a close-delimited body short
        // (RFC 9112 §9.8). Every other framing still needed bytes here too.
        if (read.error_ == asio::ssl::error::stream_truncated) {
            throw http_client_error(make_error_code(http_client_errc::io_error),
                "upstream closed the TLS connection without close_notify");
        }
        eof = !read.ok();
    }
    // Bytes beyond this response's framing cannot belong to a response the
    // client requested. Preserve the complete framed response, but retire the
    // transport so those bytes are discarded rather than parsed as a future
    // response. A rejected CONNECT likewise leaves subsequent ownership
    // unknown (RFC 9112 §9.6).
    const bool reusable = persistence == http1_close_policy::allow_reuse &&
                          (!request.is_tunnel() || request.is_upgrade()) && read_buffer.empty();
    finish_http_client_response_body(state_value, timeout, content_semantics_present, max_response_bytes);
    if (!reusable) {
        connection_.close();
    }
}

task<void> http1_client_exchange::wait_for_body_space(
    http_client_response_state& state_value, const operation_deadline& timeout) {
    while (!state_value.collect_all_ && (!state_value.pending_.empty() || !state_value.buffered_.empty())) {
        connection_.throw_if_aborted();
        const auto deadline = timeout.deadline();
        if (deadline && *deadline <= std::chrono::steady_clock::now()) {
            throw_body_timeout();
        }
        bool expired = false;
        worker_timer_registration timer;
        if (deadline) {
            (void)pool_.worker().schedule_timer(timer, *deadline,
                [&expired, &state_value](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expired = true;
                        state_value.space_signal_.notify();
                    }
                });
        }
        co_await state_value.space_signal_.wait();
        timer.cancel();
        connection_.throw_if_aborted();
        if (expired || timeout.expired()) {
            throw_body_timeout();
        }
        if (state_value.abandoned_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP response body was abandoned");
        }
    }
}

void http1_client_exchange::expire_tunnel() noexcept {
    connection_.transport_.abort(client_abort_reason::timeout);
    connection_.abort_http1_response();
    connection_.close();
}

task<void> http1_client_exchange::write_tunnel_output(http_client_output_queue& output,
    const operation_deadline& timeout, tls_tunnel_output* tls, std::exception_ptr& failure) {
    const bool secure = connection_.transport_.secure();
    auto& stream = connection_.transport_.stream();
    try {
        std::optional<http_socket_tunnel_transport<asio::ssl::stream<client_tls_socket>>> tls_transport;
        if (tls != nullptr) {
            tls_transport.emplace(stream, tls, nullptr);
        }
        http_socket_tunnel_transport plain_transport(stream.next_layer().socket(), nullptr, nullptr);
        while (!output.ended_ && !output.stopped_) {
            if (!output.chunk_ready_ && !output.end_requested_) {
                co_await output.data_.wait();
                continue;
            }
            const auto write_timeout = timeout.constrained_by(response_->effective_write_timeout_);
            if (write_timeout.expired()) {
                expire_tunnel();
                connection_.throw_if_aborted();
            }
            worker_timer_registration write_timer;
            if (const auto deadline = write_timeout.deadline()) {
                (void)pool_.worker().schedule_timer(write_timer, *deadline,
                    [this](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            expire_tunnel();
                        }
                    });
            }
            const auto ending = output.end_requested_ && !output.chunk_ready_ ? http_stream_end::end
                                                                              : http_stream_end::keep_open;
            const auto bytes_value = output.chunk_ready_ ? std::string_view(output.chunk_) : std::string_view{};
            std::size_t completed_bytes = 0;
            const auto error = tls_transport.has_value()
                                   ? co_await tls_transport->write_bytes(bytes_value, ending, &completed_bytes)
                                   : co_await plain_transport.write_bytes(bytes_value, ending, &completed_bytes);
            pool_.wire_counters().sent_ += completed_bytes;
            write_timer.cancel();
            restart_inactivity();
            connection_.throw_if_aborted();
            if (error) {
                throw http_client_error(secure && is_tls_error(error) ? make_error_code(http_client_errc::tls_failed)
                                                                      : make_error_code(http_client_errc::io_error),
                    error_code_text(error));
            }
            if (ending == http_stream_end::end) {
                output.finish();
            } else {
                output.acknowledge_chunk();
            }
        }
    } catch (...) {
        failure = std::current_exception();
        connection_.close();
        output.stop();
    }
}

task<void> http1_client_exchange::run_tunnel(http_client_response_state& state_value, const operation_deadline& timeout,
    bool websocket) {
    auto& connection = connection_;
    auto& output = state_value.tunnel_->output_;
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    const bool secure = connection.transport_.secure();
    auto& stream = connection.transport_.stream();
    // Everything that can throw after the TLS writer starts runs inside the
    // receive try below, so the started writer is always joined before `tls`
    // is destroyed; the scope and timer slots are allocated first.
    task_scope writers(pool_.worker());
    worker_timer_registration lifetime_timer;
    std::optional<tls_tunnel_output> tls;
    if (secure) {
        tls.emplace(*stream.native_handle(), stream.next_layer().socket(), pool_.worker());
        tls->start();
    }
    std::exception_ptr writer_failure;
    std::exception_ptr receive_failure;
    try {
        if (timeout.expired()) {
            expire_tunnel();
            connection.throw_if_aborted();
        }
        if (const auto deadline = timeout.deadline()) {
            (void)pool_.worker().schedule_timer(lifetime_timer, *deadline,
                [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expire_tunnel();
                    }
                });
        }
        inactivity_bound_ = websocket ? std::nullopt : state_value.tunnel_->config_.idle_timeout_;
        restart_inactivity();
        writers.spawn(write_tunnel_output(output, timeout, tls ? &*tls : nullptr, writer_failure));
        auto& read_buffer = connection.read_buffer_;
        constexpr std::size_t read_chunk_bytes = 4096;
        for (;;) {
            while (state_value.pending_.size() >= max_response_bytes && !state_value.abandoned_ &&
                   connection.connected_ && !writer_failure) {
                co_await state_value.space_signal_.wait();
                connection.throw_if_aborted();
            }
            if (state_value.abandoned_) {
                throw http_client_error(std::make_error_code(std::errc::operation_canceled), "CONNECT tunnel abandoned");
            }
            if (!connection.connected_ || writer_failure) {
                break;
            }
            const auto room = max_response_bytes - state_value.pending_.size();
            if (!read_buffer.empty()) {
                const auto count = std::min(read_buffer.size(), room);
                state_value.pending_.append(read_buffer.data(), count);
                read_buffer.consume(count);
                state_value.data_signal_.notify();
                continue;
            }
            // The consumer may swap pending_ away while a read is in flight, so
            // reads land in the connection buffer and move on the next pass.
            // A WebSocket arms read inactivity only while its application is
            // receiving; the background tunnel still observes the total deadline.
            // read_timeout_ no longer applies; the tunnel's idle timeout counts
            // both directions. A WebSocket arms read inactivity only while its
            // application is receiving.
            const auto read = co_await read_append(read_buffer, std::min(read_chunk_bytes, room), timeout, false);
            restart_inactivity();
            if (!read.ok()) {
                // Same rule as a message body: a TLS peer that drops TCP
                // without close_notify leaves the tunnel end unauthenticated
                // (RFC 8446 section 6.1), so it is a failure, not an orderly end.
                if (read.error_ == asio::ssl::error::stream_truncated) {
                    throw http_client_error(make_error_code(http_client_errc::io_error),
                        "upstream closed the TLS connection without close_notify");
                }
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
                break;
            }
        }
    } catch (...) {
        receive_failure = std::current_exception();
        connection.close();
        output.stop();
    }
    co_await writers.join();
    if (tls) {
        co_await tls->join();
    }
    stop_inactivity();
    lifetime_timer.cancel();
    connection.close();
    connection.retain_idle_buffers();
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
}

task<void> http1_client_exchange::write_immediate(std::span<const asio::const_buffer> parts,
    const operation_deadline& timeout, http1_client_response_parser& parser, bool& completed, std::exception_ptr& failure) {
    try {
        co_await write(parts, timeout);
        restart_inactivity();
        completed = true;
        const auto status = parser.complete_request_content();
        if (status == http1_client_request_content_completion_status::already_complete) {
            std::terminate();
        }
        connection_.write_buffer_.clear_retaining_small();
    } catch (...) {
        failure = std::current_exception();
        // A plain write failure can race a valid final response in the other
        // TCP direction; let its framing decide. Other failures stop the reader.
        if (http_client_failure_code(failure) != make_error_code(http_client_errc::io_error)) {
            connection_.close();
        }
    }
}

task<void> http1_client_exchange::write_upload(http_client_response_state& state_value, const operation_deadline& timeout,
    http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure) {
    auto& upload = *state_value.upload_;
    worker_timer_registration continue_timer;
    if (!upload.content_released_) {
        (void)pool_.worker().schedule_timer(continue_timer, deadline_after(upload.config_.continue_timeout_.value()),
            [&upload, &parser](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.output_.stopped_) {
                    parser.release_request_content();
                    upload.content_released_ = true;
                    upload.output_.notify_data();
                }
            });
    }
    try {
        for (;;) {
            if (upload.output_.stopped_) {
                writer.abort();
                co_return;
            }
            connection_.throw_if_aborted();
            if (timeout.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP upload timed out");
            }
            if (!upload.content_released_ || (!upload.output_.chunk_ready_ && !upload.output_.end_requested_)) {
                co_await upload.output_.data_.wait();
                continue;
            }
            writer.release_content();
            continue_timer.cancel();
            if (upload.output_.chunk_ready_) {
                const auto chunk = writer.plan_chunk(std::span<const char>(upload.output_.chunk_.data(), upload.output_.chunk_.size()));
                if (chunk.index() != 0) {
                    throw http_client_error(make_error_code(http_client_errc::invalid_request), "HTTP upload content length mismatch");
                }
                const auto& planned = std::get<0>(chunk);
                // The write lane has its own deadline, so the concurrent
                // response reader keeps its read deadline.
                std::array<asio::const_buffer, 3> parts{asio::buffer(planned.prefix()),
                    asio::buffer(planned.payload()), asio::buffer(planned.suffix())};
                const auto write_parts = coalesce_tls_write_parts(
                    connection_.transport_.secure(), connection_.write_buffer_, parts);
                co_await write(write_parts, timeout);
                restart_inactivity();
                if (write_parts.size() == 1) {
                    connection_.write_buffer_.clear_retaining_small();
                }
                writer.commit_chunk();
                upload.output_.acknowledge_chunk();
                continue;
            }
            auto section_budget = http_trailer_section_budget();
            (void)section_budget.add(http1_last_chunk_prefix.size());
            for (const auto& field : upload.trailers_) {
                (void)section_budget.add_field(field.name(), field.value());
            }
            const inline_http_header_views trailers(upload.trailers_);
            // Keep the last-chunk line and small trailer sections in this
            // operation; larger trailer sections allocate only their exact wire size.
            constexpr std::size_t inline_ending_bytes = 256;
            std::array<char, inline_ending_bytes> inline_ending;
            http_io_buffer scratch;
            const auto required_bytes = section_budget.bytes();
            const auto finish_buffer = required_bytes <= inline_ending.size()
                                           ? std::span<char>(inline_ending.data(), required_bytes)
                                           : scratch.prepare(required_bytes);
            const auto ending = writer.plan_finish(finish_buffer, trailers.view());
            if (ending.index() != 0) {
                throw http_client_error(
                    make_error_code(http_client_errc::invalid_request), "HTTP upload trailer or final length rejected");
            }
            struct completion_guard final {
                http_client_upload_state& upload_;
                explicit completion_guard(http_client_upload_state& value)
                    : upload_(value) {
                    upload_.output_.completion_pending_ = true;
                }
                ~completion_guard() {
                    upload_.output_.completion_pending_ = false;
                    upload_.output_.space_.notify();
                }
            } completion(upload);
            if (const auto bytes = std::get<0>(ending); !bytes.empty()) {
                co_await write(single_part(bytes), timeout);
            }
            writer.commit_finish();
            const auto completed = parser.complete_request_content();
            if (completed == http1_client_request_content_completion_status::exchange_terminal && !state_value.head_ready_) {
                // The receiver already failed this exchange after the parser
                // left await_response; it owns and reports that failure.
                co_return;
            }
            upload.output_.finish();
            co_return;
        }
    } catch (...) {
        const auto upload_failure = std::current_exception();
        // TCP directions are independent: a peer can reject more request
        // bytes while a complete final response is already on its way. Keep
        // reading after a plain transport write failure; framing decides
        // whether that response is valid. Timeout, cancellation, local
        // request errors and explicit abandonment still terminate both lanes.
        const bool preserve_response = !state_value.abandoned_ &&
                                       connection_.transport_.abort_reason() == client_abort_reason::none &&
                                       http_client_failure_code(upload_failure) == make_error_code(http_client_errc::io_error);
        if (preserve_response) {
            upload.failure_ = upload_failure;
            if (!state_value.complete_ && !upload.output_.stopped_) {
                failure = upload_failure;
            }
        } else if (!state_value.complete_ && !upload.output_.stopped_) {
            failure = upload_failure;
            // The caller's pending write() wakes on stop() below and reports
            // the published exchange failure instead of a bare cancellation.
            if (state_value.failure_ == nullptr) {
                state_value.failure_ = failure;
            }
        }
        upload.output_.stop();
        if (!preserve_response) {
            connection_.close();
        }
    }
}

}  // namespace ruvia::detail
