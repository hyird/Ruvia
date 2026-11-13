#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_pmr_string.h"
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
    http_clear_pmr_string_retaining_small(buffered_, response_body_read_chunk_bytes);
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
    http_clear_pmr_string_retaining_small(state_value.buffered_, response_body_read_chunk_bytes);
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
      pool_owner_(other.pool_owner_),
      resource_(other.resource_),
      options_(other.options_),
      host_stop_token_(other.host_stop_token_),
      registration_(other.registration_, this),
      registry_(other.registry_) {}

void http_client_handle::expire_capability(void* target) noexcept {
    auto& self = *static_cast<http_client_handle*>(target);
    self.pool_ = nullptr;
    self.pool_owner_.reset();
}

http_client_handle http_client_handle::with_options(operation_options options) const {
    registration_.require_active();
    http_client_handle copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

task<http_client_response> http_client_handle::dispatch(
    const http_client_request_view& view, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    detail::http_client_request_storage request(view.method_.view(), view.target_.view(), view.headers_, resource_);
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

task<http_client_exchange> http_client_handle::dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& request_options,
    const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_.value() <= std::chrono::milliseconds::zero() ||
        (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), head.headers_, resource_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

task<http_client_tunnel_result> http_client_handle::dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options,
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
    // The dispatch owns fields before temporary preparation storage dies.
    const std::string_view authority = request_options.authority_ ? std::string_view(*request_options.authority_) : std::string_view(pool_->config().authority_);
    return dispatch_connect({.authority_ = authority, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields}, config, request_options, informational);
}

task<http_client_tunnel_result> http_client_handle::dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options,
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
    detail::http_client_request_storage request("CONNECT", head.target_.view(), head.headers_, pmr_resource_or_default(resource_));
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_tunnel(head.authority_.view(), head.protocol_.view());
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
}

http_client_stats http_client_handle::stats() const {
    registration_.require_active();
    return pool_->stats();
}

std::pmr::vector<http_client_endpoint_address> http_client_handle::dns_snapshot() const {
    registration_.require_active();
    return pool_->dns_snapshot();
}
void http_client_handle::update_dns(std::span<const http_client_endpoint_address> addresses) const {
    registration_.require_active();
    pool_->update_dns(addresses);
}
task<void> http_client_handle::refresh_dns() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), refresh_client_dns(pool_owner_, options_, host_stop_token_));
}
bool http_client_handle::close_connection(std::uint64_t id) const {
    registration_.require_active();
    return pool_->close_connection(id);
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

http_origin_view http_client_handle::origin() const& {
    registration_.require_active();
    return pool_->origin();
}

http_client_handle context::client() const {
    return services().client_registries().client(operation_scope_, capabilities_.stop_token());
}

http_client_handle context::client(std::string_view alias) const {
    return services().client_registries().client(alias, operation_scope_, capabilities_.stop_token());
}

}  // namespace ruvia
