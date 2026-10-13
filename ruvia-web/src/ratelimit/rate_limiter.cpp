#include "ratelimit/rate_limiter.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/util/fnv1a.h"

#include "util/steady_clock.h"

namespace ruvia::detail {

namespace {

constexpr std::uint64_t empty_hash = 0;
constexpr rate_limit_scope default_scope{.route_ = 1};
constexpr std::uintptr_t fallback_route_scope = 2;

[[nodiscard]] std::uint64_t key_hash(rate_limit_scope scope, std::string_view key) noexcept {
    auto hash = fnv1a_accumulate_integer(fnv1a_offset_basis, static_cast<std::uint64_t>(scope.route_));
    hash = fnv1a_accumulate_integer(hash, static_cast<std::uint64_t>(scope.rule_));
    hash = fnv1a_accumulate(hash, key);
    return hash == empty_hash ? 1 : hash;
}

[[nodiscard]] std::uint64_t safe_now_ms(std::int64_t now_ms) noexcept {
    return now_ms <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(now_ms);
}

[[nodiscard]] std::uint64_t next_reset_at_ms(std::int64_t now_ms, const rate_limit_rule& rule) noexcept {
    const auto now = safe_now_ms(now_ms);
    const auto window_ms = static_cast<std::uint64_t>(rule.window_.count());
    const auto max_time = std::numeric_limits<std::uint64_t>::max();
    return window_ms > max_time - now ? max_time : now + window_ms;
}

[[nodiscard]] std::int64_t reset_after_ms(std::int64_t now_ms, std::uint64_t reset_at_ms) noexcept {
    const auto now = safe_now_ms(now_ms);
    if (reset_at_ms <= now) {
        return 1;
    }
    const auto remaining = reset_at_ms - now;
    const auto max_hint = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return remaining > max_hint ? std::numeric_limits<std::int64_t>::max()
                                : static_cast<std::int64_t>(remaining);
}

[[nodiscard]] bool key_equals(
    const rate_limit_slot& slot, rate_limit_scope scope, std::string_view key, std::uint64_t hash) noexcept {
    return slot.key_hash_ == hash && slot.scope_ == scope && slot.key_size_ == key.size() &&
           std::ranges::equal(key, std::span(slot.key_).first(slot.key_size_));
}

[[nodiscard]] bool expired(const rate_limit_slot& slot, std::uint64_t now_ms) noexcept {
    return slot.key_hash_ != empty_hash && slot.reset_at_ms_ <= now_ms;
}

void install(rate_limit_slot& slot, rate_limit_scope scope, std::string_view key, std::uint64_t hash,
    std::uint64_t reset_at_ms) noexcept {
    slot.scope_ = scope;
    slot.key_hash_ = hash;
    slot.reset_at_ms_ = reset_at_ms;
    slot.count_ = 1;
    slot.key_size_ = static_cast<std::uint8_t>(key.size());
    std::ranges::copy(key, slot.key_.begin());
}

[[nodiscard]] rate_limit_decision consume(rate_limit_slot& slot, const rate_limit_rule& rule,
    std::int64_t now_ms, std::uint64_t reset_at_ms) noexcept {
    const auto now = safe_now_ms(now_ms);
    if (slot.reset_at_ms_ <= now) {
        slot.reset_at_ms_ = reset_at_ms;
        slot.count_ = 1;
        return rate_limit_decision::allow();
    }
    if (slot.count_ >= rule.max_requests_) {
        return rate_limit_decision::reject(std::chrono::milliseconds(reset_after_ms(now_ms, slot.reset_at_ms_)));
    }
    ++slot.count_;
    return rate_limit_decision::allow();
}

[[nodiscard]] rate_limit_decision overflow_decision(const rate_limit_rule& rule) noexcept {
    return rule.overflow_policy_ == rate_limit_overflow_policy::allow
               ? rate_limit_decision::allow()
               : rate_limit_decision::reject(std::chrono::milliseconds(1));
}

}  // namespace

rate_limiter::rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
    route_rate_limit_presence route_rules, std::size_t capacity_value, std::pmr::memory_resource* resource)
    : default_rule_per_worker_(default_rule_per_worker),
      slots_(pmr_resource_or_default(resource)) {
    if (default_rule_per_worker_.has_value()) {
        validate_rate_limit_rule(*default_rule_per_worker_);
    }
    if (!std::has_single_bit(capacity_value)) {
        throw std::invalid_argument("rate-limit capacity must be a power of two");
    }
    if (default_rule_per_worker_.has_value() || route_rules == route_rate_limit_presence::present) {
        slots_.resize(capacity_value);
    }
}

rate_limit_decision rate_limiter::allow_default(std::string_view remote_address) noexcept {
    return default_rule_per_worker_.has_value() ? allow(default_scope, remote_address, *default_rule_per_worker_)
                                                : rate_limit_decision::allow();
}

rate_limit_decision rate_limiter::allow_route(std::uintptr_t route_scope, const void* rule_declaration,
    std::string_view remote_address, const rate_limit_rule& rule) noexcept {
    return allow({.route_ = route_scope == 0 ? fallback_route_scope : route_scope,
                     .rule_ = reinterpret_cast<std::uintptr_t>(rule_declaration)},
        remote_address, rule);
}

rate_limit_decision rate_limiter::allow(
    rate_limit_scope scope, std::string_view key, const rate_limit_rule& rule) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_decision(rule);
    }

    const auto now_ms = steady_now_ms();
    const auto now = safe_now_ms(now_ms);
    const auto reset_at_ms = next_reset_at_ms(now_ms, rule);
    const auto hash = key_hash(scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;

    for (std::size_t probe_value = 0; probe_value < slots_.size(); ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            auto& target = reclaimable == nullptr ? slot : *reclaimable;
            install(target, scope, key, hash, reset_at_ms);
            return rate_limit_decision::allow();
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume(slot, rule, now_ms, reset_at_ms);
        }
        if (reclaimable == nullptr && expired(slot, now)) {
            reclaimable = &slot;
        }
    }

    if (reclaimable != nullptr) {
        install(*reclaimable, scope, key, hash, reset_at_ms);
        return rate_limit_decision::allow();
    }
    return overflow_decision(rule);
}

}  // namespace ruvia::detail
