#include "ruvia/web/http_client_handle.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/context.h"
#include "ruvia/web/streaming.h"

#include "client/http_client_config_validation.h"
#include "client/http_client_pool.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "context/context_services.h"
#include "http3/http3_client_connection.h"

namespace ruvia {
namespace {

constexpr std::size_t response_body_read_chunk_bytes = std::size_t{16} * 1024;

void check_response_operation_affinity(void* target) noexcept {
    const auto& state_value = *static_cast<const detail::http_client_response_state*>(target);
    if (auto* domain = state_value.memory_domain(); domain != nullptr && !domain->worker().is_current()) {
        std::terminate();
    }
}

}  // namespace

http_client_response::http_client_response(detail::http_client_pool& pool)
    : state_(pool.response_memory_->create_state(pool)),
      body_(state_) {}

http_client_response::http_client_response(detail::http_client_response_state* state_value, bool retain) noexcept
    : state_(state_value),
      body_(state_value),
      consumer_(false) {
    if (retain && state_ != nullptr) {
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
    if (consumer_ && state_value->http3_connection_ != nullptr) {
        auto* connection = state_value->http3_connection_;
        const auto request_id = state_value->http3_request_id_;
        if (!state_value->complete_ && !state_value->abandoned_) {
            connection->abandon_response(request_id);
        }
        connection->consumer_released(request_id);
    } else if (consumer_ && !state_value->complete_ && !state_value->abandoned_ && state_value->pool_ != nullptr) {
        state_value->pool_->abandon_response(*state_value);
    }
    state_value->release_reference();
}

std::span<const http_client_informational_response> http_client_response::informational_responses() const& noexcept {
    return state_->informational_;
}

void detail::http_client_response_state::retain_informational(http_status_code status_code, std::span<const http_header_view> fields_value) {
    // Retained progress metadata has a separate fixed aggregate bound. It must
    // never grow with the duration of an upstream response stream.
    if (informational_.size() >= 8) {
        throw http_client_error(http_client_error::code_type::protocol_error, "too many informational response heads");
    }
    http_client_informational_response head(status_code, resource_);
    head.headers_.reserve(fields_value.size());
    std::size_t bytes_value = informational_field_bytes_;
    for (const auto& field : fields_value) {
        if (bytes_value > max_http_header_bytes - 32 || field.name().size() > max_http_header_bytes - bytes_value - 32 || field.value().size() > max_http_header_bytes - bytes_value - 32 - field.name().size()) {
            throw http_client_error(http_client_error::code_type::protocol_error, "informational response metadata exceeds byte limit");
        }
        bytes_value += 32 + field.name().size() + field.value().size();
        head.headers_.push_back(http_header::copy_of(field.name(), field.value(), resource_));
    }
    informational_.push_back(std::move(head));
    informational_field_bytes_ = bytes_value;
}

http_status_code http_client_response::status() const noexcept {
    return state_->status_;
}
http_protocol_version http_client_response::protocol_version() const noexcept {
    return state_->protocol_version_;
}
void http_client_response::reprioritize(http_priority priority) & {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    check_response_operation_affinity(state_);
    if (priority.urgency_ > 7) {
        throw std::invalid_argument("HTTP priority urgency must be between 0 and 7");
    }
    if (state_->complete_ || state_->abandoned_ || state_->pool_ == nullptr) {
        throw http_client_error(http_client_error::code_type::closing, "response transport is retired");
    }
    state_->pool_->reprioritize(*state_, priority);
}
std::span<const http_header> http_client_response::headers() const& noexcept {
    return state_->headers_;
}
std::span<const http_header> http_client_response::trailers() const& noexcept {
    return state_->trailers_;
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
        if (auto* domain = memory_domain()) {
            domain->destroy_state(this);
        } else {
            detail::destroy_pmr_object(this, resource_);
        }
    }
}

void detail::http_client_response_state::notify_producer_space() noexcept {
    // A changed read policy (for example collect_all_) can unblock the QUIC
    // driver even when no body storage has been released yet.
    http3_body_budget_.notify_producer();
    space_signal_.notify();
}

bool detail::http_client_response_state::bind_http3_body_budget(
    detail::http3_client_body_budget& budget) noexcept {
    return http3_body_budget_.attach(budget, producer_body_bytes());
}

void detail::http_client_response_state::release_http3_body_budget() noexcept {
    if (http3_body_budget_.retained_bytes() != 0) {
        std::terminate();
    }
    http3_body_budget_.reset();
}

bool detail::http_client_response_state::has_http3_body_budget() const noexcept {
    return http3_body_budget_.attached();
}

std::size_t detail::http_client_response_state::producer_body_bytes() const noexcept {
    if (pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_.size()) {
        std::terminate();
    }
    return buffered_.size() + pending_.size();
}

std::size_t detail::http_client_response_state::producer_body_budget_available() const noexcept {
    return http3_body_budget_.available();
}

bool detail::http_client_response_state::retain_producer_body_bytes(std::size_t bytes_value) noexcept {
    return !http3_body_budget_.attached() || http3_body_budget_.try_retain(bytes_value);
}

void detail::http_client_response_state::release_producer_body_bytes(std::size_t bytes_value) noexcept {
    if (http3_body_budget_.attached()) {
        http3_body_budget_.release(bytes_value);
    }
}

void detail::http_client_response_state::reconcile_producer_body_bytes() noexcept {
    if (!http3_body_budget_.attached()) {
        return;
    }
    if (!http3_body_budget_.try_replace(producer_body_bytes())) {
        std::terminate();
    }
}

bool detail::http_client_response_state::replace_producer_body_bytes(std::size_t bytes_value) noexcept {
    return !http3_body_budget_.attached() || http3_body_budget_.try_replace(bytes_value);
}

void detail::http_client_response_state::discard_pending_body() noexcept {
    {
        std::pmr::string empty(resource_);
        pending_.swap(empty);
    }
    reconcile_producer_body_bytes();
    notify_producer_space();
}

void detail::http_client_response_state::discard_response_body() noexcept {
    {
        std::pmr::string empty_buffered(resource_);
        std::pmr::string empty_pending(resource_);
        buffered_.swap(empty_buffered);
        pending_.swap(empty_pending);
    }
    offset_ = 0;
    reconcile_producer_body_bytes();
    notify_producer_space();
}

void detail::http_client_response_state::release_consumed_body_prefix() {
    if (offset_ == 0) {
        return;
    }
    if (offset_ > buffered_.size()) {
        std::terminate();
    }
    if (offset_ == buffered_.size()) {
        {
            std::pmr::string empty(resource_);
            buffered_.swap(empty);
        }
    } else {
        buffered_.erase(0, offset_);
    }
    offset_ = 0;
    reconcile_producer_body_bytes();
    notify_producer_space();
}

void detail::http_client_response_state::promote_pending_data() {
    auto& state_value = *this;
    if (state_value.offset_ != state_value.buffered_.size() || state_value.pending_.empty()) {
        return;
    }
    if (state_value.http2_data_credit_ && state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    std::pmr::string empty(state_value.resource_);
    state_value.buffered_.swap(empty);
    state_value.offset_ = 0;
    state_value.buffered_.swap(state_value.pending_);
    state_value.notify_producer_space();
}

bool http_client_response_body::complete() const noexcept {
    return state_ == nullptr || (state_->receive_complete() && state_->offset_ == state_->buffered_.size() &&
                                    state_->pending_.empty());
}

scoped_operation<std::optional<std::span<const std::byte>>> http_client_response_body::read() & {
    if (state_->body_operation_scope_.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_,
        state_->consume_body<std::span<const std::byte>>(), check_response_operation_affinity, state_);
}

scoped_operation<std::optional<std::string_view>> http_client_response_body::text() & {
    if (state_->body_operation_scope_.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_,
        state_->consume_body<std::string_view>(), check_response_operation_affinity, state_);
}

template <typename view>
task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::http_client_response_state::consume_body(response_stream_writer* output) {
    auto& state_value = *this;
    for (;;) {
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state_value.release_consumed_body_prefix();
        state_value.incremental_read_ = true;
        while ((state_value.body_decode_required_ && !state_value.receive_complete()) ||
               (state_value.buffered_.empty() && state_value.pending_.empty() && !state_value.receive_complete())) {
            co_await state_value.data_signal_.wait();
        }
        promote_pending_data();
        if (state_value.offset_ == state_value.buffered_.size()) {
            if (state_value.failure_) {
                std::rethrow_exception(state_value.failure_);
            }
            if (state_value.error_code_) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw http_client_error(static_cast<http_client_error::code_type>(*state_value.error_code_), message);
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
            co_await output->write(std::as_bytes(std::span(chunk.data(), chunk.size())));
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

template task<std::optional<std::span<const std::byte>>> detail::http_client_response_state::consume_body<std::span<const std::byte>>(response_stream_writer*);
template task<std::optional<std::string_view>> detail::http_client_response_state::consume_body<std::string_view>(response_stream_writer*);
template task<void> detail::http_client_response_state::consume_body<void>(response_stream_writer*);

scoped_operation<http_client_response_bytes> http_client_response_body::read_all(std::size_t max_bytes) & {
    if (state_->body_operation_scope_.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_,
        state_->read_all(max_bytes), check_response_operation_affinity, state_);
}

task<http_client_response_bytes> detail::http_client_response_state::read_all(std::size_t max_bytes) {
    auto& state_value = *this;
    state_value.release_consumed_body_prefix();
    state_value.collect_all_ = true;
    if (state_value.http2_data_credit_ && state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    state_value.notify_producer_space();
    while (!state_value.receive_complete()) {
        co_await state_value.data_signal_.wait();
    }
    if (state_value.failure_) {
        std::rethrow_exception(state_value.failure_);
    }
    if (state_value.error_code_) {
        throw http_client_error(
            static_cast<http_client_error::code_type>(*state_value.error_code_), "HTTP response body read failed");
    }
    const auto remaining = state_value.buffered_.size() - state_value.offset_;
    const auto effective_limit = std::min(max_bytes, state_value.buffered_limit_);
    if (state_value.pending_.size() > effective_limit ||
        remaining > effective_limit - state_value.pending_.size()) {
        throw http_client_error(http_client_error::code_type::response_too_large,
            "HTTP response body exceeds read_all byte limit");
    }
    const auto total_remaining = remaining + state_value.pending_.size();
    if (state_value.result_budget_domain_ == nullptr) {
        throw std::logic_error("HTTP client response has no result byte budget");
    }
    auto reservation = detail::http_client_result_budget_lease::try_acquire(
        *state_value.result_budget_domain_, total_remaining);
    if (!reservation) {
        throw http_client_error(http_client_error::code_type::result_budget_exceeded,
            "HTTP client retained result byte budget is exhausted");
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

scoped_operation<void> http_client_response_body::pipe_to(response_stream_writer& output) & {
    if (state_->body_operation_scope_.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_,
        state_->consume_body<void>(&output), check_response_operation_affinity, state_);
}

std::optional<std::string_view> http_client_response::header(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->headers_,
        [name](const auto& header_value) { return http_ascii_equals_ignore_case(header_value.name(), name); });
    return match == state_->headers_.end() ? std::nullopt
                                           : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> http_client_response::trailer(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->trailers_,
        [name](const auto& header_value) { return http_ascii_equals_ignore_case(header_value.name(), name); });
    return match == state_->trailers_.end() ? std::nullopt
                                            : std::optional<std::string_view>(match->value());
}

http_client_handle::http_client_handle(detail::http_client_pool& pool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& scope) noexcept
    : pool_(&pool),
      resource_(resource),
      registration_(scope, this, &http_client_handle::expire_capability) {}

http_client_handle::http_client_handle(detail::http_client_pool& pool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& scope,
    operation_options options) noexcept
    : pool_(&pool),
      resource_(resource),
      options_(std::move(options)),
      registration_(scope, this, &http_client_handle::expire_capability) {}

http_client_handle::http_client_handle(const http_client_handle& other)
    : pool_(other.pool_),
      resource_(other.resource_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void http_client_handle::expire_capability(void* target) noexcept {
    static_cast<http_client_handle*>(target)->pool_ = nullptr;
}

http_client_handle http_client_handle::with_options(operation_options options) const {
    detail::validate_operation_options(options);
    registration_.require_active();
    http_client_handle copy(*this);
    copy.options_ = detail::merge_operation_options(options_, std::move(options));
    return copy;
}

scoped_operation<http_client_response> http_client_handle::send(
    const http_client_request_view& view) const {
    registration_.require_active();
    detail::http_client_request_storage request(
        view.method_.view(), view.target_.view(), detail::pmr_resource_or_default(resource_));
    for (const auto& header : view.headers_) {
        request.append_header(header.name(), header.value());
    }
    if (const auto* bytes = view.content_.borrowed_bytes()) {
        request.set_body(bytes->value());
    }
    detail::validate_operation_options(options_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), pool_->execute(std::move(request), options_));
}

scoped_operation<http_client_exchange> http_client_handle::open_request(const http_client_request_view& head, http_client_upload_config upload) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_ <= std::chrono::milliseconds::zero() ||
        upload.continue_timeout_ > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()) ||
        (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), detail::pmr_resource_or_default(resource_));
    for (const auto& field : head.headers_) {
        request.append_header(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

scoped_operation<http_client_tunnel_result> http_client_handle::open_udp_tunnel(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config) const {
    registration_.require_active();
    auto* resource = detail::pmr_resource_or_default(resource_);
    std::pmr::vector<http_header_view> fields(resource);
    fields.reserve(head.headers_.size() + 1);
    for (const auto& field : head.headers_) {
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            throw std::invalid_argument("CONNECT-UDP Capsule-Protocol is driver-owned");
        }
        fields.push_back(field);
    }
    fields.emplace_back("Capsule-Protocol", "?1");
    auto authority = detail::client_uri_host(host(), resource);
    if (port() != (scheme() == http_scheme::https ? 443 : 80)) {
        std::array<char, 5> digits;
        const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), port()).ptr;
        authority.push_back(':');
        authority.append(digits.data(), end);
    }
    // open_tunnel owns all fields before this temporary preparation storage dies.
    return open_tunnel({.authority_ = authority, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields}, config);
}

scoped_operation<http_client_tunnel_result> http_client_handle::open_tunnel(const http_client_tunnel_request_view& head, http_client_tunnel_config config) const {
    registration_.require_active();
    if ((config.datagrams_ && head.protocol_.empty()) || config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes ||
        (head.protocol_.empty() ? (!is_valid_http_connect_authority(head.authority_) || !head.target_.empty()) : (!is_valid_http_method_token(head.protocol_) || !is_valid_http_origin_form_target(head.target_) || !parse_http_authority_host(borrowed_text(head.authority_)) || head.authority_.empty()))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    detail::http_client_request_storage request("CONNECT", head.target_, detail::pmr_resource_or_default(resource_));
    if (head.protocol_ == "connect-udp" && (validate_http_connect_udp_request({.version_ = http_protocol_version::http2,
                                                                                  .scheme_ = scheme() == http_scheme::https ? "https" : "http",
                                                                                  .authority_ = head.authority_,
                                                                                  .path_ = head.target_,
                                                                                  .headers_ = head.headers_})
                                                   .index() != 0)) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    request.set_tunnel(head.authority_, head.protocol_);
    for (const auto& field : head.headers_) {
        request.append_header(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
}

quic_path_migration http_client_handle::start_quic_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) const {
    registration_.require_active();
    return pool_->start_quic_path_migration(local_endpoint);
}

std::optional<quic_path_migration> http_client_handle::path_migration(
    std::uint64_t id) const {
    registration_.require_active();
    return pool_->path_migration(id);
}

quic_operation_status http_client_handle::cancel_quic_path_migration(std::uint64_t id) const {
    registration_.require_active();
    return pool_->cancel_quic_path_migration(id);
}

http_client_stats http_client_handle::stats() const {
    registration_.require_active();
    return pool_->stats();
}
std::optional<http_client_push> http_client_handle::next_push() const {
    registration_.require_active();
    return pool_->next_push();
}

std::optional<http_client_advertisement> http_client_handle::next_advertisement() const {
    registration_.require_active();
    return pool_->next_advertisement();
}

std::string_view http_client_handle::host() const& {
    registration_.require_active();
    return pool_->host();
}

std::uint16_t http_client_handle::port() const {
    registration_.require_active();
    return pool_->port();
}

http_scheme http_client_handle::scheme() const {
    registration_.require_active();
    return pool_->scheme();
}

http_client_handle context::get_http_client() const {
    return services().client_registries().get_http_client(operation_scope_, capabilities_.stop_token());
}

http_client_handle context::get_http_client(std::string_view alias) const {
    return services().client_registries().get_http_client(alias, operation_scope_, capabilities_.stop_token());
}

}  // namespace ruvia
