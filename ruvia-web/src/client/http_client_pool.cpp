#include "client/http_client_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_tunnel.h"

#include "client/client_tls.h"
#include "client/http1_client_exchange.h"
#include "client/http_client_connection.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "client/http_client_transport_error.h"
#include "server/peer_address.h"
#include "util/operation_lane_lease.h"

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
[[nodiscard]] std::size_t free_connection_position(std::pmr::vector<owner_type>& owners) {
    const auto free = std::ranges::find(owners, nullptr);
    if (free != owners.end()) {
        return static_cast<std::size_t>(free - owners.begin());
    }
    owners.emplace_back(nullptr, typename owner_type::deleter_type{process_resource()});
    return owners.size() - 1;
}

#ifdef RUVIA_ENABLE_HTTP3
// How long an alternative service that failed to connect stays unused.
constexpr std::chrono::minutes alternative_service_retry_delay{5};

// RFC 7838 field members: commas and semicolons inside quoted strings do not
// delimit members. Unsupported ALPN identifiers are skipped, never downgraded.
std::string_view take_component(std::string_view& text, char delimiter) noexcept {
    const auto end = http_find_unquoted_delimiter(text, 0, delimiter);
    const auto result = http_trim_ows(text.substr(0, end));
    text.remove_prefix((std::min)(text.size(), end + 1));
    return result;
}
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

http_client_pool::http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config, const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      config_(std::move(config)),
      origin_(config_.fixed_origin_ ? std::optional(config_.origin()) : std::nullopt),
      policy_(config_),
      advertisements_(worker_, config_.advertisements_),
      pushes_(process_resource()),
      result_budget_domain_(services.result_budget_),
      worker_limits_(services.limits_),
      tls_context_(asio::ssl::context::tls_client),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)),
      background_tasks_(worker_, {.resource_ = process_resource()}),
      connections_(process_resource()),
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

std::pmr::vector<http_client_endpoint_address> http_client_pool::dns_snapshot() const {
    std::pmr::vector<http_client_endpoint_address> result(process_resource());
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
    std::pmr::vector<asio::ip::tcp::endpoint> endpoints(process_resource());
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
    std::pmr::vector<std::pair<std::pmr::string, std::uint16_t>> targets(process_resource());
    const auto add = [&targets](std::string_view host, std::uint16_t port) {
        if (!client_host_needs_lookup(host)) {
            return;
        }
        for (const auto& target : targets) {
            if (target.second == port && http_ascii_equals_ignore_case(target.first, host)) {
                return;
            }
        }
        targets.emplace_back(std::pmr::string(host, process_resource()), port);
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
        decltype(connections_) retired_connections(process_resource());
        retired_connections.swap(connections_);
#ifdef RUVIA_ENABLE_HTTP3
        decltype(http3_connections_) retired_http3(process_resource());
        retired_http3.swap(http3_connections_);
        decltype(alternative_services_) retired_alternatives(process_resource());
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
    state_value.tunnel_.emplace(state_value.memory_domain().worker(), state_value.resource_, config);
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
    state_value.upload_.emplace(state_value.memory_domain().worker(), state_value.resource_, upload);
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
            connections_[index] = make_pmr_object<http_client_connection>(process_resource(), *this, index,
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
    if (config_.scheme_ != http_scheme::https || !route.versions_.http3_ ||
        (!route.versions_.http1_ && !route.versions_.http2_)) {
        return;
    }
    const auto names_route = [&route](const alternative_service& service) noexcept {
        return service.route_port_ == route.dial_port_ && http_ascii_equals_ignore_case(service.route_host_, route.dial_host_) &&
               http_ascii_equals_ignore_case(service.server_name_, route.server_name_);
    };
    // The route's failed alternatives stay unavailable across a re-advertisement.
    std::pmr::vector<alternative_service> previous(process_resource());
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
            return;
        }
        while (!value.empty()) {
            auto member = take_component(value, ',');
            auto authority = take_component(member, ';');
            const auto equals = authority.find('=');
            if (equals == std::string_view::npos || http_trim_ows(authority.substr(0, equals)) != "h3") {
                continue;
            }
            authority = http_trim_ows(authority.substr(equals + 1));
            if (authority.size() < 4 || authority.front() != '"' || authority.back() != '"') {
                continue;
            }
            authority.remove_prefix(1);
            authority.remove_suffix(1);
            if (authority.find_first_of("\\\" \t\r\n/@?#") != std::string_view::npos) {
                continue;
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
                    continue;
                }
                port = static_cast<std::uint16_t>(parsed_port);
                host = route.dial_host_;
            } else {
                const auto parsed_authority = parse_http_authority(authority);
                if (!parsed_authority || parsed_authority->port_kind() != http_authority_port_kind::value ||
                    *parsed_authority->port() == 0) {
                    continue;
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
                continue;
            }
            std::uint64_t age = 86400;
            bool valid = true;
            bool age_seen = false;
            while (!member.empty()) {
                const auto parameter = take_component(member, ';');
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
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto maximum = std::chrono::duration_cast<std::chrono::seconds>((std::chrono::steady_clock::time_point::max)() - now).count();
            const auto seconds = (std::min)(age, static_cast<std::uint64_t>(maximum));
            if (alternative_services_.size() == config_.http3_.max_alt_svc_entries_) {
                // One bound across routes: the least recently used entry goes.
                alternative_services_.erase(std::ranges::min_element(alternative_services_, {}, &alternative_service::used_));
            }
            alternative_service entry{.route_host_ = std::pmr::string(route.dial_host_, process_resource()),
                .route_port_ = route.dial_port_,
                .server_name_ = std::pmr::string(route.server_name_, process_resource()),
                .host_ = std::pmr::string(host, process_resource()),
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
        }
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
    std::pmr::string host(process_resource());
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
                    http3_tls_ = make_pmr_object<http3_client_tls>(process_resource(), *this);
                }
                const auto index = free_connection_position(http3_connections_);
                http3_connections_[index] = make_pmr_object<http3_client_connection>(process_resource(), *this,
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
            if (service.port_ == port && service.host_ == host && service.route_port_ == route.dial_port_ &&
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
            http_tunnel_input result{std::pmr::string(std::string_view(state.buffered_).substr(state.offset_), state.resource_), false};
            state.offset_ = state.buffered_.size();
            co_return result;
        }
        if (!state.pending_.empty()) {
            http_tunnel_input result{std::pmr::string(state.resource_), false};
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

task<void> http_client_pool::migrate_http3(std::pmr::string local_address, std::uint16_t port,
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
    std::pmr::vector<std::pair<std::size_t, std::uint64_t>> active(process_resource());
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
