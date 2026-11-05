#include "ratelimit/rate_limiter.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <random>
#include <span>

#include "ruvia/core/memory/pmr_object.h"

#include "util/steady_clock.h"

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

[[nodiscard]] std::uint64_t safe_now_ms(std::int64_t now_ms) noexcept {
    return now_ms <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(now_ms);
}

[[nodiscard]] std::uint64_t next_reset_at_ms(std::uint64_t now_ms, const rate_limit_rule& rule) noexcept {
    const auto window_ms = static_cast<std::uint64_t>(rule.window_.count());
    const auto max_time = std::numeric_limits<std::uint64_t>::max();
    return window_ms > max_time - now_ms ? max_time : now_ms + window_ms;
}

[[nodiscard]] std::int64_t reset_after_ms(std::uint64_t now_ms, std::uint64_t reset_at_ms) noexcept {
    if (reset_at_ms <= now_ms) {
        return 1;
    }
    const auto remaining = reset_at_ms - now_ms;
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

[[nodiscard]] std::optional<rate_limit_rejection> consume(rate_limit_slot& slot, const rate_limit_rule& rule,
    std::uint64_t now_ms, std::uint64_t reset_at_ms) noexcept {
    if (slot.reset_at_ms_ <= now_ms) {
        slot.reset_at_ms_ = reset_at_ms;
        slot.count_ = 1;
        return std::nullopt;
    }
    if (slot.count_ >= rule.max_requests_) {
        return rate_limit_rejection(
            std::chrono::milliseconds(reset_after_ms(now_ms, slot.reset_at_ms_)), rule.max_requests_);
    }
    ++slot.count_;
    return std::nullopt;
}

[[nodiscard]] std::optional<rate_limit_rejection> overflow_decision(const rate_limit_rule& rule) noexcept {
    return rule.overflow_policy_ == rate_limit_overflow_policy::allow
               ? std::nullopt
               : std::optional(rate_limit_rejection(std::chrono::milliseconds(1), rule.max_requests_));
}

}  // namespace

rate_limiter::rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
    route_rate_limit_presence route_rules, std::size_t capacity_value, std::pmr::memory_resource* resource)
    : default_rule_per_worker_(default_rule_per_worker),
      slots_(pmr_resource_or_default(resource)) {
    // http_server_options::set_rate_limit, the only writer of the rule and the
    // capacity, validated both; the capacity is a power of two for the mask.
    if (default_rule_per_worker_.has_value() || route_rules == route_rate_limit_presence::present) {
        slots_.resize(capacity_value);
        hash_key_ = random_hash_key();
    }
}

std::optional<rate_limit_rejection> rate_limiter::allow_default(std::string_view remote_address) noexcept {
    return default_rule_per_worker_.has_value() ? allow(default_scope, remote_address, *default_rule_per_worker_)
                                                : std::nullopt;
}

std::optional<rate_limit_rejection> rate_limiter::allow_route(std::uintptr_t route_scope,
    const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule) noexcept {
    return allow({.route_ = route_scope == 0 ? fallback_route_scope : route_scope,
                     .rule_ = reinterpret_cast<std::uintptr_t>(rule_declaration)},
        remote_address, rule);
}

std::optional<rate_limit_rejection> rate_limiter::allow(
    rate_limit_scope scope, std::string_view key, const rate_limit_rule& rule) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_decision(rule);
    }

    const auto now_ms = safe_now_ms(steady_now_ms());
    const auto reset_at_ms = next_reset_at_ms(now_ms, rule);
    const auto hash = key_hash(hash_key_, scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;

    // Slots never return to empty, so a key is installed and later found
    // within the same bounded window from its home slot. A saturated window
    // costs a fixed probe count instead of a scan of the whole table, like a
    // pingora-limits estimator's fixed per-operation cost.
    const auto probe_window = std::min(slots_.size(), rate_limit_probe_window);
    for (std::size_t probe_value = 0; probe_value < probe_window; ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            auto& target = reclaimable == nullptr ? slot : *reclaimable;
            install(target, scope, key, hash, reset_at_ms);
            return std::nullopt;
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume(slot, rule, now_ms, reset_at_ms);
        }
        if (reclaimable == nullptr && expired(slot, now_ms)) {
            reclaimable = &slot;
        }
    }

    if (reclaimable != nullptr) {
        install(*reclaimable, scope, key, hash, reset_at_ms);
        return std::nullopt;
    }
    return overflow_decision(rule);
}

}  // namespace ruvia::detail
