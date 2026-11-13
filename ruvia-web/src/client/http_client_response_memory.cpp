#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"
#include "client/http_client_pool.h"
#include "server/connection_lifecycle.h"

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

}  // namespace ruvia::detail

namespace ruvia::detail {

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

}  // namespace ruvia::detail

namespace ruvia::detail {

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

}  // namespace ruvia::detail

namespace ruvia::detail {
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

}  // namespace ruvia::detail

namespace ruvia::detail {

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
