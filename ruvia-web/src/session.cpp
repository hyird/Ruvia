#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/web/context.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/http/http_header.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/redis/redis_session.h"

#include "context_access.h"
#include "operation_interrupt.h"
#endif

#include <algorithm>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <system_error>
#include <type_traits>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"
#include "ruvia/web/streaming.h"

#include "connection_lifecycle.h"
#include "context_access.h"
#include "http_websocket_connection.h"
#include "operation_interrupt.h"
#include "route_table.h"

namespace ruvia {
namespace detail {

void context_session_state::bind(const session_middleware* owner_value) {
    if (available_) {
        throw std::logic_error("session middleware is already bound");
    }
    require_mutable();
    owner_ = owner_value;
    available_ = true;
}

void context_session_state::require_mutable() const {
    if (phase_ != phase_type::active) {
        throw std::logic_error("session cannot change after response commit begins");
    }
}

bool context_session_state::begin_commit() {
    if (!available_ || phase_ == phase_type::committed) {
        return false;
    }
    if (phase_ == phase_type::failed) {
        std::rethrow_exception(failure_);
    }
    require_mutable();
    phase_ = phase_type::committing;
    return true;
}

void context_session_state::finish_commit() noexcept {
    phase_ = phase_type::committed;
}

void context_session_state::fail_commit(std::exception_ptr exception) noexcept {
    failure_ = std::move(exception);
    phase_ = phase_type::failed;
}

void context_session_state::observe_presented_id(std::string_view id) {
    require_mutable();
    value_.template emplace<session_unrecognized>(std::string(id));
}

void context_session_state::load_recognized(std::string_view data) {
    require_mutable();
    auto* presented = std::get_if<session_unrecognized>(&value_);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    auto data_copy = std::string(data);
    auto id = std::move(presented->id_);
    value_.template emplace<session_loaded>(std::move(id), std::move(data_copy));
}

void context_session_state::set(std::string_view data) {
    require_mutable();
    if (data.empty()) {
        clear();
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        auto data_copy = std::string(data);
        auto id = std::move(loaded_state->id_);
        value_.template emplace<session_rotate>(std::move(id), std::move(data_copy));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        rotated->data_.assign(data);
        return;
    }
    if (auto* fresh = std::get_if<session_persist_new>(&value_)) {
        fresh->data_.assign(data);
        return;
    }
    if (auto* cleared = std::get_if<session_clear>(&value_)) {
        if (!cleared->old_id_.has_value()) {
            value_.template emplace<session_persist_new>(std::string(data));
            return;
        }
        auto data_copy = std::string(data);
        auto old_id = std::move(*cleared->old_id_);
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data_copy));
        return;
    }
    value_.template emplace<session_persist_new>(std::string(data));
}

void context_session_state::clear() {
    require_mutable();
    if (std::get_if<session_clear>(&value_) != nullptr) {
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        std::optional<std::string> id(std::move(loaded_state->id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        std::optional<std::string> id(std::move(rotated->old_id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    value_.template emplace<session_clear>(std::nullopt);
}

void context_session_state::regenerate() {
    require_mutable();
    auto* loaded_state = std::get_if<session_loaded>(&value_);
    if (loaded_state == nullptr) {
        return;
    }
    auto old_id = std::move(loaded_state->id_);
    auto data = std::move(loaded_state->data_);
    if (data.empty()) {
        value_.template emplace<session_clear>(
            std::optional<std::string>(std::move(old_id)));
    } else {
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data));
    }
}

std::string_view context_session_state::data() const& noexcept {
    if (const auto* state = std::get_if<session_loaded>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_persist_new>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_rotate>(&value_)) {
        return state->data_;
    }
    return {};
}

}  // namespace detail

std::string_view session::data() const& noexcept {
    return state_->data();
}

void session::set(std::string_view data) {
    state_->set(data);
}

void session::clear() {
    state_->clear();
}

void session::regenerate() {
    state_->regenerate();
}

session context::session() {
    if (!session_state().available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return ruvia::session(session_state());
}

#ifdef RUVIA_ENABLE_REDIS
task<void> detail::session_access::commit(context& context_value) {
    auto& state_value = context_value.session_state();
    if (!state_value.begin_commit()) {
        co_return;
    }
    try {
        if (const auto* owner = state_value.owner()) {
            co_await owner->commit(context_value);
        }
        state_value.finish_commit();
    } catch (...) {
        state_value.fail_commit(std::current_exception());
        throw;
    }
}
#endif

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia {
namespace {

// "<key prefix><session id>", inline for typical key sizes.
using session_key = detail::inline_text_concat<128>;

}  // namespace

const session_config& session_middleware::validate(const session_config& source_value) {
    if (source_value.redis_alias_.empty()) {
        throw std::invalid_argument("session Redis alias must not be empty");
    }
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("session cookie name must be a valid HTTP token");
    }
    if (source_value.key_prefix_.empty()) {
        throw std::invalid_argument("session key prefix must not be empty");
    }
    if (source_value.ttl_.count() <= 0) {
        throw std::invalid_argument("session TTL must be greater than zero");
    }
    // New sessions expire through SET PX in milliseconds; rotation sends the
    // same TTL as EX seconds, which Redis likewise converts to milliseconds.
    if (source_value.ttl_ > std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())) {
        throw std::invalid_argument("session TTL must be representable in milliseconds");
    }
    return source_value;
}

session_middleware::session_middleware()
    : session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config)
    : redis_alias_(validate(config).redis_alias_),
      cookie_name_(config.cookie_name_),
      key_prefix_(config.key_prefix_),
      ttl_(config.ttl_) {}

task<void> session_middleware::invoke(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    const auto cookie = c.req().cookie(cookie_name_);
    if (cookie && detail::is_valid_session_id(*cookie)) {
        detail::session_access::observe_presented_id(c, *cookie);
        auto get_task = [&] {
            const session_key key({key_prefix_, *cookie});
            return c.redis(redis_alias_).get(key.view());
        }();
        if (auto stored = co_await std::move(get_task)) {
            detail::session_access::load(c, *stored);
        }
    }

    co_await next_value();
    co_await detail::session_access::commit(c);
}

task<void> session_middleware::commit(context& c) const {
    const auto& state_value = detail::session_access::state(c);
    if (state_value.untouched() != nullptr || state_value.unrecognized() != nullptr ||
        state_value.loaded() != nullptr) {
        co_return;
    }

    const auto connection = c.conn();
    const bool secure = connection.scheme() == http_scheme::https;
    // Session Cookie 属于明确的跨替换响应策略，只在最终交付/流提交时应用。
    // Redis 成功后通过无分配移动发布完整字段，既有 commit guard 保证只持久化一次。
    auto& response = detail::context_access::response_policy(c);
    auto staged = response.headers().clone(1);
    const auto publish = [&]() noexcept {
        response.commit_headers(std::move(staged));
    };
    if (const auto* cleared = state_value.cleared()) {
        detail::append_expired_session_cookie_header(staged, cookie_name_, secure);
        if (cleared->old_id_.has_value()) {
            auto del_task = [&] {
                const session_key key({key_prefix_, *cleared->old_id_});
                return c.redis(redis_alias_).del(key.view());
            }();
            (void)(co_await std::move(del_task));
        }
        publish();
        co_return;
    }

    std::string_view data;
    std::string_view old_id;
    if (const auto* rotated = state_value.rotate()) {
        data = rotated->data_;
        old_id = rotated->old_id_;
    } else {
        data = state_value.persist_new()->data_;
    }

    std::array<char, detail::secure_token_size> id_buffer;
    const auto new_id = detail::generate_secure_token(id_buffer);
    detail::append_session_cookie_header(staged, cookie_name_, new_id, secure);

    const session_key key({key_prefix_, new_id});
    bool applied = false;
    if (!old_id.empty()) {
        auto eval_task = [&] {
            const session_key old_key({key_prefix_, old_id});
            // session_middleware::validate requires a positive TTL.
            std::array<char, http_max_unsigned_decimal_size> ttl_buffer;
            const auto ttl = http_format_unsigned(ttl_buffer, static_cast<std::uint64_t>(ttl_.count()));
            const std::array<std::string_view, 2> keys{old_key.view(), key.view()};
            const std::array<std::string_view, 2> args{data, ttl};
            // Redis executes the entire transition atomically. A stale request may
            // neither recreate a revoked identifier nor mint a successor from it.
            return c.redis(redis_alias_).eval(detail::session_rotation_script, keys, args);
        }();
        const auto result_value = co_await std::move(eval_task);
        if (result_value.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, result_value.error());
        }
        applied = result_value.integer() == 1;
    } else {
        auto set_task = [&] {
            redis_set_options options;
            options.condition_ = redis_set_condition::if_absent;
            options.expiration_ = redis_set_expiration::expires_after(ttl_);
            return c.redis(redis_alias_).set(key.view(), data, std::move(options));
        }();
        const auto result_value = co_await std::move(set_task);
        applied = result_value.applied();
    }
    if (!applied) {
        throw http_error({.status_ = http_status::conflict,
            .code_ = "session_conflict",
            .message_ = "session expired or was revoked; retry with a new session"});
    }
    publish();
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS

namespace ruvia::detail {

namespace {

constexpr std::uint64_t empty_hash = 0;
constexpr rate_limit_scope default_scope{.route_ = 1};
constexpr std::uintptr_t fallback_route_scope = 2;

[[nodiscard]] rate_limit_hash_key random_hash_key() {
    std::random_device source;
    rate_limit_hash_key key{};
    for (auto& word : key) {
        word = (std::uint64_t{source()} << 32) | source();
    }
    return key;
}

[[nodiscard]] std::uint64_t load_little_endian(const unsigned char* bytes, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value |= std::uint64_t{bytes[index]} << (8 * index);
    }
    return value;
}

void store_little_endian(unsigned char* bytes, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<unsigned char>(value >> (8 * index));
    }
}

struct siphash_state final {
    std::uint64_t v0_;
    std::uint64_t v1_;
    std::uint64_t v2_;
    std::uint64_t v3_;

    void round() noexcept {
        v0_ += v1_;
        v1_ = std::rotl(v1_, 13);
        v1_ ^= v0_;
        v0_ = std::rotl(v0_, 32);
        v2_ += v3_;
        v3_ = std::rotl(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = std::rotl(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = std::rotl(v1_, 17);
        v1_ ^= v2_;
        v2_ = std::rotl(v2_, 32);
    }

    void compress(std::uint64_t word) noexcept {
        v3_ ^= word;
        round();
        v0_ ^= word;
    }
};

// SipHash-1-3: the keyed PRF behind Rust's std hasher, the grade of keyed hash
// pingora-limits places its counters with.
[[nodiscard]] std::uint64_t siphash13(const rate_limit_hash_key& key, std::span<const unsigned char> message) noexcept {
    siphash_state state{
        .v0_ = key[0] ^ 0x736f6d6570736575ULL,
        .v1_ = key[1] ^ 0x646f72616e646f6dULL,
        .v2_ = key[0] ^ 0x6c7967656e657261ULL,
        .v3_ = key[1] ^ 0x7465646279746573ULL,
    };
    const auto whole_words = message.size() - message.size() % 8;
    for (std::size_t offset = 0; offset < whole_words; offset += 8) {
        state.compress(load_little_endian(message.data() + offset, 8));
    }
    state.compress((static_cast<std::uint64_t>(message.size()) << 56) |
                   load_little_endian(message.data() + whole_words, message.size() - whole_words));
    state.v2_ ^= 0xff;
    state.round();
    state.round();
    state.round();
    return state.v0_ ^ state.v1_ ^ state.v2_ ^ state.v3_;
}

// The caller bounds `key` by rate_limit_max_key_bytes.
[[nodiscard]] std::uint64_t key_hash(
    const rate_limit_hash_key& hash_key, rate_limit_scope scope, std::string_view key) noexcept {
    std::array<unsigned char, 16 + rate_limit_max_key_bytes> message{};
    store_little_endian(message.data(), static_cast<std::uint64_t>(scope.route_));
    store_little_endian(message.data() + 8, static_cast<std::uint64_t>(scope.rule_));
    if (!key.empty()) {
        std::memcpy(message.data() + 16, key.data(), key.size());
    }
    const auto hash = siphash13(hash_key, std::span(message).first(16 + key.size()));
    return hash == empty_hash ? 1 : hash;
}

[[nodiscard]] std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr auto max_value = std::numeric_limits<std::uint64_t>::max();
    return right > max_value - left ? max_value : left + right;
}

// An unsigned 128-bit value, high word first so the defaulted ordering is the
// numeric one. The estimate's products need it: a count and a window length
// each span the full 64-bit range a rule allows.
struct wide_unsigned final {
    std::uint64_t high_{0};
    std::uint64_t low_{0};

    friend constexpr auto operator<=>(const wide_unsigned&, const wide_unsigned&) noexcept = default;
};

[[nodiscard]] constexpr wide_unsigned multiply_wide(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr std::uint64_t half_mask = 0xffff'ffffULL;
    const auto low_low = (left & half_mask) * (right & half_mask);
    const auto low_high = (left & half_mask) * (right >> 32);
    const auto high_low = (left >> 32) * (right & half_mask);
    const auto high_high = (left >> 32) * (right >> 32);
    const auto middle = (low_low >> 32) + (low_high & half_mask) + (high_low & half_mask);
    return {.high_ = high_high + (low_high >> 32) + (high_low >> 32) + (middle >> 32),
        .low_ = (middle << 32) | (low_low & half_mask)};
}

// Requires a nonzero value.
[[nodiscard]] constexpr wide_unsigned wide_predecessor(wide_unsigned value) noexcept {
    return value.low_ == 0 ? wide_unsigned{.high_ = value.high_ - 1, .low_ = std::numeric_limits<std::uint64_t>::max()}
                           : wide_unsigned{.high_ = value.high_, .low_ = value.low_ - 1};
}

// floor(dividend / divisor) by restoring long division. Requires
// dividend.high_ < divisor, which is exactly when the quotient fits 64 bits.
[[nodiscard]] constexpr std::uint64_t divide_wide(wide_unsigned dividend, std::uint64_t divisor) noexcept {
    if (dividend.high_ == 0) {
        return dividend.low_ / divisor;
    }
    auto remainder = dividend.high_;
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        // A set top bit makes the shifted remainder 2^64 or more, above any
        // divisor; the wrapping subtraction below still yields the true value.
        const bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((dividend.low_ >> bit) & 1U);
        quotient <<= 1;
        if (carry || remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1U;
        }
    }
    return quotient;
}

// Configuration validation and rate_limit's static_assert keep every window
// positive.
[[nodiscard]] std::uint64_t window_length_ms(const rate_limit_rule& rule) noexcept {
    return static_cast<std::uint64_t>(rule.window_.count());
}

[[nodiscard]] std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point now) noexcept {
    return now <= start ? 0 : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

[[nodiscard]] bool key_equals(
    const rate_limit_slot& slot, rate_limit_scope scope, std::string_view key, std::uint64_t hash) noexcept {
    return slot.key_hash_ == hash && slot.scope_ == scope && slot.key_size_ == key.size() &&
           std::ranges::equal(key, std::span(slot.key_).first(slot.key_size_));
}

[[nodiscard]] bool stale(const rate_limit_slot& slot, std::chrono::steady_clock::time_point now) noexcept {
    return slot.key_hash_ != empty_hash && elapsed_ms(slot.window_start_, now) >= slot.stale_after_ms_;
}

// Moves the key to the window containing now. After one full window the
// current count becomes the previous one; after two or more neither count is
// inside the trailing window any more, so the key restarts as a new one.
void roll_window(rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    const auto elapsed = elapsed_ms(slot.window_start_, now);
    if (elapsed < window_ms) {
        return;
    }
    if (elapsed - window_ms < window_ms) {
        slot.previous_count_ = slot.current_count_;
        // The elapsed check guarantees this advance fits before now.
        slot.window_start_ += saturating_duration_cast(rule.window_);
    } else {
        slot.previous_count_ = 0;
        slot.window_start_ = now;
    }
    slot.current_count_ = 0;
}

// Refusals leave the counts alone, so the estimate only decays: the previous
// count fades out across the current window, and at the boundary the current
// count becomes the previous one. Retry-After is the first millisecond at
// which the admission test passes again; the reset is when the estimate
// reaches zero.
[[nodiscard]] rate_limit_rejection refusal(const rate_limit_rule& rule, std::uint64_t previous,
    std::uint64_t current, std::uint64_t window_ms, std::uint64_t remaining_ms) noexcept {
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    std::uint64_t retry_after_ms = 0;
    if (current >= limit) {
        // The next window opens with previous == limit and passes the test
        // once one millisecond of it has elapsed.
        retry_after_ms = saturating_add(remaining_ms, 1);
    } else {
        // Refused with room left means previous * remaining >= room * window,
        // so previous > 0 and the largest passing remaining,
        // floor((room * window - 1) / previous), lies below remaining.
        const auto passing_remaining_ms =
            divide_wide(wide_predecessor(multiply_wide(limit - current, window_ms)), previous);
        retry_after_ms = remaining_ms - passing_remaining_ms;
    }
    // Windows are below 2^63 ms, so this sum and the retry sum above stay exact.
    const auto reset_after_ms = current == 0 ? remaining_ms : saturating_add(remaining_ms, window_ms);
    return rate_limit_rejection(retry_after_ms, reset_after_ms, rule.max_requests_);
}

// Admits while previous * remaining / window + current < max_requests_, where
// remaining is the part of the current window still ahead, i.e. the share of
// the previous window that the trailing window still covers. Both sides are
// scaled by the window so the comparison is exact.
[[nodiscard]] std::optional<rate_limit_rejection> consume(
    rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    roll_window(slot, rule, now);
    const auto remaining_ms = window_ms - elapsed_ms(slot.window_start_, now);
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    const auto previous = static_cast<std::uint64_t>(slot.previous_count_);
    const auto current = static_cast<std::uint64_t>(slot.current_count_);
    if (current < limit && multiply_wide(previous, remaining_ms) < multiply_wide(limit - current, window_ms)) {
        ++slot.current_count_;
        return std::nullopt;
    }
    return refusal(rule, previous, current, window_ms, remaining_ms);
}

[[nodiscard]] std::optional<rate_limit_rejection> consume_new_key(rate_limit_slot& slot, rate_limit_scope scope,
    std::string_view key, std::uint64_t hash, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    slot.scope_ = scope;
    slot.key_hash_ = hash;
    slot.window_start_ = now;
    const auto window_ms = window_length_ms(rule);
    slot.stale_after_ms_ = saturating_add(window_ms, window_ms);
    slot.current_count_ = 0;
    slot.previous_count_ = 0;
    slot.key_size_ = static_cast<std::uint8_t>(key.size());
    std::ranges::copy(key, slot.key_.begin());
    return consume(slot, rule, now);
}

[[nodiscard]] std::optional<rate_limit_rejection> overflow_decision(const rate_limit_rule& rule) noexcept {
    return rule.overflow_policy_ == rate_limit_overflow_policy::allow
               ? std::nullopt
               : std::optional(rate_limit_rejection(1, 1, rule.max_requests_));
}

[[nodiscard]] rate_limit_decision consume_decision(
    rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    roll_window(slot, rule, now);
    const auto remaining_ms = window_ms - elapsed_ms(slot.window_start_, now);
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    const auto previous = static_cast<std::uint64_t>(slot.previous_count_);
    const auto current = static_cast<std::uint64_t>(slot.current_count_);
    const auto prev_weighted = multiply_wide(previous, remaining_ms);
    if (current < limit && prev_weighted < multiply_wide(limit - current, window_ms)) {
        ++slot.current_count_;
        const auto next_current = current + 1U;
        const auto room_after_scaled = multiply_wide(limit - next_current, window_ms);
        std::size_t remaining_requests = 0;
        if (prev_weighted <= room_after_scaled) {
            const std::uint64_t borrow = room_after_scaled.low_ < prev_weighted.low_ ? 1U : 0U;
            const wide_unsigned diff{
                .high_ = room_after_scaled.high_ - prev_weighted.high_ - borrow,
                .low_ = room_after_scaled.low_ - prev_weighted.low_,
            };
            remaining_requests = static_cast<std::size_t>(divide_wide(diff, window_ms));
        }
        return rate_limit_decision{
            .allowed_ = true,
            .limit_ = rule.max_requests_,
            .remaining_ = remaining_requests,
            .retry_after_ms_ = 0,
            .reset_after_ms_ = saturating_add(remaining_ms, window_ms),
        };
    }
    const auto rejected = refusal(rule, previous, current, window_ms, remaining_ms);
    return rate_limit_decision{
        .allowed_ = false,
        .limit_ = rejected.max_requests(),
        .remaining_ = 0,
        .retry_after_ms_ = rejected.retry_after_ms(),
        .reset_after_ms_ = rejected.reset_after_ms(),
    };
}

[[nodiscard]] rate_limit_decision consume_new_key_decision(rate_limit_slot& slot,
    rate_limit_scope scope, std::string_view key, std::uint64_t hash,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    slot.scope_ = scope;
    slot.key_hash_ = hash;
    slot.window_start_ = now;
    const auto window_ms = window_length_ms(rule);
    slot.stale_after_ms_ = saturating_add(window_ms, window_ms);
    slot.current_count_ = 0;
    slot.previous_count_ = 0;
    slot.key_size_ = static_cast<std::uint8_t>(key.size());
    std::ranges::copy(key, slot.key_.begin());
    return consume_decision(slot, rule, now);
}

[[nodiscard]] rate_limit_decision overflow_rate_limit_decision(const rate_limit_rule& rule) noexcept {
    if (rule.overflow_policy_ == rate_limit_overflow_policy::allow) {
        return rate_limit_decision{
            .allowed_ = true,
            .limit_ = rule.max_requests_,
            .remaining_ = 0,
            .retry_after_ms_ = 0,
            .reset_after_ms_ = window_length_ms(rule),
        };
    }
    return rate_limit_decision{
        .allowed_ = false,
        .limit_ = rule.max_requests_,
        .remaining_ = 0,
        .retry_after_ms_ = 1,
        .reset_after_ms_ = 1,
    };
}
}  // namespace

void validate_rate_limit_config(const rate_limit_config& config) {
    const auto& rule = config.rule_;
    if (rule.max_requests_ == 0) {
        throw std::invalid_argument("rate limit max requests must be greater than zero");
    }
    if (rule.window_.count() <= 0) {
        throw std::invalid_argument("rate limit window must be greater than zero");
    }
    if (rule.overflow_policy_ != rate_limit_overflow_policy::deny &&
        rule.overflow_policy_ != rate_limit_overflow_policy::allow) {
        throw std::invalid_argument("rate limit overflow policy is invalid");
    }
    if (!std::has_single_bit(config.capacity_per_worker_)) {
        throw std::invalid_argument(
            "rate-limit capacity per worker must be a power of two");
    }
}

rate_limiter::rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
    route_rate_limit_presence route_rules, std::size_t capacity_value)
    : default_rule_per_worker_(default_rule_per_worker) {
    // http_server_options::set_rate_limit, the only writer of the rule and the
    // capacity, validated both; the capacity is a power of two for the mask.
    if (default_rule_per_worker_.has_value() || route_rules == route_rate_limit_presence::present) {
        slots_.resize(capacity_value);
        hash_key_ = random_hash_key();
    }
}

void rate_limiter::adopt_route_storage(rate_limiter& prepared) noexcept {
    if (slots_.empty()) {
        slots_.swap(prepared.slots_);
        std::swap(hash_key_, prepared.hash_key_);
    }
}

std::optional<rate_limit_rejection> rate_limiter::allow_default(
    std::string_view remote_address, std::chrono::steady_clock::time_point now) noexcept {
    return default_rule_per_worker_.has_value()
               ? allow(default_scope, remote_address, *default_rule_per_worker_, now)
               : std::nullopt;
}

std::optional<rate_limit_rejection> rate_limiter::allow_route(std::uintptr_t route_scope,
    const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule,
    std::chrono::steady_clock::time_point now) noexcept {
    return allow({.route_ = route_scope == 0 ? fallback_route_scope : route_scope,
                     .rule_ = reinterpret_cast<std::uintptr_t>(rule_declaration)},
        remote_address, rule, now);
}

std::optional<rate_limit_rejection> rate_limiter::allow(rate_limit_scope scope, std::string_view key,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_decision(rule);
    }

    // Preserve the arrival's sub-millisecond phase; only elapsed time is quantized.
    now = std::max(now, std::chrono::steady_clock::time_point{});
    const auto hash = key_hash(hash_key_, scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;

    // Slots never return to empty, so a key is installed and later found
    // within the same bounded probe window from its home slot. A saturated
    // probe window costs a fixed probe count instead of a scan of the whole
    // table, like a pingora-limits estimator's fixed per-operation cost.
    const auto probe_window = std::min(slots_.size(), rate_limit_probe_window);
    for (std::size_t probe_value = 0; probe_value < probe_window; ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            return consume_new_key(reclaimable == nullptr ? slot : *reclaimable, scope, key, hash, rule, now);
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume(slot, rule, now);
        }
        if (reclaimable == nullptr && stale(slot, now)) {
            reclaimable = &slot;
        }
    }

    if (reclaimable != nullptr) {
        return consume_new_key(*reclaimable, scope, key, hash, rule, now);
    }
    return overflow_decision(rule);
}

rate_limit_decision rate_limiter::decide_default(
    std::string_view key, std::chrono::steady_clock::time_point now) noexcept {
    if (!default_rule_per_worker_.has_value()) {
        return rate_limit_decision{};
    }
    return decide_rule(default_scope, key, *default_rule_per_worker_, now);
}

rate_limit_decision rate_limiter::decide_rule(rate_limit_scope scope, std::string_view key,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_rate_limit_decision(rule);
    }
    now = std::max(now, std::chrono::steady_clock::time_point{});
    const auto hash = key_hash(hash_key_, scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;
    const auto probe_window = std::min(slots_.size(), rate_limit_probe_window);
    for (std::size_t probe_value = 0; probe_value < probe_window; ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            return consume_new_key_decision(
                reclaimable == nullptr ? slot : *reclaimable, scope, key, hash, rule, now);
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume_decision(slot, rule, now);
        }
        if (reclaimable == nullptr && stale(slot, now)) {
            reclaimable = &slot;
        }
    }
    if (reclaimable != nullptr) {
        return consume_new_key_decision(*reclaimable, scope, key, hash, rule, now);
    }
    return overflow_rate_limit_decision(rule);
}

namespace {

[[nodiscard]] std::optional<rate_limit_rejection> decide_route_rate_limit(
    context& context_value, const void* declaration, const rate_limit_rule& rule) noexcept {
    auto* limiter = context_access::rate_limiter(context_value);
    if (limiter == nullptr) {
        return std::nullopt;
    }

    char key_buffer[rate_limit_key_buffer_bytes];
    // The client, not the hop -- same as the app-wide limiter. Behind a trusted
    // proxy, remote() is the proxy and every caller would share one key.
    return limiter->allow_route(context_access::route_rate_limit_scope(context_value), declaration,
        rate_limit_key_for(context_value.conn().client().address(), key_buffer), rule,
        rate_limit_instant(context_access::rate_limit_arrival(context_value)));
}

void set_unsigned_header(http_response& response, std::string_view name, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> buffer;
    response.header(name, http_format_unsigned(buffer, value));
}

// Rounds up in the unsigned millisecond domain, where every delay the limiter
// reports fits: a client waiting the whole seconds named must not arrive
// before the millisecond the limiter computed.
[[nodiscard]] std::uint64_t whole_seconds_after(std::uint64_t delay_ms) noexcept {
    return delay_ms / 1000 + (delay_ms % 1000 == 0 ? 0 : 1);
}

}  // namespace

http_error_info rate_limit_rejection_error() noexcept {
    return http_error_info({.status_ = ruvia::http_status::too_many_requests,
        .code_ = "too_many_requests",
        .message_ = "rate limit exceeded"});
}

// Remaining is 0 because the refusal itself shows the estimate has no room.
// Retry-After names when the estimate admits the next request; Reset names
// when it has drained to zero and the whole limit is available again.
void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection) {
    response.reserve_headers(response.headers().size() + 4U);
    set_unsigned_header(response, "Retry-After", whole_seconds_after(rejection.retry_after_ms()));
    set_unsigned_header(response, "X-RateLimit-Limit", rejection.max_requests());
    response.header_stable_view("X-RateLimit-Remaining", "0");
    set_unsigned_header(response, "X-RateLimit-Reset", whole_seconds_after(rejection.reset_after_ms()));
}

void enforce_route_rate_limit(context& context_value, const void* declaration, const rate_limit_rule& rule) {
    if (const auto rejection = decide_route_rate_limit(context_value, declaration, rule)) {
        throw rate_limit_rejected(*rejection);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

std::string_view format_rate_limit_address_key(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    return detail::rate_limit_key_for(address, buffer);
}

std::string_view format_rate_limit_address_key(
    std::string_view address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    return detail::rate_limit_key_for(address, buffer);
}

void apply_rate_limit_headers(http_response& response, const rate_limit_decision& decision) {
    response.reserve_headers(response.headers().size() + (decision.allowed() ? 3U : 4U));
    if (!decision.allowed()) {
        detail::set_unsigned_header(response, "Retry-After", decision.retry_after_seconds());
    }
    detail::set_unsigned_header(response, "X-RateLimit-Limit", decision.limit());
    detail::set_unsigned_header(response, "X-RateLimit-Remaining", decision.remaining());
    detail::set_unsigned_header(response, "X-RateLimit-Reset", decision.reset_after_seconds());
}

struct sliding_window_rate_limiter::impl_type final {
    impl_type(std::optional<rate_limit_rule> default_rule, std::size_t capacity_value)
        : limiter_(default_rule, detail::route_rate_limit_presence::present, capacity_value) {}

    detail::rate_limiter limiter_;
};

sliding_window_rate_limiter::sliding_window_rate_limiter(const rate_limit_config& config) {
    detail::validate_rate_limit_config(config);
    impl_ = std::make_unique<impl_type>(config.rule_, config.capacity_per_worker_);
}

sliding_window_rate_limiter::sliding_window_rate_limiter(std::size_t capacity_per_worker) {
    if (!std::has_single_bit(capacity_per_worker)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    impl_ = std::make_unique<impl_type>(std::nullopt, capacity_per_worker);
}
sliding_window_rate_limiter::sliding_window_rate_limiter(
    sliding_window_rate_limiter&&) noexcept = default;
sliding_window_rate_limiter& sliding_window_rate_limiter::operator=(
    sliding_window_rate_limiter&&) noexcept = default;
sliding_window_rate_limiter::~sliding_window_rate_limiter() = default;

bool sliding_window_rate_limiter::has_default_rule() const noexcept {
    return impl_ != nullptr && impl_->limiter_.has_default_rule();
}

std::size_t sliding_window_rate_limiter::capacity() const noexcept {
    return impl_ != nullptr ? impl_->limiter_.capacity() : 0;
}

rate_limit_decision sliding_window_rate_limiter::decide(
    std::string_view key, std::chrono::steady_clock::time_point now) noexcept {
    if (impl_ == nullptr) {
        return rate_limit_decision{};
    }
    return impl_->limiter_.decide_default(key, now);
}

rate_limit_decision sliding_window_rate_limiter::decide(
    const ip_address& address, std::chrono::steady_clock::time_point now) noexcept {
    char key_buffer[rate_limit_key_buffer_bytes];
    return decide(detail::rate_limit_key_for(address, key_buffer), now);
}

rate_limit_decision sliding_window_rate_limiter::decide(std::string_view key,
    const rate_limit_rule& rule, std::uintptr_t scope,
    std::chrono::steady_clock::time_point now) noexcept {
    if (impl_ == nullptr || rule.max_requests_ == 0 || rule.window_.count() <= 0) {
        return rate_limit_decision{};
    }
    return impl_->limiter_.decide_rule({.route_ = scope == 0 ? 2U : scope, .rule_ = 1U}, key, rule, now);
}

rate_limit_decision sliding_window_rate_limiter::decide(const ip_address& address,
    const rate_limit_rule& rule, std::uintptr_t scope,
    std::chrono::steady_clock::time_point now) noexcept {
    char key_buffer[rate_limit_key_buffer_bytes];
    return decide(detail::rate_limit_key_for(address, key_buffer), rule, scope, now);
}

}  // namespace ruvia

namespace ruvia::detail {
}  // namespace ruvia::detail

// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index. A request never walks or stats the tree: a
// file-backed response opens its one indexed entry on the blocking pool and
// checks the handle against the indexed identity.

namespace ruvia {
namespace {

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool has_hidden_path_segment(std::string_view relative_generic) noexcept {
    return relative_generic.starts_with('.') || (relative_generic.find("/.") != std::string_view::npos);
}

[[nodiscard]] detail::static_root_state* make_static_root_state(detail::static_root_config_storage config) {
    return new detail::static_root_state(std::move(config));
}

[[nodiscard]] std::filesystem::path canonical_static_root_path(const std::filesystem::path& root) {
    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root, ec)) {
        throw std::invalid_argument("static file root not found");
    }
    return canonical_root;
}

[[nodiscard]] const detail::static_root_entry* find_static_root_entry(
    const std::vector<detail::static_root_entry>& entries,
    std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(entries, relative_path,
        [](const detail::static_root_entry& entry_value) noexcept {
            return std::string_view(entry_value.relative_path_);
        });
}

[[nodiscard]] bool contains_static_directory(
    const std::vector<std::string>& directories, std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(directories, relative_path,
               [](const std::string& directory) noexcept { return std::string_view(directory); }) != nullptr;
}

[[nodiscard]] detail::static_file_version static_root_entry_version(
    const detail::static_root_entry& entry_value) noexcept {
    return {entry_value.modified_seconds_, entry_value.modified_token_};
}

[[nodiscard]] bool same_static_root_entry_metadata(
    const detail::static_root_entry& left, const detail::static_root_entry& right) noexcept {
    return left.relative_path_ == right.relative_path_ && left.file_path_ == right.file_path_ &&
           left.content_type_ == right.content_type_ && left.size_ == right.size_ &&
           left.identity_ == right.identity_ && left.modified_token_ == right.modified_token_ &&
           left.modified_seconds_ == right.modified_seconds_ && left.etag_ == right.etag_ &&
           left.last_modified_ == right.last_modified_ &&
           left.directly_servable_ == right.directly_servable_;
}

[[nodiscard]] const detail::static_root_entry* find_fresh_sidecar_entry(
    const detail::static_root_state& state_value, const detail::static_root_entry& identity,
    std::string_view suffix) {
    if (!state_value.has_sidecars_) {
        return nullptr;
    }
    const detail::inline_text_concat<256> lookup_path(
        {identity.relative_path_, suffix});
    const auto* variant = find_static_root_entry(state_value.entries_, lookup_path.view());
    if (variant == nullptr ||
        !static_root_entry_version(*variant).at_least_as_new_as(static_root_entry_version(identity))) {
        return nullptr;
    }
    return variant;
}

[[nodiscard]] const detail::static_root_memory_variant* find_memory_variant(
    const detail::static_root_entry& entry_value, http_content_coding coding) noexcept {
    for (const auto& variant : entry_value.memory_variants_) {
        if (variant.content_coding_ == coding) {
            return &variant;
        }
    }
    return nullptr;
}

// Reuses an unchanged file's encoded bytes from the previous snapshot. The
// buffer is immutable, so the new snapshot shares it instead of copying it.
void share_memory_variant(detail::static_root_entry& entry_value,
    const detail::static_root_memory_variant& source_value) {
    auto& stored = entry_value.memory_variants_.emplace_back();
    stored.content_coding_ = source_value.content_coding_;
    stored.bytes_ = source_value.bytes_;
    stored.modified_token_ = source_value.modified_token_;
    stored.modified_seconds_ = source_value.modified_seconds_;
    stored.etag_ = source_value.etag_;
    stored.last_modified_ = source_value.last_modified_;
}

// The path an indexed file is opened by: on POSIX the part beneath the
// retained root handle (every entry path is the canonical root joined with
// its relative path, so it starts right after the root and its separator),
// on Windows the canonical path itself.
[[nodiscard]] const http_native_path_char_type* static_root_open_path(
    [[maybe_unused]] const detail::static_root_state& state_value, const http_native_path_char_type* file_path) noexcept {
#if defined(__unix__)
    const auto& root_path = state_value.root_;
    return file_path + root_path.size() + (root_path.ends_with('/') ? 0 : 1);
#else
    return file_path;
#endif
}

// Opens an indexed file the way every use does (static_root_entry_open is the
// per-request form). On POSIX it is opened beneath the retained root handle
// without following a symlink in any component, so a path swapped after
// indexing cannot reach outside the root. Windows opens the canonical path;
// the identity check against the index is its guard.
[[nodiscard]] detail::native_file_descriptor open_static_root_file(const detail::static_root_state& state_value,
    const http_native_path_char_type* file_path, std::error_code& ec) noexcept {
#if defined(__unix__)
    return detail::open_native_file_beneath(
        state_value.root_directory_->get(), static_root_open_path(state_value, file_path), ec);
#else
    return detail::open_native_file(static_root_open_path(state_value, file_path), ec);
#endif
}

// A file that changed (or vanished) since it was indexed yields nothing: only
// that entry loses its variants and stays file-backed, validated per request.
[[nodiscard]] std::optional<std::string> read_static_root_entry_bytes(const detail::static_root_state& state_value,
    const detail::static_root_entry& entry_value) {
    if (entry_value.size_ > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("static file is too large to buffer");
    }
    const auto size = static_cast<std::size_t>(entry_value.size_);
    // Read and revalidate through one handle: the kept bytes are exactly those
    // of the indexed snapshot, with no path re-resolution in between.
    std::error_code ec;
    const auto input = open_static_root_file(state_value, entry_value.file_path_.c_str(), ec);
    if (ec || detail::validate_native_file(input.get(), entry_value.identity_, entry_value.size_)) {
        return std::nullopt;
    }
    std::string bytes;
    bytes.resize(size);
    if ((size != 0 && detail::read_native_file_at(input.get(), 0, bytes.data(), size)) ||
        detail::validate_native_file(input.get(), entry_value.identity_, entry_value.size_)) {
        return std::nullopt;
    }
    return bytes;
}

void store_precompressed_variant(detail::static_root_entry& entry_value, http_content_coding coding,
    std::string encoded, bool emit_response_validators) {
    auto bytes = std::make_shared<std::string>(std::move(encoded));
    auto& variant = entry_value.memory_variants_.emplace_back();
    variant.content_coding_ = coding;
    variant.modified_token_ = entry_value.modified_token_;
    variant.modified_seconds_ = entry_value.modified_seconds_;
    variant.bytes_ = std::move(bytes);
    if (emit_response_validators) {
        variant.etag_ = detail::make_static_file_snapshot_etag(variant.bytes_->size(), entry_value.modified_token_, entry_value.identity_, coding);
        variant.last_modified_ = entry_value.last_modified_;
    }
}

template <typename t_type>
void hash_value(std::uint64_t& hash, t_type value) noexcept {
    static_assert(std::is_integral_v<t_type>);
    hash = fnv1a_accumulate_integer(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t static_root_fingerprint(const detail::static_root_state& state_value) noexcept {
    std::uint64_t hash = fnv1a_offset_basis;
#if defined(__unix__)
    // A root directory replaced by another one with an identical index is
    // still a different snapshot: requests open entries relative to it.
    hash_value(hash, state_value.root_identity_.device_);
    hash_value(hash, state_value.root_identity_.inode_);
#endif
    hash_value(hash, state_value.entries_.size());
    for (const auto& entry : state_value.entries_) {
        hash = fnv1a_accumulate(hash, entry.relative_path_);
        hash_value(hash, entry.size_);
        hash_value(hash, entry.modified_token_);
        hash_value(hash, entry.modified_seconds_);
        hash_value(hash, entry.identity_.requires_validation());
        for (const auto word : entry.identity_.words()) {
            hash_value(hash, word);
        }
        hash_value(hash, entry.directly_servable_);
    }
    return hash;
}

}  // namespace

detail::static_root_config_storage detail::static_root_access::copy_config(
    const static_root& root) {
    return static_root_config_storage(root.state_->config_);
}

std::string_view detail::static_root_access::index_file(const static_root& root) noexcept {
    return root.state_->config_.index_file_;
}

bool detail::static_root_access::has_directory_index(const static_root& root) noexcept {
    return !root.state_->config_.index_file_.empty();
}

bool detail::static_root_access::has_sidecars(const static_root& root) noexcept {
    return root.state_->has_sidecars_;
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find(
    const static_root& root, std::string_view relative_path) noexcept {
    auto entry_value = find_variant(root, relative_path);
    if (!entry_value.has_value() || !entry_value->directly_servable_) {
        return std::nullopt;
    }
    return entry_value;
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find_variant(
    const static_root& root, std::string_view relative_path) noexcept {
    const auto& state_value = *root.state_;
    const auto& entries = state_value.entries_;
    const auto* const entry_value = find_static_root_entry(entries, relative_path);
    if (entry_value == nullptr) {
        return std::nullopt;
    }
    return detail::static_root_entry_view(entry_value->file_path_.c_str(), entry_value->content_type_,
        state_value.config_.cache_control_, entry_value->etag_, entry_value->last_modified_, entry_value->size_, entry_value->identity_,
        entry_value->modified_token_, entry_value->modified_seconds_, state_value.config_.range_requests_,
        state_value.config_.response_validators_, entry_value->directly_servable_, &entry_value->memory_variants_,
        entry_value->resident_bytes_.get());
}

bool detail::static_root_access::is_indexed_directory(
    const static_root& root, std::string_view relative_path) noexcept {
    if (!has_directory_index(root)) {
        return false;
    }
    return contains_static_directory(root.state_->directories_, relative_path);
}

namespace {

[[nodiscard]] std::optional<detail::static_root_entry_view> resolve_normalized_static_root_entry(
    const static_root& root, bool directory_slash, std::string_view& relative, std::string& storage) {
    auto entry_value =
        directory_slash ? std::optional<detail::static_root_entry_view>{} : detail::static_root_access::find(root, relative);
    if (!entry_value.has_value() && detail::static_root_access::is_indexed_directory(root, relative)) {
        // A normalized path never ends with '/'.
        const auto index = detail::static_root_access::index_file(root);
        // reserve() may invalidate a normalized path borrowed from storage.
        const bool copy_relative = relative.data() != storage.data();
        storage.reserve(relative.size() + (relative.empty() ? 0U : 1U) + index.size());
        if (copy_relative) {
            storage.assign(relative.data(), relative.size());
        }
        if (!storage.empty()) {
            storage.push_back('/');
        }
        storage.append(index.data(), index.size());
        relative = storage;
        entry_value = detail::static_root_access::find(root, relative);
    }
    return entry_value;
}

}  // namespace

std::optional<detail::static_root_entry_view> detail::static_root_access::resolve(
    const static_root& root, std::string_view& relative, std::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    relative = normalize_static_relative_path(relative, storage);
    if (relative.empty() && !has_directory_index(root)) {
        throw_invalid_static_file_path();
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::optional<detail::static_root_entry_view> detail::static_root_access::try_resolve(
    const static_root& root, std::string_view& relative, std::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    const auto normalized = try_normalize_static_relative_path(relative, storage);
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    relative = *normalized;
    if (relative.empty() && !has_directory_index(root)) {
        return std::nullopt;
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::uint64_t detail::static_root_access::fingerprint(const static_root& root) noexcept {
    return root.state_->fingerprint_;
}

void detail::static_root_access::install_precompressed_variants(
    static_root& root, const static_root* previous, const static_root_precompression_options& options) {
    if (!options.enabled()) {
        return;
    }
    if (options.min_bytes_ == 0 || options.max_bytes_ < options.min_bytes_) {
        throw std::invalid_argument("invalid static root precompression options");
    }

    auto& state_value = *root.state_;
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    const bool emit_response_validators =
        state_value.config_.response_validators_ == static_response_validator_policy::emit;

    for (auto& entry : state_value.entries_) {
        if (!entry.directly_servable_ || entry.size_ < options.min_bytes_ ||
            entry.size_ > options.max_bytes_ ||
            !is_http_media_type_compressible(entry.content_type_)) {
            continue;
        }

        const detail::static_root_entry* previous_entry = nullptr;
        if (previous_state != nullptr) {
            previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && !same_static_root_entry_metadata(entry, *previous_entry)) {
                previous_entry = nullptr;
            }
        }

        std::optional<std::string> plain;
        const std::string* plain_bytes = entry.resident_bytes_.get();
        for (const auto& sidecar : detail::static_precompression_sidecars) {
            const auto coding = sidecar.content_coding_;
            if (!options.enabled(coding) || find_fresh_sidecar_entry(state_value, entry, sidecar.suffix_) != nullptr) {
                continue;
            }
            if (previous_entry != nullptr) {
                if (const auto* previous_variant = find_memory_variant(*previous_entry, coding);
                    previous_variant != nullptr) {
                    share_memory_variant(entry, *previous_variant);
                    continue;
                }
            }
            if (plain_bytes == nullptr) {
                plain = read_static_root_entry_bytes(state_value, entry);
                if (!plain.has_value()) {
                    entry.memory_variants_.clear();
                    break;
                }
                plain_bytes = &*plain;
            }
            if (plain_bytes->empty()) {
                continue;
            }
            auto encoded = encode_http_content(
                coding, *plain_bytes, {.max_encoded_bytes_ = plain_bytes->size() - 1});
            if (encoded.has_value()) {
                store_precompressed_variant(entry, coding, std::move(*encoded),
                    emit_response_validators);
            }
        }
    }
}

void detail::static_root_access::install_resident_bodies(
    static_root& root, const static_root* previous, const static_root_resident_options& options) {
    if (!options.enabled()) {
        return;
    }
    auto& state_value = *root.state_;
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    std::size_t used = 0;
    for (auto& entry : state_value.entries_) {
        if (entry.size_ > options.max_file_bytes_ || entry.size_ > options.budget_bytes_ - used) {
            continue;
        }
        const auto size = static_cast<std::size_t>(entry.size_);
        if (previous_state != nullptr) {
            const auto* previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && previous_entry->resident_bytes_ != nullptr &&
                same_static_root_entry_metadata(entry, *previous_entry)) {
                entry.resident_bytes_ = previous_entry->resident_bytes_;
                used += size;
                continue;
            }
        }
        auto bytes = read_static_root_entry_bytes(state_value, entry);
        if (!bytes.has_value()) {
            continue;
        }
        entry.resident_bytes_ = std::make_shared<std::string>(std::move(*bytes));
        used += size;
    }
}

detail::static_root_entry_open detail::static_root_access::entry_open(
    const static_root& root, const static_root_entry_view& entry, bool checked) {
    const auto& state_value = *root.state_;
    http_native_path_string_type open_path(static_root_open_path(state_value, entry.file_path()));
#if defined(__unix__)
    return static_root_entry_open(state_value.root_directory_, std::move(open_path), entry.identity(), entry.size(), checked);
#else
    return static_root_entry_open(std::move(open_path), entry.identity(), entry.size(), checked);
#endif
}

http_response_file_descriptor detail::static_root_entry_open::operator()() const noexcept {
    std::error_code ec;
#if defined(__unix__)
    auto input = detail::open_native_file_beneath(root_directory_->get(), open_path_.c_str(), ec);
#else
    auto input = detail::open_native_file(open_path_.c_str(), ec);
#endif
    if (ec || (checked_ && detail::validate_native_file(input.get(), identity_, size_))) {
        return {};
    }
    return adopt_response_file_handle(std::move(input));
}

bool detail::static_root_access::same_snapshot(
    const static_root& left, const static_root& right) noexcept {
    const auto& lhs = *left.state_;
    const auto& rhs = *right.state_;
    if (lhs.root_ != rhs.root_ || lhs.directories_ != rhs.directories_ ||
        lhs.entries_.size() != rhs.entries_.size()) {
        return false;
    }
#if defined(__unix__)
    if (lhs.root_identity_ != rhs.root_identity_) {
        return false;
    }
#endif
    if (lhs.config_ != rhs.config_) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.entries_.size(); ++i) {
        if (!same_static_root_entry_metadata(lhs.entries_[i], rhs.entries_[i])) {
            return false;
        }
    }
    return true;
}

namespace {

[[nodiscard]] std::unique_ptr<detail::static_root_state>
build_static_root_state(const std::filesystem::path& root, detail::static_root_config_storage config_storage) {
    const auto canonical_root = canonical_static_root_path(root);
    std::unique_ptr<detail::static_root_state> state(make_static_root_state(std::move(config_storage)));
    std::error_code ec;
    auto& state_value = *state;
    http_assign_native_path(state_value.root_, canonical_root);
#if defined(__unix__)
    {
        auto root_directory = detail::open_native_directory(state_value.root_.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("open static file root", canonical_root, ec);
        }
        state_value.root_identity_ = detail::native_directory_handle_identity(root_directory.get(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root", canonical_root, ec);
        }
        state_value.root_directory_ = std::make_shared<detail::native_file_descriptor>(std::move(root_directory));
    }
#endif

    const auto& config = state_value.config_;
    const auto serve_dotfiles = detail::static_root_serves_dotfiles(config.dotfiles_);
    if (!config.index_file_.empty()) {
        state_value.directories_.push_back({});
    }

    // Index construction is transactional. A directory may change while it is
    // being walked, but publishing a partial snapshot would turn one transient
    // filesystem error into arbitrary 404s. Let the caller keep the previous
    // root during polling, and fail startup when there is no previous snapshot.
    std::filesystem::recursive_directory_iterator iter(canonical_root, ec);
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iter != end; iter.increment(ec)) {
        const auto& file_path = iter->path();
        ec.clear();
        const auto status = iter->symlink_status(ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root entry", file_path, ec);
        }
        if (std::filesystem::is_symlink(status)) {
            continue;
        }
        auto relative = detail::static_file_utf8_string(
            file_path.lexically_relative(canonical_root));
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        // Default-deny hidden paths: skip dotfiles and do not descend into
        // dot-directories (.git, .ssh, ...) so their contents are never indexed.
        if (!serve_dotfiles && has_hidden_path_segment(relative)) {
            if (std::filesystem::is_directory(status)) {
                iter.disable_recursion_pending();
            }
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!config.index_file_.empty()) {
                state_value.directories_.push_back(std::move(relative));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto extension = detail::lower_static_file_extension_from_utf8(relative);
        const bool is_sidecar_extension = detail::is_precompressed_sidecar_extension(extension);
        const bool directly_servable = detail::file_type_allowed(extension, config);
        bool usable_as_sidecar = false;
        if (!directly_servable && is_sidecar_extension) {
            const auto stem_view =
                std::string_view(relative).substr(0, relative.size() - extension.size());
            usable_as_sidecar = detail::file_type_allowed(
                detail::lower_static_file_extension_from_utf8(stem_view), config);
        }
        if (!directly_servable && !usable_as_sidecar) {
            continue;
        }
        // The entry is opened beneath the retained root, the way a request
        // opens it: a directory swapped for a symlink after it was listed
        // fails the snapshot instead of indexing a file outside the root.
        http_native_path_string_type native_path;
        http_assign_native_path(native_path, file_path);
        ec.clear();
        const auto snapshot = [&] {
            const auto input = open_static_root_file(state_value, native_path.c_str(), ec);
            return ec ? detail::response_file_snapshot{} : detail::snapshot_native_file_handle(input.get(), ec);
        }();
        if (ec) {
            throw std::filesystem::filesystem_error(
                "snapshot static file root entry", file_path, ec);
        }
        const auto emit_response_validators =
            config.response_validators_ == static_response_validator_policy::emit;
        detail::static_root_entry entry;
        entry.relative_path_ = std::move(relative);
        entry.file_path_ = std::move(native_path);
        entry.content_type_ = detail::content_type_for(extension, config);
        entry.size_ = snapshot.size_;
        entry.identity_ = snapshot.identity_;
        entry.modified_token_ = snapshot.modified_token_;
        entry.modified_seconds_ = snapshot.modified_seconds_;
        entry.directly_servable_ = directly_servable;
        if (emit_response_validators) {
            entry.etag_ = detail::make_static_file_snapshot_etag(
                snapshot.size_, snapshot.modified_token_, snapshot.identity_);
            if (const auto date = format_http_date(snapshot.modified_seconds_)) {
                entry.last_modified_.assign(date->data(), date->size());
            }
        }
        if (state_value.entries_.empty()) {
            state_value.entries_.reserve(8);
        }
        if (is_sidecar_extension) {
            state_value.has_sidecars_ = true;
        }
        state_value.entries_.push_back(std::move(entry));
    }
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    std::ranges::sort(state_value.entries_,
        [](const detail::static_root_entry& left, const detail::static_root_entry& right) {
            return left.relative_path_ < right.relative_path_;
        });
    std::ranges::sort(state_value.directories_);
    state_value.directories_.erase(
        std::ranges::unique(state_value.directories_).begin(), state_value.directories_.end());
#if defined(__unix__)
    // The walk resolved the root by path. Publish only when that path still
    // names the retained directory, so requests open entries in the indexed
    // directory rather than in one swapped in while the index was built.
    if (detail::native_directory_path_identity(state_value.root_.c_str(), ec) != state_value.root_identity_ || ec) {
        throw std::filesystem::filesystem_error("static file root changed while it was indexed", canonical_root,
            ec ? ec : std::make_error_code(std::errc::state_not_recoverable));
    }
#endif
    state_value.fingerprint_ = static_root_fingerprint(state_value);
    return state;
}

}  // namespace

std::unique_ptr<static_root> detail::static_root_access::make(
    const std::filesystem::path& root, const static_root_config_storage& config) {
    return make(root, detail::static_root_config_storage(config));
}

std::unique_ptr<static_root> detail::static_root_access::make(
    const std::filesystem::path& root, static_root_config_storage&& config) {
    auto state = build_static_root_state(root, std::move(config));
    return std::unique_ptr<static_root>(new static_root(
        std::unique_ptr<detail::static_root_state, static_root::state_deleter_type>(state.release())));
}

static_root::static_root(std::unique_ptr<detail::static_root_state, state_deleter_type> state) noexcept
    : state_(std::move(state)) {}

static_root::static_root(const std::filesystem::path& root, static_root_options options)
    : state_([&] {
          detail::validate_static_root_options(options);
          auto built = build_static_root_state(
              root, detail::store_validated_static_root_config(options));
          return std::unique_ptr<detail::static_root_state, state_deleter_type>(built.release());
      }()) {
    detail::static_root_access::install_resident_bodies(*this, nullptr,
        {.max_file_bytes_ = options.resident_max_bytes_.value_or(0),
            .budget_bytes_ = options.resident_budget_bytes_.value_or(0)});
}

static_root::~static_root() = default;

void static_root::state_deleter_type::operator()(detail::static_root_state* state_value) const noexcept {
    delete state_value;
}

std::filesystem::path static_root::path() const {
    return http_make_path_from_native_path(state_->root_);
}

namespace detail {
namespace {

void lowercase_ascii(std::string& value) noexcept {
    for (char& character : value) {
        character = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(character)));
    }
}

inline constexpr std::string_view default_static_file_types[] = {
    "apng",
    "avif",
    "bmp",
    "css",
    "cur",
    "eot",
    "gif",
    "htm",
    "html",
    "ico",
    "jpeg",
    "jpg",
    "js",
    "json",
    "map",
    "mjs",
    "otf",
    "png",
    "svg",
    "ttf",
    "txt",
    "wasm",
    "webmanifest",
    "webp",
    "woff",
    "woff2",
    "xml",
    "xsl",
};

const static_root_mime_type_storage* find_static_mime_type(
    const std::vector<static_root_mime_type_storage>& mime_types,
    std::string_view extension) noexcept {
    return find_static_index_entry(mime_types, extension,
        [](const static_root_mime_type_storage& mime) noexcept {
            return std::string_view(mime.extension_);
        });
}

}  // namespace

static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value) {
    static_root_config_storage result;
    result.cache_control_ = source_value.cache_control_.view();
    result.index_file_ = source_value.index_file_.view();
    result.default_content_type_ = source_value.default_content_type_.view();
    result.mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = result.mime_types_.emplace_back();
        const auto ext = mime.extension_.view();
        const bool has_dot = ext.starts_with('.');
        stored.extension_.reserve(ext.size() + (has_dot ? 0U : 1U));
        if (!has_dot) {
            stored.extension_.push_back('.');
        }
        stored.extension_.append(ext);
        lowercase_ascii(stored.extension_);
        stored.content_type_ = mime.content_type_.view();
    }
    std::ranges::sort(result.mime_types_,
        [](const static_root_mime_type_storage& left, const static_root_mime_type_storage& right) {
            return left.extension_ < right.extension_;
        });

    result.file_type_kind_ = source_value.file_types_.kind_;
    if (result.file_type_kind_ == static_file_type_policy::kind::only) {
        result.file_type_extensions_.reserve(source_value.file_types_.extensions_.size());
        for (std::string_view extension : source_value.file_types_.extensions_) {
            if (extension.starts_with('.')) {
                extension.remove_prefix(1);
            }
            auto& stored = result.file_type_extensions_.emplace_back(extension);
            lowercase_ascii(stored);
        }
        std::ranges::sort(result.file_type_extensions_);
        result.file_type_extensions_.erase(std::ranges::unique(result.file_type_extensions_).begin(),
            result.file_type_extensions_.end());
    }
    result.range_requests_ = source_value.range_requests_;
    result.response_validators_ = source_value.response_validators_;
    result.dotfiles_ = source_value.dotfiles_;
    return result;
}

bool is_valid_static_file_extension(std::string_view extension) noexcept {
    if (extension.empty() || (extension.find('/') != std::string_view::npos) ||
        (extension.find('\\') != std::string_view::npos)) {
        return false;
    }
    if (extension == "." || extension == "..") {
        return false;
    }
    if (extension.front() == '.') {
        extension.remove_prefix(1);
    }
    return !extension.empty();
}

bool file_type_allowed(std::string_view extension, const static_root_config_storage& config) {
    if (config.file_type_kind_ == static_file_type_policy::kind::all) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (config.file_type_kind_ == static_file_type_policy::kind::defaults) {
        return std::ranges::binary_search(default_static_file_types, value);
    }
    return std::ranges::binary_search(config.file_type_extensions_, value);
}

std::string content_type_for(std::string_view extension,
    const static_root_config_storage& config) {
    if (const auto* const mime = find_static_mime_type(config.mime_types_, extension); mime != nullptr) {
        return std::string(mime->content_type_);
    }

    const auto guessed = guess_static_file_content_type_from_extension(extension);
    if (guessed != std::string_view("application/octet-stream") ||
        config.default_content_type_.empty()) {
        return std::string(guessed);
    }
    return std::string(config.default_content_type_);
}

}  // namespace detail

std::optional<static_file_representation> select_static_file_representation(const static_root& root,
    std::string_view relative, const http_response_coding_selection& selection, detail::static_root_entry_view identity) {
    if (!selection.acceptable()) {
        return std::nullopt;
    }
    const bool has_sidecars = detail::static_root_access::has_sidecars(root);
    if ((!has_sidecars && !identity.has_memory_variants()) ||
        (!selection.accepts(http_content_coding::brotli) &&
            !selection.accepts(http_content_coding::zstd) &&
            !selection.accepts(http_content_coding::gzip))) {
        if (selection.identity_accepted()) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        return std::nullopt;
    }

    struct candidate final {
        http_content_coding content_coding_;
        std::optional<detail::static_root_entry_view> entry_;
        std::optional<detail::static_root_memory_variant_view> memory_variant_;
    };
    std::array<candidate, detail::static_precompression_sidecars.size()> candidates{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        candidates[i].content_coding_ = detail::static_precompression_sidecars[i].content_coding_;
    }

    auto available = http_response_coding_candidates::identity_only();
    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_path;
    const bool use_inline_path = has_sidecars && relative.size() + 4U <= inline_path.size();
    if (use_inline_path && !relative.empty()) {
        std::memcpy(inline_path.data(), relative.data(), relative.size());
    }
    std::string variant_path;
    const detail::static_file_version identity_version{
        identity.modified_seconds(), identity.modified_token()};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& candidate = candidates[i];
        if (!selection.accepts(candidate.content_coding_)) {
            continue;
        }
        if (has_sidecars) {
            const auto suffix = detail::static_precompression_sidecars[i].suffix_;
            const std::size_t total_size = relative.size() + suffix.size();
            std::string_view lookup_path;
            if (use_inline_path) {
                std::memcpy(inline_path.data() + relative.size(), suffix.data(), suffix.size());
                lookup_path = std::string_view(inline_path.data(), total_size);
            } else {
                variant_path.reserve(total_size);
                variant_path.assign(relative.data(), relative.size());
                variant_path.append(suffix.data(), suffix.size());
                lookup_path = variant_path;
            }
            if (const auto entry_value = detail::static_root_access::find_variant(root, lookup_path);
                entry_value.has_value() &&
                detail::static_file_version{entry_value->modified_seconds(), entry_value->modified_token()}
                    .at_least_as_new_as(identity_version)) {
                candidate.entry_ = *entry_value;
                available.include(candidate.content_coding_);
                continue;
            }
        }
        if (auto memory_variant = identity.memory_variant(candidate.content_coding_);
            memory_variant.has_value()) {
            candidate.memory_variant_ = *memory_variant;
            available.include(candidate.content_coding_);
        }
    }

    if (const auto chosen = selection.select(available); chosen.acceptable()) {
        if (chosen.coding() == http_content_coding::identity) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        for (const auto& candidate : candidates) {
            if (candidate.content_coding_ == chosen.coding() && candidate.entry_.has_value()) {
                return static_file_representation(*candidate.entry_, candidate.content_coding_);
            }
            if (candidate.content_coding_ == chosen.coding() &&
                candidate.memory_variant_.has_value()) {
                return static_file_representation(
                    identity, *candidate.memory_variant_, candidate.content_coding_);
            }
        }
    }

    return std::nullopt;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] http_error_info normalize_http_error_info(http_error_info error) noexcept {
    auto status = error.status();
    if (!status.is_error()) {
        status = http_status::internal_server_error;
    }
    auto status_text = error.status_text();
    if (status_text.empty() || !is_valid_http_header_value(status_text)) {
        status_text = http_reason_phrase(status);
        if (status_text.empty()) {
            // Application error presentation remains a Web concern. Do not
            // invent an HTTP/1 reason phrase for an extension status code.
            status_text = "HTTP Error";
        }
    }
    auto code = error.code();
    if (code.empty()) {
        code = default_error_code(status);
    }
    auto message = error.message();
    if (message.empty()) {
        message = status_text;
    }
    return http_error_info({.status_ = status,
        .code_ = code,
        .message_ = message,
        .status_text_ = status_text,
        .validation_issues_ = error.validation_issues()});
}

[[nodiscard]] std::string serialize_error_response(http_error_info error) {
    const auto phrase = http_reason_phrase(error.status());
    const auto title = phrase.empty() ? std::string_view("HTTP Error") : phrase;
    const auto issues = error.validation_issues();
    std::size_t reserve_size = 66U + detail::json_string_size_hint(title) +
                               detail::json_string_size_hint(error.message()) +
                               detail::json_string_size_hint(error.code());
    for (const auto& issue : issues) {
        reserve_size += 30U + detail::json_string_size_hint(issue.field()) +
                        detail::json_string_size_hint(issue.code()) +
                        detail::json_string_size_hint(issue.message());
    }
    std::string out;
    out.reserve(reserve_size);
    out.append("{\"type\":\"about:blank\",\"title\":");
    detail::append_json_string(out, title);
    out.append(",\"status\":");
    http_append_unsigned(out, error.status().value());
    out.append(",\"detail\":");
    detail::append_json_string(out, error.message());
    out.append(",\"code\":");
    detail::append_json_string(out, error.code());
    if (!issues.empty()) {
        out.append(",\"errors\":[");
        for (std::size_t i = 0; i < issues.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out.append("{\"field\":");
            detail::append_json_string(out, issues[i].field());
            out.append(",\"code\":");
            detail::append_json_string(out, issues[i].code());
            out.append(",\"message\":");
            detail::append_json_string(out, issues[i].message());
            out.push_back('}');
        }
        out.push_back(']');
    }
    out.push_back('}');
    return out;
}

// `error` is already normalized.
[[nodiscard]] http_response make_normalized_error_response(http_error_info error) {
    http_response response;
    response.status(error.status());
    response.header_stable_view("Content-Type", "application/problem+json");
    response.owned_body(serialize_error_response(error));
    return response;
}

}  // namespace

void validation_issue::swap(validation_issue& other) noexcept {
    field_.swap(other.field_);
    code_.swap(other.code_);
    message_.swap(other.message_);
}

validation_issue& validation_issue::operator=(const validation_issue& other) {
    if (this != &other) {
        validation_issue candidate(other);
        swap(candidate);
    }
    return *this;
}

validation_issue& validation_issue::operator=(validation_issue&& other) noexcept {
    if (this != &other) {
        swap(other);
        other.field_.clear();
        other.code_.clear();
        other.message_.clear();
    }
    return *this;
}

http_error::http_error(http_error_info_options options)
    : status_(options.status_),
      status_text_(options.status_text_.view()),
      code_(options.code_.view()),
      message_(options.message_.view()),
      validation_issues_(options.validation_issues_.begin(), options.validation_issues_.end()) {}

http_error::http_error(http_status_code status, std::string_view code, std::string_view message,
    std::string_view status_text, std::vector<validation_issue>&& issues)
    : status_(status),
      status_text_(status_text),
      code_(code),
      message_(message),
      validation_issues_(std::move(issues)) {}

http_error& http_error::operator=(const http_error& other) {
    if (this != &other) {
        std::vector<validation_issue> copied_issues(other.validation_issues_);
        std::string copied_status_text(other.status_text_);
        std::string copied_code(other.code_);
        std::string copied_message(other.message_);
        validation_issues_.swap(copied_issues);
        status_text_.swap(copied_status_text);
        code_.swap(copied_code);
        message_.swap(copied_message);
        status_ = other.status_;
    }
    return *this;
}
const char* http_error::what() const noexcept {
    return message_.c_str();
}

http_error_info http_error::info() const& noexcept {
    return http_error_info({.status_ = status_,
        .code_ = code_,
        .message_ = message_,
        .status_text_ = status_text_,
        .validation_issues_ = validation_issues_});
}

std::string_view default_error_code(http_status_code status) noexcept {
    switch (status.value()) {
        case http_status::bad_request.value():
            return "bad_request";
        case http_status::unauthorized.value():
            return "unauthorized";
        case http_status::forbidden.value():
            return "forbidden";
        case http_status::not_found.value():
            return "not_found";
        case http_status::method_not_allowed.value():
            return "method_not_allowed";
        case http_status::conflict.value():
            return "conflict";
        case http_status::precondition_failed.value():
            return "precondition_failed";
        case http_status::content_too_large.value():
            return "content_too_large";
        case http_status::range_not_satisfiable.value():
            return "range_not_satisfiable";
        case http_status::expectation_failed.value():
            return "expectation_failed";
        case http_status::unprocessable_content.value():
            return "unprocessable_content";
        case http_status::too_many_requests.value():
            return "too_many_requests";
        case http_status::request_header_fields_too_large.value():
            return "request_header_fields_too_large";
        case http_status::internal_server_error.value():
            return "internal_error";
        case http_status::not_implemented.value():
            return "not_implemented";
        case http_status::service_unavailable.value():
            return "service_unavailable";
        case http_status::http_version_not_supported.value():
            return "http_version_not_supported";
        default:
            return status.is_server_error() ? "internal_error" : "bad_request";
    }
}

std::string exception_text(const std::exception_ptr& exception) {
    if (exception == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::system_error& error) {
        return system_error_text(error);
    } catch (const std::exception& error) {
        return std::string(error.what());
    } catch (...) {
        return std::string("non-standard exception");
    }
}

http_response detail::make_default_error_response(http_error_info error) {
    return make_normalized_error_response(normalize_http_error_info(error));
}

http_response detail::make_route_not_found_response() {
    return make_default_error_response(
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
}

// Running the application's error handler, or the default response when there
// is none. A handler that throws is answered with the default response too:
// transport output stays deterministic and no exception detail reaches the
// client.
task<http_response> detail::invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler) {
    error = normalize_http_error_info(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context_value, error);
        } catch (const http_error& nested) {
            co_return make_default_error_response(nested.info());
        } catch (...) {
            // The error handler itself threw: nothing above it observes the
            // failure, so report it server-side, keep transport output
            // deterministic, and avoid echoing exception detail to the client.
            ruvia::report_unhandled_failure("error handler", std::current_exception());
            co_return make_default_error_response(
                http_error_info({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "error_handler_failed",
                    .message_ = "error handler failed"}));
        }
    }

    co_return make_normalized_error_response(error);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// One synchronous representation pass: apply CORS (`cors`, when set) and the
// listener's Alt-Svc, and run the compression policy up to its offload
// boundary, which only a worker with a blocking pool can cross.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request,
    const http_response_coding_policy& policy, std::string_view alternative_service,
    const http_cors_policy* cors, http_response& response, const http_server_options& options,
    const worker_ref& worker) {
    if (cors != nullptr) {
        (void)cors->apply_response_headers(request, response);
    }
    apply_alternative_service(response, alternative_service);
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_,
                *options.codecs_, worker.blocking_pool() != nullptr);
        }
    }
    return {http_response_compression_status::not_applicable, false};
}

}  // namespace

http_response_preparation http_response_tail::prepare(
    http_response& response, const http_server_options& options, const worker_ref& worker) const {
    const auto& request = *request_;
    const auto first_pass = prepare_representation(
        request, coding_policy_, alternative_service_, cors(options), response, options, worker);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        const auto step = recovery.advance(policy, request, response, first_pass.status_);
        if (step.action_ == buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), false});
        }
        return http_response_preparation(policy, recovery, step);
    }
    return http_response_preparation(coding_policy_);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation,
    http_response& response, const http_server_options& options, const route_table& routes,
    const context_services& services) const {
    const auto& request = *request_;
    const auto& deferred = std::get<http_response_preparation::deferred_type>(preparation.state_);
    auto policy = deferred.policy_;
    auto recovery = deferred.recovery_;
    const auto* pending_step = deferred.pending_step_ ? &*deferred.pending_step_ : nullptr;
    http_buffered_compression_step pass{http_response_compression_status::not_applicable, true};
    for (;;) {
        buffered_response_recovery_step step;
        if (pending_step != nullptr) {
            step = *pending_step;
            pending_step = nullptr;
        } else {
            auto compression = pass.status_;
            if (pass.offload_) {
                compression = co_await compress_response_offloaded(
                    *policy.selection(), response, services.worker(), services.get_stop_token());
            }
            step = recovery.advance(policy, request, response, compression);
        }
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handle_error(request, *step.error_, services);
        }
        pass = prepare_representation(
            request, policy, alternative_service_, cors(options), response, options, services.worker());
    }
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] bool is_csrf_token_header(std::string_view name) noexcept {
    for (const std::string_view prefix : {"sec-", "proxy-", "access-control-request-", "x-forwarded-"}) {
        if (name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix)) {
            return false;
        }
    }
    for (const std::string_view reserved : {
             "accept", "accept-language", "content-language", "content-type", "range",
             "accept-charset", "accept-encoding", "connection", "content-length",
             "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
             "origin", "permissions-policy", "referer", "set-cookie", "te",
             "trailer", "transfer-encoding", "upgrade", "via",
             "user-agent", "priority", "save-data", "device-memory", "dpr", "width",
             "viewport-width", "downlink", "ect", "rtt", "forwarded", "x-real-ip",
             "x-requested-with"}) {
        if (http_ascii_equals_ignore_case(name, reserved)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const csrf_protection_config& csrf_protection::validate(const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    if (!is_csrf_token_header(source_value.header_name_)) {
        throw std::invalid_argument("CSRF token requires a script-supplied custom request field");
    }
    return source_value;
}

csrf_protection::csrf_protection()
    : csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config)
    : cookie_name_(validate(config).cookie_name_),
      header_name_(config.header_name_) {}

task<void> csrf_protection::invoke(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head ||
                      method == http_known_method::options;
    const auto cookie = c.req().cookie(cookie_name_);
    if (!safe) {
        const auto header_value = c.req().header(header_name_);
        if (!cookie || cookie->empty() || !header_value || header_value->empty() ||
            !detail::constant_time_bytes_equal(*cookie, *header_value)) {
            throw http_error({.status_ = ruvia::http_status::forbidden,
                .code_ = "csrf_token_mismatch",
                .message_ = "CSRF token missing or invalid"});
        }
    } else if (!cookie || cookie->empty()) {
        std::array<char, detail::secure_token_size> buffer;
        const auto token = detail::generate_secure_token(buffer);
        const auto connection = c.conn();
        const cookie_options options{
            .path_ = "/",
            .same_site_ = cookie_same_site::lax,
            .secure_ = connection.scheme() == http_scheme::https ? cookie_attribute_policy::emit
                                                                 : cookie_attribute_policy::omit,
        };
        c.policy().set_cookie({.name_ = cookie_name_, .value_ = token, .attributes_ = options});
    }
    co_await next_value();
}

}  // namespace ruvia

namespace ruvia {

namespace {

constexpr std::string_view content_security_policy_name = "Content-Security-Policy";
constexpr std::string_view referrer_policy_name = "Referrer-Policy";
constexpr std::string_view permissions_policy_name = "Permissions-Policy";

[[nodiscard]] bool emits_default_security_header(default_security_header_policy policy) {
    switch (policy) {
        case default_security_header_policy::emit_default:
            return true;
        case default_security_header_policy::omit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replaces_existing_security_header(security_header_conflict_policy policy) {
    switch (policy) {
        case security_header_conflict_policy::preserve_existing:
            return false;
        case security_header_conflict_policy::replace_existing:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware()
    : security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& source_value)
    : emit_content_type_options_(emits_default_security_header(source_value.content_type_options_header_)),
      emit_frame_options_(emits_default_security_header(source_value.frame_options_header_)),
      emit_strict_transport_security_(
          emits_default_security_header(source_value.strict_transport_security_header_)),
      emit_disabled_xss_protection_([&] {
          switch (source_value.xss_protection_header_) {
              case xss_protection_header_policy::emit_disabled:
                  return true;
              case xss_protection_header_policy::omit:
                  return false;
              default:
                  throw std::invalid_argument("X-XSS-Protection header policy is invalid");
          }
      }()),
      replace_existing_(replaces_existing_security_header(source_value.existing_headers_)) {
    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(source_value.content_security_policy_);
    validate_value(source_value.referrer_policy_);
    validate_value(source_value.permissions_policy_);
    for (const auto& header : source_value.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_);
    }
}

task<void> security_headers_middleware::invoke(context& context_value, next& next_value) {
    const bool secure_transport = context_value.conn().scheme() == http_scheme::https;
    const auto set_header = [&context_value, replace_existing = replace_existing_](
                                std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        detail::context_access::security_header(context_value, name, value, replace_existing);
    };

    if (emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    if (emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header(content_security_policy_name, content_security_policy_, true);
    set_header(referrer_policy_name, referrer_policy_, true);
    set_header(permissions_policy_name, permissions_policy_, true);

    for (const auto& header : custom_headers_) {
        if (!secure_transport &&
            http_ascii_equals_ignore_case(header.name(), "Strict-Transport-Security")) {
            continue;
        }
        set_header(header.name(), header.value(), false);
    }
    co_await next_value();
}

}  // namespace ruvia

namespace {

constexpr const char* response_stream_lane_in_progress = "response stream output operation is already in progress";

// What an output operation of a response stream borrows from its writer.
struct response_stream_output final {
    void* target_;
    bool& active_;
    ruvia::detail::operation_interrupt_source& interrupts_;
    const ruvia::worker_ref& worker_;
    ruvia::context* context_;
};

template <typename operation_type>
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation,
    const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(
        output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
    try {
        if constexpr (std::is_void_v<typename decltype(start())::value_type>) {
            co_await start();
            interrupt.throw_if_interrupted();
        } else {
            auto result = co_await start();
            interrupt.throw_if_interrupted();
            co_return result;
        }
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept
    : writer_(writer.operation_scope_.active() ? &writer : nullptr),
      registration_(writer.operation_scope_, this, &sse_writer::expire_capability) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability(void* target) noexcept {
    static_cast<sse_writer*>(target)->writer_ = nullptr;
}

http_tunnel& context::tunnel() const {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::websocket() const {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

response_stream_writer& context::stream() {
    const auto* output = response_output().response_stream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

sse_writer context::stream_sse() {
    auto& writer = stream();
    if (!writer.committed()) {
        set_stable_response_header("Content-Type", "text/event-stream");
        set_stable_response_header("Cache-Control", "no-cache");
    }
    return sse_writer(writer);
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

task<void> write_transferred_chunk(response_stream_output output, task<void> (*write)(void*, std::string_view),
    std::string chunk, operation_options operation) {
    co_await run_response_stream_output(output, std::move(operation), "response stream write",
        [&] { return write(output.target_, chunk); });
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>),
    std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end",
        [&] { return end(output.target_, views.view()); });
}

task<timer_sleep_result> sleep_stopped_by_either(void* target,
    task<timer_sleep_result> (*sleep)(void*, std::chrono::milliseconds, const stop_token&),
    std::chrono::milliseconds duration, stop_token context_stop, stop_token stop) {
    const stop_group either(context_stop, stop);
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::text(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit",
            [target = target_, commit = commit_] { return commit(target); }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return write(as_chars(chunk), std::move(operation));
}

task<void> response_stream_writer::write(std::string_view chunk, operation_options operation) & {
    require_active();
    return write_owned(std::string(chunk), std::move(operation));
}

task<void> response_stream_writer::write_owned(std::string&& chunk, operation_options operation) {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk({target_, output_active_, *interrupts_, *worker_, context_}, write_, std::move(chunk), std::move(operation)));
}

task<void> response_stream_writer::writeln(std::string_view chunk, operation_options operation) & {
    require_active();
    std::string owned;
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned), std::move(operation));
}

task<timer_sleep_result> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable()) {
        return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, sleep_stopped_by_either(target_, sleep_, duration.value(), stop_token_, std::move(stop)));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    require_output();
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    return writer().end(trailers, std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task(std::move(operation)));
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, "multipart body read is already in progress");
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "multipart body read");
    }
    const detail::operation_steps steps(std::move(operation));
    read_guard_type read_guard_value(*this);
    try {
        for (;;) {
            auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
            if (const auto* part = result_value.part()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.done() != nullptr) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.need_input() != nullptr) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
                if (!chunk) {
                    std::get<receiving_state_type>(state_).parser_.finish_input();
                } else {
                    std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
                }
                continue;
            }
            if (const auto* failure = result_value.failure()) {
                throw failure->protocol_error();
            }
            throw std::logic_error("unexpected multipart poll result");
        }
    } catch (const std::system_error& error) {
        // An interrupted body read fed the parser nothing; parsing resumes on
        // the next read.
        if (steps.interrupted(error)) {
            read_guard_value.commit();
        }
        throw;
    }
}

}  // namespace ruvia

namespace {
ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options),
    bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

using websocket_write_type = ruvia::task<void> (*)(void*, ruvia::websocket_opcode, std::string_view, bool,
    ruvia::detail::websocket_write_extent, ruvia::operation_options);

// The final fragment releases the data lane of its fragmented message.
ruvia::task<void> write_websocket_payload(void* target, websocket_write_type write, ruvia::websocket_opcode opcode,
    std::string payload_value, bool& write_active, bool compress, ruvia::detail::websocket_write_extent extent,
    ruvia::operation_options operation, bool& fragmented_message_active) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, extent, std::move(operation));
    if (extent == ruvia::detail::websocket_write_extent::final_fragment) {
        fragmented_message_active = false;
    }
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target, websocket_write_type write,
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease,
    ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, ruvia::detail::websocket_write_extent::message,
        std::move(operation));
}

ruvia::task<void> close_websocket_with_reason(void* target,
    ruvia::task<void> (*close)(void*, ruvia::websocket_close_options, ruvia::operation_options),
    ruvia::websocket_close_options options, std::string reason, ruvia::operation_options operation,
    bool& read_active, bool& write_active, const bool& message_send_active) {
    // The write lane also excludes another close.
    auto write_lease = ruvia::detail::claim_operation_lane(write_active, "websocket close cannot overlap an output operation");
    if (message_send_active) {
        throw std::logic_error("websocket close cannot overlap a message send");
    }
    // A pending read observes the peer Close itself; otherwise close reads,
    // and holds the read lane while it does.
    const ruvia::detail::operation_lane_lease read_lease(read_active);
    static_cast<void>(read_lease);
    options.reason_ = reason;
    co_await close(target, options, std::move(operation));
}

}  // namespace

namespace ruvia {
namespace {
detail::websocket_state& require_websocket(const std::shared_ptr<detail::websocket_state>& state) {
    if (!state) {
        throw std::logic_error("websocket owner was moved");
    }
    state->require_active();
    return *state;
}
void retire_websocket(std::shared_ptr<detail::websocket_state> state) noexcept {
    if (!state) {
        return;
    }
    if (state->owner_) {
        state->abort();
    }
    state->operation_scope_.expire();
    if (auto owner = std::exchange(state->owner_, nullptr)) {
        if (state->retire_) {
            state->retire_(owner.get());
        }
    }
}
}  // namespace
websocket::websocket(std::shared_ptr<detail::websocket_state> state) noexcept
    : state_(std::move(state)) {}
websocket::websocket(websocket&&) noexcept = default;
websocket& websocket::operator=(websocket&& other) noexcept {
    if (this != &other) {
        retire_websocket(std::exchange(state_, std::move(other.state_)));
    }
    return *this;
}
websocket::~websocket() {
    retire_websocket(std::move(state_));
}
task<std::optional<websocket_message>> websocket::read(operation_options operation) & {
    return require_websocket(state_).read(std::move(operation));
}
task<void> websocket::text(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).text(payload, options, std::move(operation));
}
task<void> websocket::binary(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::binary(std::span<const std::byte> payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::ping(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).ping(payload, std::move(operation));
}
task<void> websocket::pong(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).pong(payload, std::move(operation));
}
task<void> websocket::write_owned(websocket_opcode opcode, std::string&& payload, bool compress, operation_options operation) {
    return require_websocket(state_).write(opcode, std::move(payload), compress, std::move(operation));
}
task<void> websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).send(std::move(message), options, std::move(operation));
}
task<void> websocket::close(websocket_close_options options, operation_options operation) & {
    return require_websocket(state_).close(options, std::move(operation));
}
void websocket::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
websocket_message_writer websocket::begin_text(websocket_send_options options) & {
    return require_websocket(state_).begin_text(options);
}
websocket_message_writer websocket::begin_binary(websocket_send_options options) & {
    return require_websocket(state_).begin_binary(options);
}
std::string_view websocket::subprotocol() const& noexcept {
    return state_ ? state_->subprotocol() : std::string_view{};
}
bool websocket::connected() const noexcept {
    if (!state_) {
        return false;
    }
    if (!state_->operation_scope_.active()) {
        return false;
    }
    state_->require_active();
    return state_->protocol_(state_->target_).liveness_mode() == websocket_liveness_mode::open;
}
const worker_ref& websocket::worker() const& {
    return *require_websocket(state_).worker_;
}
std::optional<std::uint16_t> websocket::peer_close_code() const noexcept {
    return state_ ? state_->peer_close_code() : std::nullopt;
}
std::string_view websocket::peer_close_reason() const& noexcept {
    return state_ ? state_->peer_close_reason() : std::string_view{};
}
websocket_termination websocket::termination() const noexcept {
    return state_ ? state_->termination() : websocket_termination::none;
}
std::uint64_t websocket::bytes_received() const noexcept {
    return state_ ? state_->bytes_received() : 0;
}
std::uint64_t websocket::bytes_sent() const noexcept {
    return state_ ? state_->bytes_sent() : 0;
}
task<std::optional<websocket_message>> detail::websocket_state::read(operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (read_timeout_) {
        operation = override_operation_options({.timeout_ = read_timeout_->value()}, std::move(operation));
    }
    return make_scoped_operation(operation_scope_, read_websocket(target_, read_, read_active_, std::move(operation)));
}

task<void> detail::websocket_state::text(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::span<const std::byte> payload_value, websocket_send_options options, operation_options operation) & {
    return binary(as_chars(payload_value), options, std::move(operation));
}

task<void> detail::websocket_state::pong(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::pong, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::ping(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::ping, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    auto message_lease = detail::claim_operation_lane(message_send_active_, "websocket message send is already pending");
    return make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease),
            write_active_, options.compress_, std::move(operation)));
}

task<void> detail::websocket_state::close(websocket_close_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    std::string owned(options.reason_.view());
    return make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation),
            read_active_, write_active_, message_send_active_));
}

void detail::websocket_state::abort() & noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

std::optional<std::uint16_t> detail::websocket_state::peer_close_code() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return std::nullopt;
    }
    return peer_close_code_(target_);
}

std::string_view detail::websocket_state::peer_close_reason() const& noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return {};
    }
    return peer_close_reason_(target_);
}

websocket_termination detail::websocket_state::termination() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return websocket_termination::none;
    }
    return protocol_(target_).termination();
}

std::uint64_t detail::websocket_state::bytes_received() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_received();
}

std::uint64_t detail::websocket_state::bytes_sent() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_sent();
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    return write(opcode, std::string(payload_value), compress, std::move(operation), extent);
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string&& payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    if (extent == detail::websocket_write_extent::message && fragmented_message_active_ &&
        (opcode == websocket_opcode::text || opcode == websocket_opcode::binary)) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    operation = override_operation_options(defaults_, std::move(operation));
    return make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(payload_value), write_active_, compress, extent,
            std::move(operation), fragmented_message_active_));
}

websocket_message_writer detail::websocket_state::begin_text(websocket_send_options options) & {
    return begin_message(websocket_opcode::text, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_binary(websocket_send_options options) & {
    return begin_message(websocket_opcode::binary, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_message(websocket_opcode opcode, bool compress) {
    require_active();
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is already open");
    }
    fragmented_message_active_ = true;
    ++fragmented_message_sequence_;
    return websocket_message_writer(*this, opcode, compress);
}

websocket_message_writer::websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept
    : owner_(&owner),
      message_id_(owner.fragmented_message_sequence_),
      opcode_(opcode),
      compress_(compress),
      registration_(owner.operation_scope_, this, &websocket_message_writer::expire_capability) {}

websocket_message_writer::websocket_message_writer(websocket_message_writer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      message_id_(other.message_id_),
      opcode_(other.opcode_),
      compress_(other.compress_),
      registration_(std::move(other.registration_), this) {}

websocket_message_writer::~websocket_message_writer() {
    abort();
}

void websocket_message_writer::expire_capability(void* target) noexcept {
    static_cast<websocket_message_writer*>(target)->owner_ = nullptr;
}

bool websocket_message_writer::message_open() const noexcept {
    return owner_ != nullptr && owner_->fragmented_message_active_ &&
           owner_->fragmented_message_sequence_ == message_id_;
}

void websocket_message_writer::abort() & noexcept {
    if (!message_open()) {
        owner_ = nullptr;
        return;
    }
    // The peer cannot tell where an unfinished message ends.
    auto* owner = std::exchange(owner_, nullptr);
    owner->fragmented_message_active_ = false;
    owner->abort();
}

task<void> websocket_message_writer::write(std::string_view chunk, operation_options operation) & {
    return submit(chunk, false, std::move(operation));
}

task<void> websocket_message_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return submit(as_chars(chunk), false, std::move(operation));
}

task<void> websocket_message_writer::finish(std::string_view last_chunk, operation_options operation) & {
    return submit(last_chunk, true, std::move(operation));
}

task<void> websocket_message_writer::finish(std::span<const std::byte> last_chunk, operation_options operation) & {
    return submit(as_chars(last_chunk), true, std::move(operation));
}

task<void> websocket_message_writer::submit(std::string_view chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, chunk, compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

task<void> websocket_message_writer::submit_owned(
    std::string&& chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, std::move(chunk), compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

}  // namespace ruvia
