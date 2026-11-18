#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "byte_view.h"
#include "context.h"
#include "context_access.h"
#include "decimal_text.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_client_request_storage.h"
#include "http_client_response_head.h"
#include "http_client_response_state.h"
#include "http_client_types.h"
#include "http_connect_udp.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_io_buffer.h"
#include "http_message.h"
#include "http_priority.h"
#include "http_proxy_forwarding.h"
#include "http_response_body.h"
#include "http_response_headers.h"
#include "http_status.h"
#include "io_operation.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "request_stream.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia::detail {

void http_client_response_memory_domain::charge_in_flight(std::size_t bytes_value) {
    if (bytes_value != 0 && !result_budget_domain_->reserve_in_flight(bytes_value)) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded), "HTTP client in-flight response memory budget is exhausted");
    }
}

void http_client_response_memory_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value != 0) {
        result_budget_domain_->release_in_flight(bytes_value);
    }
}

http_client_response_memory_domain::http_client_response_memory_domain(const worker_ref& worker_value, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) :
    worker_(worker_value),
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

http_client_response_memory_domain::owner http_client_response_memory_domain::create(const worker_ref& worker_value, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    return owner(new http_client_response_memory_domain(worker_value, result_budget_domain), deleter_type{});
}

void http_client_response_memory_domain::destroy() noexcept {
    delete this;
}

void http_client_response_memory_domain::deleter_type::operator()(http_client_response_memory_domain* domain) const noexcept {
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

http_client_response_state::http_client_response_state(http_client_response_memory_domain& memory_domain) :
    head_signal_(memory_domain.worker()),
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
    data_credit_.reset();
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

http_client_result_budget_domain::http_client_result_budget_domain(http_client_result_budget_config config) :
    max_retained_bytes_(config.max_retained_bytes_),
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
        if (retained_bytes_.compare_exchange_weak(retained, retained + bytes_value, std::memory_order_acq_rel, std::memory_order_relaxed)) {
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

http_client_result_budget_lease::http_client_result_budget_lease(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept :
    domain_(domain),
    bytes_(bytes_value) {}

http_client_result_budget_lease::http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept :
    domain_(std::move(other.domain_)),
    bytes_(std::exchange(other.bytes_, 0)) {}

http_client_result_budget_lease::~http_client_result_budget_lease() {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
    }
}

std::optional<http_client_result_budget_lease> http_client_result_budget_lease::try_acquire(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept {
    if (!domain->try_reserve(bytes_value)) {
        return std::nullopt;
    }
    return http_client_result_budget_lease(domain, bytes_value);
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
    if (!state_value.decode_content_ || plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() || plan->content_semantics() != http_response_content_semantics::with_content || state_value.status_.status() == http_status::partial_content) {
        return;
    }
    // Parsed once with the final head; decoding reuses this result at FIN.
    auto& parsed_coding = state_value.content_coding_.emplace(parse_http_content_coding_headers(state_value.headers_));
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
    state_value.body_decode_required_ = parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr || transforms_body;
    if (state_value.body_decode_required_) {
        state_value.collect_all_ = true;
    } else {
        state_value.content_coding_.reset();
    }
}

// Uses the parser-computed body plan and keeps decoded output bounded by the
// response's configured byte limit.
void decode_http_client_response_body(http_client_response_state& state_value, bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (!content_semantics_present || !state_value.body_decode_required_) {
        return;
    }
    try {
        const auto& parsed_coding = *state_value.content_coding_;
        if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP response Content-Encoding");
        }
        // Encoded bytes are never exposed, so the unread part of buffered_ and
        // pending_ together are the whole body. Decode in place when one holds it.
        const auto buffered_bytes = state_value.buffered_.size() - state_value.offset_;
        std::string_view encoded;
        if (state_value.pending_.empty()) {
            encoded = std::string_view(state_value.buffered_).substr(state_value.offset_);
        } else if (buffered_bytes == 0) {
            encoded = state_value.pending_;
        } else {
            if (state_value.pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_bytes) {
                throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
            }
            // Encoded bytes have no outstanding consumer borrow, and the
            // catch path discards them on failure: reuse their owned storage.
            state_value.buffered_.erase(0, state_value.offset_);
            state_value.offset_ = 0;
            state_value.buffered_.append(state_value.pending_);
            encoded = state_value.buffered_;
        }

        const auto decoded_limit = std::min(max_decoded_bytes, state_value.buffered_limit_);
        auto decoded = decode_http_content(parsed_coding.codings(), encoded, {.max_decoded_bytes_ = decoded_limit});
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
                return http_ascii_equals_ignore_case(field.name(), "Content-Encoding") || http_ascii_equals_ignore_case(field.name(), "Content-Length");
            });
            state_value.notify_producer_space();
            return;
        }
        const auto* failure = std::get_if<http_content_decode_error>(&decoded);
        if (failure != nullptr && *failure == http_content_decode_error::decoded_size_exceeded) {
            throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
        }
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP response Content-Encoding");
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

void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head) {
    const auto status = head.status();
    state_value.content_length_ = head.content_length();
    auto fields = std::move(head).take_headers();
    state_value.headers_ = std::move(fields);
    const auto body_plan = plan_http_response_body(state_value.request_method_, status);
    state_value.framing_ = body_plan.content_semantics() == http_response_content_semantics::connect_tunnel
        ? http_client_response_framing::tunnel
        : body_plan.body_suppressed()
        ? http_client_response_framing::none
        : http_client_response_framing::stream;
}

void publish_http_client_final_head(http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head) {
    if (state_value.upload_ && !state_value.upload_->output_.ended_) {
        const auto status = head.status_.status();
        const bool rewrite_upload_redirect = state_value.upload_->redirect_request_ != nullptr && (status == http_status::see_other || ((status == http_status::moved_permanently || status == http_status::found) && state_value.request_method_ == http_known_method::post));
        if (head.request_content_signal_ == http_client_request_content_signal::exchange_complete || rewrite_upload_redirect) {
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
        if (head.opens_tunnel_ && tunnel->udp_ && validate_http_connect_udp_response(state_value.protocol_version_, state_value.status_.status().value(), state_value.headers_).has_value()) {
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

void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout, bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
    decode_http_client_response_body(state_value, content_semantics_present, max_decoded_bytes);
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
}

}  // namespace ruvia::detail

namespace ruvia {

http_client_response_bytes::http_client_response_bytes(std::span<const char> buffered, std::span<const char> pending, detail::http_client_result_budget_lease&& lease_value) :
    data_(std::make_unique_for_overwrite<std::byte[]>(lease_value.size())),
    size_(lease_value.size()) {
    if (!buffered.empty()) {
        std::memcpy(data_.get(), buffered.data(), buffered.size());
    }
    if (!pending.empty()) {
        std::memcpy(data_.get() + buffered.size(), pending.data(), pending.size());
    }
    budget_domain_ = std::move(lease_value).take_domain();
}

http_client_response_bytes::http_client_response_bytes(http_client_response_bytes&& other) noexcept :
    budget_domain_(std::move(other.budget_domain_)),
    data_(std::move(other.data_)),
    size_(std::exchange(other.size_, 0)) {}

http_client_response_bytes& http_client_response_bytes::operator=(http_client_response_bytes&& other) noexcept {
    if (this != &other) {
        release();
        budget_domain_ = std::move(other.budget_domain_);
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
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
        budget_domain_->release(size_);
        budget_domain_.reset();
    }
    size_ = 0;
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
                case http_client_errc::websocket_eof_before_close:
                    return "WebSocket EOF before Close";
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

detail::http_client_response_state& require_response_state(detail::http_client_response_state* state_value) {
    if (state_value == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    return *state_value;
}

detail::http_client_response_state& require_response_body_state(detail::http_client_response_state* state_value) {
    auto& state = require_response_state(state_value);
    check_response_operation_affinity(&state);
    return state;
}

void prepare_forwarded_response_head(const detail::http_client_response_state& state, const detail::http_client_forwarded_response& output) {
    auto& head = detail::context_access::mutable_stream_head(output.context_);
    const auto fields = std::span<const http_header>(state.headers_);
    const auto nominated = parse_http_connection_nominated_fields(fields);
    if (nominated.status() != http_connection_fields_status::valid) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid or oversized HTTP response Connection field");
    }
    auto staged = head.headers().clone();
    staged.remove_header("content-length");
    staged.remove_header("transfer-encoding");
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!is_http_hop_by_hop_or_nominated_field(field->name(), nominated) && !http_ascii_equals_ignore_case(field->name(), "content-length")) {
            const bool repeated = std::any_of(fields.begin(), field, [&](const http_header& previous) {
                return http_ascii_equals_ignore_case(previous.name(), field->name());
            });
            staged.header(field->name(), field->value(), {.mode_ = repeated ? http_response_header_mode::append : http_response_header_mode::replace});
        }
    }
    const auto status = state.status_.status();
    const auto body_plan = plan_http_response_body(output.request_method_, status);
    std::optional<std::uint64_t> length;
    if (body_plan.explicit_content_length_allowed()) {
        if (state.request_method_ == http_known_method::head && !body_plan.body_suppressed()) {
            // A HEAD's representation length is not a body available to forward.
            length = 0;
        } else if (state.content_length_ && find_first_http_header(fields, "content-length") &&
            !find_first_http_header(fields, "transfer-encoding") &&
            !is_http_hop_by_hop_or_nominated_field("content-length", nominated) &&
            (state.framing_ == http_client_response_framing::fixed_length || state.framing_ == http_client_response_framing::none || state.framing_ == http_client_response_framing::stream)) {
            // Raw wire length survives decoding in state, but not in normalized
            // fields. Only an unchanged representation has a usable length.
            length = *state.content_length_ - std::min(*state.content_length_, state.consumed_body_bytes_);
        }
        if (output.protocol_version_ == http_protocol_version::http11 &&
            (!state.trailers_.empty() || (state.framing_ == http_client_response_framing::stream && !state.receive_complete()))) {
            // Multiplexed streams may still deliver unannounced trailing fields.
            // HTTP/1.1 needs chunked to preserve them without buffering the body.
            length.reset();
        }
    }
    if (length) {
        std::array<char, max_decimal_size> digits{};
        staged.header("content-length", format_decimal(digits, *length));
    }
    head.status(status);
    head.commit_headers(std::move(staged));
}

// Keep borrowed nomination storage out of the forwarding coroutine frame.
std::vector<http_header_view> prepare_forwarded_response_trailers(const detail::http_client_response_state& state) {
    std::vector<http_header_view> trailers;
    trailers.reserve(state.trailers_.size());
    const auto nominated = parse_http_connection_nominated_fields(state.headers_);
    if (nominated.status() != http_connection_fields_status::valid) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid or oversized HTTP response Connection field");
    }
    for (const auto& field : state.trailers_) {
        if (!is_http_hop_by_hop_trailer_field(field.name(), nominated)) {
            trailers.emplace_back(field.name(), field.value());
        }
    }
    return trailers;
}

// Each frame binds one scope. The destination frame also pins response storage
// while its source-bound consumer commits headers, writes and finishes.
task<void> pipe_response_in_destination([[maybe_unused]] http_client_response response, task<void> source) {
    co_await std::move(source);
}
}  // namespace

http_client_response::http_client_response(detail::http_client_pool& pool) :
    state_(pool.response_memory().create_state(pool)),
    body_(state_) {
    const auto& config = pool.config();
    state_->effective_connect_timeout_ = config.connect_timeout_;
    state_->effective_read_timeout_ = config.read_timeout_;
    state_->effective_write_timeout_ = config.write_timeout_;
    state_->effective_acquire_timeout_ = config.acquire_timeout_;
    state_->decode_content_ = config.decode_content_;
}

http_client_response::http_client_response(detail::http_client_response_state* state_value) noexcept :
    state_(state_value),
    body_(state_value),
    consumer_(false) {
    if (state_ != nullptr) {
        state_->retain_reference();
    }
}

http_client_response::http_client_response(http_client_response&& other) noexcept :
    state_(std::exchange(other.state_, nullptr)),
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
        throw http_client_error(make_error_code(http_client_errc::closing), "response transport is retired", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    state_value.pool_->reprioritize(state_value, priority);
}
std::span<const http_header> http_client_response::headers() const& {
    return require_response_state(state_).headers_;
}
std::vector<http_header> http_client_response::take_headers() && {
    auto& state_value = require_response_state(state_);
    check_response_operation_affinity(&state_value);
    return std::exchange(state_value.headers_, {});
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
            (void)timer.schedule(memory_domain_.worker(), *deadline, [&deadline_stop](worker_timer_outcome outcome) noexcept {
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
    while ((!head_ready_ || informational_active()) && !failure_ && !pool_closed_) {
        throw_if_read_interrupted();
        co_await head_signal_.wait();
    }
    if (!head_ready_ || informational_active() || informational_phase_ == informational_delivery_phase::failed || body_decode_required_) {
        rethrow_response_failure();
    }
    if (pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), head_message, std::nullopt, connection_info_.id_ != 0 ? &connection_info_ : nullptr);
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
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client encoded response failed", std::nullopt, connection_info_.id_ != 0 ? &connection_info_ : nullptr);
    }
}

namespace {
[[noreturn]] void throw_interrupted(const detail::http_client_response_state& state_value, detail::operation_interrupt_cause cause, std::string_view timed_out, std::string_view cancelled) {
    const bool timeout = cause == detail::operation_interrupt_cause::timed_out;
    throw http_client_error(timeout ? std::make_error_code(std::errc::timed_out) : std::make_error_code(std::errc::operation_canceled), timeout ? timed_out : cancelled, std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
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
            throw_interrupted(*this, error.code() == std::errc::timed_out ? operation_interrupt_cause::timed_out : operation_interrupt_cause::cancelled, "HTTP client read timed out", "HTTP client read cancelled");
        }
        throw;
    }
}

void detail::http_client_response_state::throw_if_output_interrupted() const {
    if (output_operation_ != nullptr && output_operation_->interrupted()) {
        throw_interrupted(*this, *output_operation_->cause(), "HTTP client write timed out", "HTTP client write cancelled");
    }
}

detail::http_client_operation_interruption::http_client_operation_interruption(http_client_response_state& state, http_client_operation_lane lane, const operation_options& operation) try :
    state_(state),
    lane_(lane),
    interrupt_(state.interrupt_source_, state.memory_domain().worker(), operation, &state, lane == http_client_operation_lane::read ? &wake_reader : &abort_output, lane == http_client_operation_lane::read ? "HTTP client read" : "HTTP client write") {
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
    if (!state_->receive_complete() || state_->offset_ != state_->buffered_.size() || !state_->pending_.empty()) {
        return false;
    }
    state_->rethrow_response_failure();
    if (state_->pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt, state_->connection_info_.id_ != 0 ? &state_->connection_info_ : nullptr);
    }
    return true;
}

task<std::optional<std::span<const std::byte>>> http_client_response_body::read(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return state_value.body_operation_scope_.attach(state_value.consume_body<std::span<const std::byte>>(std::move(operation)));
}

task<std::optional<std::string_view>> http_client_response_body::read_text(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return state_value.body_operation_scope_.attach(state_value.consume_body<std::string_view>(std::move(operation)));
}

template <typename view>
task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::http_client_response_state::consume_body(operation_options operation, std::conditional_t<std::is_void_v<view>, http_client_response_output, std::nullptr_t> output) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    [[maybe_unused]] const operation_steps steps(operation);
    const http_client_operation_interruption interruption(state_value, http_client_operation_lane::read, operation);
    if constexpr (std::is_void_v<view>) {
        if (output.forwarded_) {
            state_value.throw_if_read_interrupted();
            state_value.release_consumed_body_prefix();
            auto prepare = [&] {
                prepare_forwarded_response_head(state_value, *output.forwarded_);
            };
            try {
                if (co_await streaming_access::commit_prepared(output.writer_, callback_access::bind<void()>(prepare), state_value.downstream_step(steps)) == stream_commit_result::completed) {
                    co_return;
                }
            } catch (...) {
                state_value.rethrow_downstream_failure(steps);
            }
        }
    }
    for (;;) {
        // A forward is bounded as a whole: its bound also ends it between
        // chunks that are already buffered.
        if constexpr (std::is_void_v<view>) {
            state_value.throw_if_read_interrupted();
        }
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state_value.release_consumed_body_prefix();
        while ((state_value.body_decode_required_ && !state_value.receive_complete()) || (state_value.buffered_.empty() && state_value.pending_.empty() && !state_value.receive_complete())) {
            state_value.throw_if_read_interrupted();
            co_await state_value.data_signal_.wait();
        }
        promote_pending_data();
        if (state_value.offset_ == state_value.buffered_.size()) {
            state_value.rethrow_response_failure();
            if (state_value.pool_closed_) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw http_client_error(make_error_code(http_client_errc::closing), message, std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
            }
            state_value.discard_response_body();
            if constexpr (std::is_void_v<view>) {
                if (output.forwarded_) {
                    const auto trailers = state_value.trailers_.empty() ? std::vector<http_header_view>{} : prepare_forwarded_response_trailers(state_value);
                    try {
                        co_await output.writer_.finish(trailers, state_value.downstream_step(steps));
                    } catch (...) {
                        state_value.rethrow_downstream_failure(steps);
                    }
                }
                co_return;
            } else {
                co_return std::nullopt;
            }
        }
        const auto count = std::min(response_body_read_chunk_bytes, state_value.buffered_.size() - state_value.offset_);
        const auto chunk = std::string_view(state_value.buffered_).substr(state_value.offset_, count);
        if constexpr (std::is_void_v<view>) {
            try {
                co_await streaming_access::write_borrowed(output.writer_, chunk, state_value.downstream_step(steps));
            } catch (...) {
                state_value.rethrow_downstream_failure(steps);
            }
        }
        state_value.offset_ += count;
        state_value.consumed_body_bytes_ = saturating_add(state_value.consumed_body_bytes_, static_cast<std::uint64_t>(count));
        if constexpr (!std::is_void_v<view>) {
            if constexpr (std::same_as<view, std::string_view>) {
                co_return chunk;
            } else {
                co_return std::as_bytes(std::span(chunk.data(), chunk.size()));
            }
        }
    }
}

template task<std::optional<std::span<const std::byte>>> detail::http_client_response_state::consume_body<std::span<const std::byte>>(operation_options, std::nullptr_t);
template task<std::optional<std::string_view>> detail::http_client_response_state::consume_body<std::string_view>(operation_options, std::nullptr_t);
template task<void> detail::http_client_response_state::consume_body<void>(operation_options, detail::http_client_response_output);

task<http_client_response_bytes> http_client_response::collect_body(std::size_t max_bytes, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return state_value.body_operation_scope_.attach(state_value.collect_body<http_client_response_bytes>(max_bytes, std::move(operation)));
}

detail::operation_scope& http_client_response::body_scope() & {
    return require_response_body_state(state_).body_operation_scope_;
}

task<void> http_client_response::parse_response_body(detail::http_client_response_state& state, std::size_t max_bytes, operation_options operation, detail::callback_ref<void(std::string_view)> parse) {
    return state.collect_body<void>(max_bytes, std::move(operation), parse);
}

template <typename result_type>
task<result_type> detail::http_client_response_state::collect_body(std::size_t max_bytes, operation_options operation, callback_ref<void(std::string_view)> parse) {
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
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto remaining = state_value.buffered_.size() - state_value.offset_;
    const auto effective_limit = std::min(max_bytes, state_value.buffered_limit_);
    if (state_value.pending_.size() > effective_limit || remaining > effective_limit - state_value.pending_.size()) {
        throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response body exceeds its collection byte limit", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto total_remaining = remaining + state_value.pending_.size();
    if (total_remaining == 0) {
        state_value.discard_response_body();
        if constexpr (std::is_void_v<result_type>) {
            parse({});
            co_return;
        } else {
            co_return http_client_response_bytes{};
        }
    }
    auto reservation = detail::http_client_result_budget_lease::try_acquire(state_value.memory_domain_.result_budget_domain_, total_remaining);
    if (!reservation) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded), "HTTP client retained result byte budget is exhausted", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto consume = [&state_value, total_remaining]() noexcept {
        state_value.consumed_body_bytes_ = saturating_add(state_value.consumed_body_bytes_, static_cast<std::uint64_t>(total_remaining));
        state_value.discard_response_body();
    };
    const auto buffered = std::string_view(state_value.buffered_).substr(state_value.offset_);
    const std::string_view pending = state_value.pending_;
    if constexpr (std::is_void_v<result_type>) {
        if (buffered.empty() || pending.empty()) {
            // Parsing here is synchronous and produces an owning result. The
            // read lane keeps the borrowed bytes stable until parsing returns.
            try {
                parse(buffered.empty() ? pending : buffered);
            } catch (...) {
                consume();
                throw;
            }
            consume();
            co_return;
        }
    }
    http_client_response_bytes result_value(std::span(buffered), std::span(pending), std::move(*reservation));
    consume();
    if constexpr (std::is_void_v<result_type>) {
        parse(result_value.text());
        co_return;
    } else {
        co_return std::move(result_value);
    }
}

task<void> http_client_response_body::pipe_to(response_stream_writer& output, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return state_value.body_operation_scope_.attach(state_value.consume_body<void>(std::move(operation), {output}));
}

std::optional<std::string_view> http_client_response::header(std::string_view name) const& {
    return find_first_http_header(require_response_state(state_).headers_, name);
}

http_unique_field_lookup http_client_response::unique_header(std::string_view name) const& {
    return find_unique_http_header(require_response_state(state_).headers_, name);
}

std::optional<std::string_view> http_client_response::trailer(std::string_view name) const& {
    return find_first_http_header(require_response_state(state_).trailers_, name);
}

task<void> http_client_response::pipe_to(context& output, operation_options operation) & {
    return pipe_to_destination(output, output.stream(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to(request_stream& output, operation_options operation) & {
    return pipe_to_destination(output.context_, output.stream(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to_destination(context& output, response_stream_writer& writer, detail::operation_scope& destination, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    state_->require_owner_worker("HTTP response requires its owner worker");
    if (!state_->body_operation_scope_.active() || !destination.active()) {
        throw std::logic_error("HTTP response pipe_to scope is closed");
    }
    detail::http_client_response_output target{writer, detail::http_client_forwarded_response{output, output.req().known_method(), output.req().protocol_version()}};
    auto source = state_->body_operation_scope_.attach(state_->consume_body<void>(std::move(operation), target));
    return destination.attach(pipe_response_in_destination(http_client_response(state_), std::move(source)));
}

}  // namespace ruvia
