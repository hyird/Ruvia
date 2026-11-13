#include "ruvia/web/session.h"

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
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/redis/redis_session.h"

#include "context/context_access.h"
#include "http/inline_http_header_views.h"
#include "http/tls_protocol_policy.h"
#endif

#include <algorithm>
#include <bit>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <span>

#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/rate_limit.h"

#include "context/context_access.h"
#include "http/inline_http_header_views.h"

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
