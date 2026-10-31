#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"

#include "client/http_client_pool.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "context/context_services.h"
#include "util/operation_lane_lease.h"

namespace ruvia {
namespace {

constexpr std::size_t response_body_read_chunk_bytes = std::size_t{16} * 1024;

void check_response_operation_affinity(void* target) noexcept {
    const auto& state_value = *static_cast<const detail::http_client_response_state*>(target);
    if (!state_value.memory_domain().worker().is_current()) {
        std::terminate();
    }
}

}  // namespace

http_client_response::http_client_response(detail::http_client_pool& pool)
    : state_(pool.response_memory().create_state(pool)),
      body_(state_) {}

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

std::span<const http_client_informational_response> http_client_response::informational_responses() const& noexcept {
    return state_->informational_;
}

http_status_code http_client_response::status() const noexcept {
    return state_->status_.status();
}
http_received_status http_client_response::received_status() const noexcept {
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
    if (state_->complete_) {
        // The peer already finished the stream; there is nothing left to schedule.
        return;
    }
    if (state_->abandoned_ || state_->pool_ == nullptr) {
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
        memory_domain_.destroy_state(this);
    }
}

void detail::http_client_response_state::notify_producer_space() noexcept {
    space_signal_.notify();
}

void detail::http_client_response_state::rethrow_response_failure() const {
    if (failure_ && !upload_only_failure_) {
        std::rethrow_exception(failure_);
    }
}

task<void> detail::http_client_response_state::await_head(std::string_view head_message) {
    while (!head_ready_ && !failure_ && !pool_closed_) {
        co_await head_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(http_client_error::code_type::closing, head_message);
    }
    if ((tunnel_ && tunnel_->accepted_) || !body_decode_required_) {
        co_return;
    }
    if (http2_data_credit_ && pool_ != nullptr) {
        pool_->release_response_data(*this);
    }
    notify_producer_space();
    while (!complete_) {
        co_await data_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP client encoded response failed");
    }
}

void detail::http_client_response_state::discard_response_body() noexcept {
    {
        std::pmr::string empty_buffered(resource_);
        std::pmr::string empty_pending(resource_);
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
    {
        std::pmr::string empty(resource_);
        buffered_.swap(empty);
    }
    offset_ = 0;
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

task<std::optional<std::span<const std::byte>>> http_client_response_body::read() & {
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_, state_->consume_body<std::span<const std::byte>>());
}

task<std::optional<std::string_view>> http_client_response_body::text() & {
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_, state_->consume_body<std::string_view>());
}

template <typename view>
task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::http_client_response_state::consume_body(response_stream_writer* output) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    for (;;) {
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state_value.release_consumed_body_prefix();
        while ((state_value.body_decode_required_ && !state_value.receive_complete()) ||
               (state_value.buffered_.empty() && state_value.pending_.empty() && !state_value.receive_complete())) {
            co_await state_value.data_signal_.wait();
        }
        promote_pending_data();
        if (state_value.offset_ == state_value.buffered_.size()) {
            state_value.rethrow_response_failure();
            if (state_value.pool_closed_) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw http_client_error(http_client_error::code_type::closing, message);
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

task<http_client_response_bytes> http_client_response_body::read_all(std::size_t max_bytes) & {
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_, state_->read_all(max_bytes));
}

task<http_client_response_bytes> detail::http_client_response_state::read_all(std::size_t max_bytes) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    state_value.release_consumed_body_prefix();
    state_value.collect_all_ = true;
    if (state_value.http2_data_credit_ && state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    state_value.notify_producer_space();
    while (!state_value.receive_complete()) {
        co_await state_value.data_signal_.wait();
    }
    state_value.rethrow_response_failure();
    if (state_value.pool_closed_) {
        throw http_client_error(http_client_error::code_type::closing, "HTTP response body read failed");
    }
    const auto remaining = state_value.buffered_.size() - state_value.offset_;
    const auto effective_limit = std::min(max_bytes, state_value.buffered_limit_);
    if (state_value.pending_.size() > effective_limit ||
        remaining > effective_limit - state_value.pending_.size()) {
        throw http_client_error(http_client_error::code_type::response_too_large,
            "HTTP response body exceeds read_all byte limit");
    }
    const auto total_remaining = remaining + state_value.pending_.size();
    if (total_remaining == 0) {
        state_value.discard_response_body();
        co_return http_client_response_bytes{};
    }
    auto reservation = detail::http_client_result_budget_lease::try_acquire(
        state_value.memory_domain_.result_budget_domain_, total_remaining);
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

task<void> http_client_response_body::pipe_to(response_stream_writer& output) & {
    check_response_operation_affinity(state_);
    return ::ruvia::make_scoped_operation(state_->body_operation_scope_, state_->consume_body<void>(&output));
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
    validate_operation_options(options);
    registration_.require_active();
    http_client_handle copy(*this);
    copy.options_ = merge_operation_options(options_, std::move(options));
    return copy;
}

task<http_client_response> http_client_handle::send(
    const http_client_request_view& view) const {
    validate_operation_options(options_);
    detail::http_client_request_storage request(view.method_.view(), view.target_.view(), view.headers_, resource_);
    if (const auto* bytes = view.content_.borrowed_bytes()) {
        request.set_body(bytes->value());
    }
    return ::ruvia::make_scoped_operation(
        registration_.scope(), pool_->execute(std::move(request), options_));
}

task<http_client_exchange> http_client_handle::open_request(const http_client_request_view& head, http_client_upload_config upload) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_ <= std::chrono::milliseconds::zero() ||
        upload.continue_timeout_ > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()) ||
        (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), head.headers_, resource_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

task<http_client_tunnel_result> http_client_handle::open_udp_tunnel(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config) const {
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
    static constexpr std::size_t inline_field_capacity = 16;
    const std::size_t total_fields = head.headers_.size() + 1U;
    std::array<http_header_view, inline_field_capacity> inline_fields{};
    std::pmr::vector<http_header_view> heap_fields(resource_);
    std::span<const http_header_view> fields{};
    if (total_fields <= inline_fields.size()) {
        std::size_t index = 0;
        for (const auto& field : head.headers_) {
            inline_fields[index++] = field;
        }
        inline_fields[index++] = http_header_view{"Capsule-Protocol", "?1"};
        fields = std::span<const http_header_view>(inline_fields.data(), total_fields);
    } else {
        heap_fields.reserve(total_fields);
        for (const auto& field : head.headers_) {
            heap_fields.push_back(field);
        }
        heap_fields.emplace_back("Capsule-Protocol", "?1");
        fields = heap_fields;
    }
    // open_tunnel owns all fields before this temporary preparation storage dies.
    return open_tunnel({.authority_ = pool_->config().authority_, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields}, config);
}

task<http_client_tunnel_result> http_client_handle::open_tunnel(const http_client_tunnel_request_view& head, http_client_tunnel_config config) const {
    registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes ||
        (head.protocol_.empty() ? (!is_valid_http_connect_authority(head.authority_) || !head.target_.empty()) : (!is_valid_http_method_token(head.protocol_) || !is_valid_http_origin_form_target(head.target_) || !parse_http_authority_host(borrowed_text(head.authority_)) || head.authority_.empty()))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    if (head.protocol_ == "connect-udp" && validate_http_connect_udp_request_to_send({.version_ = http_protocol_version::http2,
                                                                                         .method_ = "CONNECT",
                                                                                         .scheme_ = scheme() == http_scheme::https ? "https" : "http",
                                                                                         .authority_ = head.authority_,
                                                                                         .path_ = head.target_,
                                                                                         .headers_ = head.headers_})
                                               .has_value()) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    detail::http_client_request_storage request("CONNECT", head.target_, head.headers_, pmr_resource_or_default(resource_));
    request.set_tunnel(head.authority_, head.protocol_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
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

http_client_handle context::http_client() const {
    return services().client_registries().http_client(operation_scope_, capabilities_.stop_token());
}

http_client_handle context::http_client(std::string_view alias) const {
    return services().client_registries().http_client(alias, operation_scope_, capabilities_.stop_token());
}

}  // namespace ruvia
