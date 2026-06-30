#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <thread>

#include "ruvia/app/RateLimitRule.h"

namespace ruvia::detail {

struct RateLimitCheck final {
    bool allowed{true};
    std::int64_t resetAfterMs{1};
};

[[nodiscard]] inline std::int64_t rateLimiterNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Single-process shared fixed-window limiter. All worker threads hit the same
// startup-allocated open-addressing table; request-path updates are atomic CAS
// operations and never allocate or take a lock. Keys are (scope, remote IP).
class RateLimiter final {
public:
    explicit RateLimiter(
        RateLimitRule appRule,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          appRule_(normalizeRateLimitRule(appRule)),
          slotCount_(nextPowerOfTwo(appRule_.slotCount)),
          slots_(allocateSlots(slotCount_)) {}

    ~RateLimiter() {
        if (slots_ == nullptr) {
            return;
        }
        auto allocator = allocatorFor(resource_);
        for (std::size_t i = 0; i < slotCount_; ++i) {
            std::destroy_at(slots_ + i);
        }
        allocator.deallocate(slots_, slotCount_);
    }

    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return appRule_.maxRequests > 0;
    }

    [[nodiscard]] RateLimitCheck allowGlobal(std::string_view remoteAddress) noexcept {
        return allow(kGlobalScope, remoteAddress, appRule_, appRule_.failClosed);
    }

    [[nodiscard]] RateLimitCheck allowRoute(
        std::uintptr_t routeScope,
        std::string_view remoteAddress,
        const RateLimitRule& rule) noexcept {
        return allow(routeScope == 0 ? kFallbackRouteScope : routeScope, remoteAddress, rule, appRule_.failClosed);
    }

private:
    static constexpr std::size_t kMaxKeyBytes = 64;
    static constexpr std::uint64_t kEmptyHash = 0;
    static constexpr std::uint64_t kInstallingHash = 1;
    static constexpr std::uint64_t kCountBits = kRateLimitCounterBits;
    static constexpr std::uint64_t kCountMask = static_cast<std::uint64_t>(kMaxRateLimitRequests);
    static constexpr std::uintptr_t kGlobalScope = 1;
    static constexpr std::uintptr_t kFallbackRouteScope = 2;

    struct Slot final {
        std::atomic<std::uintptr_t> scope{0};
        std::atomic<std::uint64_t> keyHash{kEmptyHash};
        std::atomic<std::uint16_t> keySize{0};
        std::array<char, kMaxKeyBytes> keyBytes{};
        std::atomic<std::uint64_t> state{0};
    };

    [[nodiscard]] static std::pmr::polymorphic_allocator<Slot> allocatorFor(
        std::pmr::memory_resource* resource) noexcept {
        return std::pmr::polymorphic_allocator<Slot>(resource);
    }

    [[nodiscard]] Slot* allocateSlots(std::size_t count) {
        auto allocator = allocatorFor(resource_);
        auto* slots = allocator.allocate(count);
        std::size_t constructed = 0;
        try {
            for (; constructed < count; ++constructed) {
                std::construct_at(slots + constructed);
            }
        } catch (...) {
            while (constructed > 0) {
                --constructed;
                std::destroy_at(slots + constructed);
            }
            allocator.deallocate(slots, count);
            throw;
        }
        return slots;
    }

    [[nodiscard]] static std::size_t nextPowerOfTwo(std::size_t value) noexcept {
        std::size_t result = 1;
        while (result < value) {
            result <<= 1U;
        }
        return result;
    }

    [[nodiscard]] static std::uint64_t keyHash(std::uintptr_t scope, std::string_view key) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](std::uint64_t value) noexcept {
            for (std::size_t i = 0; i < sizeof(value); ++i) {
                hash ^= static_cast<unsigned char>((value >> (i * 8U)) & 0xffU);
                hash *= 1099511628211ULL;
            }
        };
        mix(static_cast<std::uint64_t>(scope));
        for (const unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash <= kInstallingHash ? hash + kInstallingHash + 1 : hash;
    }

    [[nodiscard]] static std::uint64_t currentWindow(std::int64_t nowMs, const RateLimitRule& rule) noexcept {
        const auto safeNow = nowMs < 0 ? std::int64_t{0} : nowMs;
        return static_cast<std::uint64_t>(safeNow / rule.window.count());
    }

    [[nodiscard]] static std::int64_t windowResetAfter(
        std::int64_t nowMs,
        std::uint64_t window,
        const RateLimitRule& rule) noexcept {
        const auto nextWindowMs = static_cast<std::int64_t>(window + 1) * rule.window.count();
        const auto remaining = nextWindowMs - nowMs;
        return remaining <= 0 ? 1 : remaining;
    }

    [[nodiscard]] static std::uint64_t packState(std::uint64_t window, std::uint64_t count) noexcept {
        return (window << kCountBits) | (count & kCountMask);
    }

    [[nodiscard]] static std::uint64_t stateWindow(std::uint64_t state) noexcept {
        return state >> kCountBits;
    }

    [[nodiscard]] static std::uint64_t stateCount(std::uint64_t state) noexcept {
        return state & kCountMask;
    }

    [[nodiscard]] static bool keyEquals(
        const Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash) noexcept {
        if (slot.keyHash.load(std::memory_order_acquire) != hash) {
            return false;
        }
        if (slot.scope.load(std::memory_order_acquire) != scope) {
            return false;
        }
        const auto size = slot.keySize.load(std::memory_order_acquire);
        if (size != key.size()) {
            return false;
        }
        return std::memcmp(slot.keyBytes.data(), key.data(), size) == 0;
    }

    [[nodiscard]] bool tryInstall(
        Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash,
        std::uint64_t window,
        std::uint64_t expectedHash) noexcept {
        if (!slot.keyHash.compare_exchange_strong(
                expectedHash,
                kInstallingHash,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        std::memcpy(slot.keyBytes.data(), key.data(), key.size());
        slot.scope.store(scope, std::memory_order_release);
        slot.keySize.store(static_cast<std::uint16_t>(key.size()), std::memory_order_release);
        slot.state.store(packState(window, 0), std::memory_order_release);
        slot.keyHash.store(hash, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static RateLimitCheck consume(
        Slot& slot,
        std::int64_t nowMs,
        const RateLimitRule& rule,
        std::uint64_t window) noexcept {
        for (;;) {
            auto observed = slot.state.load(std::memory_order_acquire);
            const auto observedWindow = stateWindow(observed);
            const auto observedCount = stateCount(observed);
            const auto resetAfterMs = windowResetAfter(nowMs, window, rule);

            if (observedWindow != window) {
                const auto desired = packState(window, 1);
                if (slot.state.compare_exchange_weak(
                        observed,
                        desired,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfterMs};
                }
                continue;
            }

            if (observedCount >= rule.maxRequests) {
                return RateLimitCheck{.allowed = false, .resetAfterMs = resetAfterMs};
            }

            const auto desired = packState(window, observedCount + 1);
            if (slot.state.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfterMs};
            }
        }
    }

    [[nodiscard]] RateLimitCheck allow(
        std::uintptr_t scope,
        std::string_view key,
        const RateLimitRule& rule,
        bool failClosed) noexcept {
        if (rule.maxRequests == 0) {
            return RateLimitCheck{};
        }
        if (key.size() > kMaxKeyBytes) {
            return RateLimitCheck{.allowed = !failClosed, .resetAfterMs = 1};
        }

        const auto nowMs = rateLimiterNowMs();
        const auto window = currentWindow(nowMs, rule);
        const auto hash = keyHash(scope, key);
        const auto start = static_cast<std::size_t>(hash) & (slotCount_ - 1);

        for (std::size_t probe = 0; probe < slotCount_; ++probe) {
            auto& slot = slots_[(start + probe) & (slotCount_ - 1)];
            const auto observedHash = slot.keyHash.load(std::memory_order_acquire);
            if (observedHash == hash && keyEquals(slot, scope, key, hash)) {
                return consume(slot, nowMs, rule, window);
            }
            if (observedHash == kInstallingHash) {
                std::this_thread::yield();
                --probe;
                continue;
            }
            if (observedHash == kEmptyHash &&
                tryInstall(slot, scope, key, hash, window, observedHash)) {
                return consume(slot, nowMs, rule, window);
            }
        }

        return RateLimitCheck{.allowed = !failClosed, .resetAfterMs = 1};
    }

    std::pmr::memory_resource* resource_;
    RateLimitRule appRule_;
    std::size_t slotCount_;
    Slot* slots_{nullptr};
};

}  // namespace ruvia::detail
