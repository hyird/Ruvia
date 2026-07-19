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
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/RateLimitRule.h"

namespace ruvia::detail {

class RateLimitAllowed final {
private:
    constexpr RateLimitAllowed() noexcept = default;
    friend class RateLimitDecision;
};

class RateLimitRejection final {
public:
    [[nodiscard]] constexpr std::chrono::milliseconds retryAfter() const noexcept {
        return retryAfter_;
    }

private:
    explicit constexpr RateLimitRejection(
        std::chrono::milliseconds retryAfter) noexcept
        : retryAfter_(retryAfter) {}

    std::chrono::milliseconds retryAfter_;
    friend class RateLimitDecision;
};

class RateLimitDecision final {
public:
    [[nodiscard]] static constexpr RateLimitDecision allow() noexcept {
        return RateLimitDecision(RateLimitAllowed{});
    }

    [[nodiscard]] static constexpr RateLimitDecision reject(
        std::chrono::milliseconds retryAfter) noexcept {
        return RateLimitDecision(RateLimitRejection(retryAfter));
    }

    [[nodiscard]] constexpr const RateLimitAllowed* allowed() const & noexcept {
        return std::get_if<RateLimitAllowed>(&value_);
    }

    [[nodiscard]] const RateLimitAllowed* allowed() const && = delete;

    [[nodiscard]] constexpr const RateLimitRejection* rejection() const & noexcept {
        return std::get_if<RateLimitRejection>(&value_);
    }

    [[nodiscard]] const RateLimitRejection* rejection() const && = delete;

private:
    explicit constexpr RateLimitDecision(RateLimitAllowed allowed) noexcept
        : value_(allowed) {}

    explicit constexpr RateLimitDecision(RateLimitRejection rejection) noexcept
        : value_(rejection) {}

    std::variant<RateLimitAllowed, RateLimitRejection> value_;
};

static_assert(std::is_trivially_copyable_v<RateLimitDecision>);
static_assert(sizeof(RateLimitDecision) <= 2 * sizeof(std::chrono::milliseconds));

[[nodiscard]] inline std::int64_t rateLimiterNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct SteadyRateLimiterClock final {
    [[nodiscard]] static std::int64_t nowMs() noexcept {
        return rateLimiterNowMs();
    }
};

enum class RouteRateLimitPresence : std::uint8_t {
    kAbsent,
    kPresent,
};

// One fixed-window table owned and accessed by exactly one HttpServer worker.
// Startup allocates every slot from WorkerMemory; request-path lookup mutates
// ordinary worker-local state and performs no allocation, locking, atomics, or
// cross-thread coordination. Keys are (route scope, normalized remote address).
template <typename Clock>
class BasicRateLimiter {
public:
    BasicRateLimiter(
        std::optional<RateLimitRule> defaultRulePerWorker,
        RouteRateLimitPresence routeRules,
        std::size_t slotCount,
        std::pmr::memory_resource* resource = nullptr)
        : defaultRulePerWorker_(std::move(defaultRulePerWorker)),
          slots_(pmrResourceOrDefault(resource)) {
        if (!std::has_single_bit(slotCount)) {
            throw std::invalid_argument(
                "rate-limit slot count must be a power of two");
        }
        if (defaultRulePerWorker_.has_value() ||
            routeRules == RouteRateLimitPresence::kPresent) {
            slots_.resize(slotCount);
        }
    }

    BasicRateLimiter(const BasicRateLimiter&) = delete;
    BasicRateLimiter& operator=(const BasicRateLimiter&) = delete;

    [[nodiscard]] bool hasDefaultRule() const noexcept {
        return defaultRulePerWorker_.has_value();
    }

    [[nodiscard]] std::size_t slotCapacity() const noexcept {
        return slots_.size();
    }

    [[nodiscard]] RateLimitDecision allowDefault(std::string_view remoteAddress) noexcept {
        return defaultRulePerWorker_.has_value()
            ? allow(kDefaultScope, remoteAddress, *defaultRulePerWorker_)
            : RateLimitDecision::allow();
    }

    [[nodiscard]] RateLimitDecision allowRoute(
        std::uintptr_t routeScope,
        std::string_view remoteAddress,
        const RateLimitRule& rule) noexcept {
        return allow(
            routeScope == 0 ? kFallbackRouteScope : routeScope,
            remoteAddress,
            rule);
    }

private:
    // Normalized IPv6 /64 keys use only 19 bytes. The larger fallback also
    // accommodates canonical peer strings carrying an IPv6 scope identifier.
    static constexpr std::size_t kMaxKeyBytes = 64;
    static constexpr std::uint64_t kEmptyHash = 0;
    static constexpr std::uintptr_t kDefaultScope = 1;
    static constexpr std::uintptr_t kFallbackRouteScope = 2;

    struct Slot final {
        std::uintptr_t scope{0};
        std::uint64_t keyHash{kEmptyHash};
        std::uint64_t resetAtMs{0};
        std::size_t count{0};
        std::uint8_t keySize{0};
        std::array<char, kMaxKeyBytes> key{};
    };
    static_assert(sizeof(Slot) <= 112, "worker rate-limit slots must stay compact");

    [[nodiscard]] static std::uint64_t keyHash(
        std::uintptr_t scope,
        std::string_view key) noexcept {
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
        return hash == kEmptyHash ? 1 : hash;
    }

    [[nodiscard]] static std::uint64_t safeNowMs(std::int64_t nowMs) noexcept {
        return nowMs <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(nowMs);
    }

    [[nodiscard]] static std::uint64_t currentResetAtMs(
        std::int64_t nowMs,
        const RateLimitRule& rule) noexcept {
        const auto now = safeNowMs(nowMs);
        const auto windowMs = static_cast<std::uint64_t>(rule.window().count());
        const auto bucket = now / windowMs;
        const auto maxTime = std::numeric_limits<std::uint64_t>::max();
        if (bucket >= maxTime / windowMs) {
            return maxTime;
        }
        return (bucket + 1) * windowMs;
    }

    [[nodiscard]] static std::int64_t resetAfterMs(
        std::int64_t nowMs,
        std::uint64_t resetAtMs) noexcept {
        const auto now = safeNowMs(nowMs);
        if (resetAtMs <= now) {
            return 1;
        }
        const auto remaining = resetAtMs - now;
        const auto maxHint = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
        return remaining > maxHint
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(remaining);
    }

    [[nodiscard]] static bool keyEquals(
        const Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash) noexcept {
        return slot.keyHash == hash &&
            slot.scope == scope &&
            slot.keySize == key.size() &&
            std::equal(key.begin(), key.end(), slot.key.begin());
    }

    [[nodiscard]] static bool expired(
        const Slot& slot,
        std::uint64_t nowMs) noexcept {
        return slot.keyHash != kEmptyHash && slot.resetAtMs <= nowMs;
    }

    static void install(
        Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash,
        std::uint64_t resetAtMs) noexcept {
        slot.scope = scope;
        slot.keyHash = hash;
        slot.resetAtMs = resetAtMs;
        slot.count = 1;
        slot.keySize = static_cast<std::uint8_t>(key.size());
        std::ranges::copy(key, slot.key.begin());
    }

    [[nodiscard]] static RateLimitDecision consume(
        Slot& slot,
        const RateLimitRule& rule,
        std::int64_t nowMs,
        std::uint64_t resetAtMs) noexcept {
        const auto now = safeNowMs(nowMs);
        if (slot.resetAtMs <= now) {
            slot.resetAtMs = resetAtMs;
            slot.count = 1;
            return RateLimitDecision::allow();
        }
        if (slot.count >= rule.maxRequests()) {
            return RateLimitDecision::reject(std::chrono::milliseconds(
                resetAfterMs(nowMs, slot.resetAtMs)));
        }
        ++slot.count;
        return RateLimitDecision::allow();
    }

    [[nodiscard]] RateLimitDecision allow(
        std::uintptr_t scope,
        std::string_view key,
        const RateLimitRule& rule) noexcept {
        const bool allowOnOverflow =
            rule.overflowPolicy() == RateLimitOverflowPolicy::kAllow;
        if (slots_.empty()) {
            return allowOnOverflow
                ? RateLimitDecision::allow()
                : RateLimitDecision::reject(std::chrono::milliseconds(1));
        }
        if (key.size() > kMaxKeyBytes) {
            return allowOnOverflow
                ? RateLimitDecision::allow()
                : RateLimitDecision::reject(std::chrono::milliseconds(1));
        }

        const auto nowMs = Clock::nowMs();
        const auto now = safeNowMs(nowMs);
        const auto resetAtMs = currentResetAtMs(nowMs, rule);
        const auto hash = keyHash(scope, key);
        const auto mask = slots_.size() - 1;
        const auto start = static_cast<std::size_t>(hash) & mask;
        Slot* reclaimable = nullptr;

        for (std::size_t probe = 0; probe < slots_.size(); ++probe) {
            auto& slot = slots_[(start + probe) & mask];
            if (slot.keyHash == kEmptyHash) {
                auto& target = reclaimable == nullptr ? slot : *reclaimable;
                install(target, scope, key, hash, resetAtMs);
                return RateLimitDecision::allow();
            }
            if (keyEquals(slot, scope, key, hash)) {
                return consume(slot, rule, nowMs, resetAtMs);
            }
            if (reclaimable == nullptr && expired(slot, now)) {
                reclaimable = &slot;
            }
        }

        if (reclaimable != nullptr) {
            install(*reclaimable, scope, key, hash, resetAtMs);
            return RateLimitDecision::allow();
        }
        return allowOnOverflow
            ? RateLimitDecision::allow()
            : RateLimitDecision::reject(std::chrono::milliseconds(1));
    }

    std::optional<RateLimitRule> defaultRulePerWorker_;
    std::pmr::vector<Slot> slots_;
};

class RateLimiter final : public BasicRateLimiter<SteadyRateLimiterClock> {
public:
    using BasicRateLimiter<SteadyRateLimiterClock>::BasicRateLimiter;
};

}  // namespace ruvia::detail
