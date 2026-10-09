#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/rate_limit_rule.h"

namespace ruvia::detail {

class rate_limit_allowed final {
private:
    constexpr rate_limit_allowed() noexcept = default;
    friend class rate_limit_decision;
};

class rate_limit_rejection final {
public:
    [[nodiscard]] constexpr std::chrono::milliseconds retry_after() const noexcept {
        return retry_after_;
    }

private:
    explicit constexpr rate_limit_rejection(std::chrono::milliseconds retry_after) noexcept
        : retry_after_(retry_after) {}

    std::chrono::milliseconds retry_after_;
    friend class rate_limit_decision;
};

class rate_limit_decision final {
public:
    [[nodiscard]] static constexpr rate_limit_decision allow() noexcept {
        return rate_limit_decision(rate_limit_allowed{});
    }

    [[nodiscard]] static constexpr rate_limit_decision reject(
        std::chrono::milliseconds retry_after) noexcept {
        return rate_limit_decision(rate_limit_rejection(retry_after));
    }

    [[nodiscard]] constexpr const rate_limit_allowed* allowed() const& noexcept {
        return std::get_if<rate_limit_allowed>(&value_);
    }

    [[nodiscard]] const rate_limit_allowed* allowed() const&& = delete;

    [[nodiscard]] constexpr const rate_limit_rejection* rejection() const& noexcept {
        return std::get_if<rate_limit_rejection>(&value_);
    }

    [[nodiscard]] const rate_limit_rejection* rejection() const&& = delete;

private:
    explicit constexpr rate_limit_decision(rate_limit_allowed allowed) noexcept
        : value_(allowed) {}

    explicit constexpr rate_limit_decision(rate_limit_rejection rejection) noexcept
        : value_(rejection) {}

    std::variant<rate_limit_allowed, rate_limit_rejection> value_;
};

static_assert(std::is_trivially_copyable_v<rate_limit_decision>);
static_assert(sizeof(rate_limit_decision) <= 2 * sizeof(std::chrono::milliseconds));

[[nodiscard]] inline std::int64_t rate_limiter_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct steady_rate_limiter_clock final {
    [[nodiscard]] static std::int64_t now_ms() noexcept {
        return rate_limiter_now_ms();
    }
};

enum class route_rate_limit_presence : std::uint8_t {
    absent,
    present,
};

// One fixed-window table owned and accessed by exactly one web_worker_runtime
// worker. Startup allocates every slot from worker_memory; request-path lookup
// mutates ordinary worker-local state and performs no allocation, locking,
// atomics, or cross-thread coordination. Keys are (route scope, normalized
// remote address).
template <typename clock_type>
class rate_limiter final {
public:
    rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
        route_rate_limit_presence route_rules, std::size_t capacity_value,
        std::pmr::memory_resource* resource = nullptr)
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

    rate_limiter(const rate_limiter&) = delete;
    rate_limiter& operator=(const rate_limiter&) = delete;

    [[nodiscard]] bool has_default_rule() const noexcept {
        return default_rule_per_worker_.has_value();
    }

    [[nodiscard]] std::size_t key_capacity() const noexcept {
        return slots_.size();
    }

    [[nodiscard]] rate_limit_decision allow_default(std::string_view remote_address) noexcept {
        return default_rule_per_worker_.has_value()
                   ? allow(default_scope, remote_address, *default_rule_per_worker_)
                   : rate_limit_decision::allow();
    }

    [[nodiscard]] rate_limit_decision allow_route(std::uintptr_t route_scope,
        std::string_view remote_address, const rate_limit_rule& rule) noexcept {
        return allow(route_scope == 0 ? fallback_route_scope : route_scope, remote_address, rule);
    }

private:
    // Normalized IPv6 /64 keys use only 19 bytes. The larger fallback also
    // accommodates canonical peer strings carrying an IPv6 scope identifier.
    static constexpr std::size_t max_key_bytes = 64;
    static constexpr std::uint64_t empty_hash = 0;
    static constexpr std::uintptr_t default_scope = 1;
    static constexpr std::uintptr_t fallback_route_scope = 2;

    struct slot_type final {
        std::uintptr_t scope_{0};
        std::uint64_t key_hash_{empty_hash};
        std::uint64_t reset_at_ms_{0};
        std::size_t count_{0};
        std::uint8_t key_size_{0};
        std::array<char, max_key_bytes> key_{};
    };
    static_assert(sizeof(slot_type) <= 112, "worker rate-limit slots must stay compact");

    [[nodiscard]] static std::uint64_t key_hash(
        std::uintptr_t scope, std::string_view key) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](std::uint64_t value) noexcept {
            for (std::size_t i = 0; i < sizeof(value); ++i) {
                hash ^= static_cast<unsigned char>((value >> (i * 8U)) & 0xffU);
                hash *= 1099511628211ULL;
            }
        };
        mix(static_cast<std::uint64_t>(scope));
        for (const unsigned char byte : key) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash == empty_hash ? 1 : hash;
    }

    [[nodiscard]] static std::uint64_t safe_now_ms(std::int64_t now_ms) noexcept {
        return now_ms <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(now_ms);
    }

    [[nodiscard]] static std::uint64_t next_reset_at_ms(
        std::int64_t now_ms, const rate_limit_rule& rule) noexcept {
        const auto now = safe_now_ms(now_ms);
        const auto window_ms = static_cast<std::uint64_t>(rule.window_.count());
        const auto max_time = std::numeric_limits<std::uint64_t>::max();
        return window_ms > max_time - now ? max_time : now + window_ms;
    }

    [[nodiscard]] static std::int64_t reset_after_ms(
        std::int64_t now_ms, std::uint64_t reset_at_ms) noexcept {
        const auto now = safe_now_ms(now_ms);
        if (reset_at_ms <= now) {
            return 1;
        }
        const auto remaining = reset_at_ms - now;
        const auto max_hint = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return remaining > max_hint ? std::numeric_limits<std::int64_t>::max()
                                    : static_cast<std::int64_t>(remaining);
    }

    [[nodiscard]] static bool key_equals(
        const slot_type& slot, std::uintptr_t scope, std::string_view key, std::uint64_t hash) noexcept {
        return slot.key_hash_ == hash && slot.scope_ == scope && slot.key_size_ == key.size() &&
               std::ranges::equal(key, std::span(slot.key_).first(slot.key_size_));
    }

    [[nodiscard]] static bool expired(const slot_type& slot, std::uint64_t now_ms) noexcept {
        return slot.key_hash_ != empty_hash && slot.reset_at_ms_ <= now_ms;
    }

    static void install(slot_type& slot, std::uintptr_t scope, std::string_view key, std::uint64_t hash,
        std::uint64_t reset_at_ms) noexcept {
        slot.scope_ = scope;
        slot.key_hash_ = hash;
        slot.reset_at_ms_ = reset_at_ms;
        slot.count_ = 1;
        slot.key_size_ = static_cast<std::uint8_t>(key.size());
        std::ranges::copy(key, slot.key_.begin());
    }

    [[nodiscard]] static rate_limit_decision consume(slot_type& slot, const rate_limit_rule& rule,
        std::int64_t now_ms, std::uint64_t reset_at_ms) noexcept {
        const auto now = safe_now_ms(now_ms);
        if (slot.reset_at_ms_ <= now) {
            slot.reset_at_ms_ = reset_at_ms;
            slot.count_ = 1;
            return rate_limit_decision::allow();
        }
        if (slot.count_ >= rule.max_requests_) {
            return rate_limit_decision::reject(
                std::chrono::milliseconds(reset_after_ms(now_ms, slot.reset_at_ms_)));
        }
        ++slot.count_;
        return rate_limit_decision::allow();
    }

    [[nodiscard]] rate_limit_decision allow(
        std::uintptr_t scope, std::string_view key, const rate_limit_rule& rule) noexcept {
        const bool allow_on_overflow = rule.overflow_policy_ == rate_limit_overflow_policy::allow;
        if (slots_.empty()) {
            return allow_on_overflow ? rate_limit_decision::allow()
                                     : rate_limit_decision::reject(std::chrono::milliseconds(1));
        }
        if (key.size() > max_key_bytes) {
            return allow_on_overflow ? rate_limit_decision::allow()
                                     : rate_limit_decision::reject(std::chrono::milliseconds(1));
        }

        const auto now_ms = clock_type::now_ms();
        const auto now = safe_now_ms(now_ms);
        const auto reset_at_ms = next_reset_at_ms(now_ms, rule);
        const auto hash = key_hash(scope, key);
        const auto mask = slots_.size() - 1;
        const auto start = static_cast<std::size_t>(hash) & mask;
        slot_type* reclaimable = nullptr;

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
        return allow_on_overflow ? rate_limit_decision::allow()
                                 : rate_limit_decision::reject(std::chrono::milliseconds(1));
    }

    std::optional<rate_limit_rule> default_rule_per_worker_;
    std::pmr::vector<slot_type> slots_;
};

using rate_limiter_type = rate_limiter<steady_rate_limiter_clock>;

}  // namespace ruvia::detail
