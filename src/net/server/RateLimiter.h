#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <thread>

#include "ruvia/app/RateLimitRule.h"
#include "ruvia/memory/PmrResource.h"

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
        std::pmr::memory_resource* resource = nullptr)
        : resource_(pmrResourceOrDefault(resource)),
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
    static constexpr std::size_t kKeyWordBytes = sizeof(std::uint64_t);
    static constexpr std::size_t kMaxKeyWords = (kMaxKeyBytes + kKeyWordBytes - 1) / kKeyWordBytes;
    static constexpr std::size_t kLazyReclaimProbeStart = 4;
    static constexpr std::uint64_t kEmptyHash = 0;
    static constexpr std::uint64_t kInstallingHash = 1;
    static constexpr std::uint64_t kReclaimingState = 0;
    static constexpr std::uint64_t kCountBits = kRateLimitCounterBits;
    static constexpr std::uint64_t kCountMask = static_cast<std::uint64_t>(kMaxRateLimitRequests);
    static constexpr std::uint64_t kMaxStateTimeMs = UINT64_MAX >> kCountBits;
    static constexpr std::uintptr_t kGlobalScope = 1;
    static constexpr std::uintptr_t kFallbackRouteScope = 2;
    static constexpr std::size_t kSlotAlignment = 64;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(kSlotAlignment) Slot final {
        std::atomic<std::uintptr_t> scope{0};
        std::atomic<std::uint64_t> keyHash{kEmptyHash};
        std::atomic<std::uint16_t> keySize{0};
        std::array<std::atomic<std::uint64_t>, kMaxKeyWords> keyWords{};
        std::atomic<std::uint64_t> state{0};
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    static_assert(alignof(Slot) >= kSlotAlignment, "rate limit slots must not share a cache line");

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

    [[nodiscard]] static std::uint64_t currentResetAtMs(std::int64_t nowMs, const RateLimitRule& rule) noexcept {
        const auto safeNow = nowMs < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(nowMs);
        const auto windowMs = static_cast<std::uint64_t>(rule.window.count());
        const auto bucket = safeNow / windowMs;
        if (bucket >= kMaxStateTimeMs / windowMs) {
            return kMaxStateTimeMs;
        }
        return (bucket + 1) * windowMs;
    }

    [[nodiscard]] static std::int64_t resetAfterMs(
        std::int64_t nowMs,
        std::uint64_t resetAtMs) noexcept {
        const auto remaining = static_cast<std::int64_t>(resetAtMs) - nowMs;
        return remaining <= 0 ? 1 : remaining;
    }

    [[nodiscard]] static std::uint64_t packState(std::uint64_t resetAtMs, std::uint64_t count) noexcept {
        return (resetAtMs << kCountBits) | (count & kCountMask);
    }

    [[nodiscard]] static std::uint64_t stateResetAtMs(std::uint64_t state) noexcept {
        return state >> kCountBits;
    }

    [[nodiscard]] static std::uint64_t stateCount(std::uint64_t state) noexcept {
        return state & kCountMask;
    }

    [[nodiscard]] static bool stateExpired(std::uint64_t state, std::int64_t nowMs) noexcept {
        if (state == kReclaimingState) {
            return true;
        }
        const auto safeNow = nowMs < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(nowMs);
        return stateResetAtMs(state) <= safeNow;
    }

    [[nodiscard]] static std::size_t keyWordCount(std::size_t keySize) noexcept {
        return (keySize + kKeyWordBytes - 1) / kKeyWordBytes;
    }

    [[nodiscard]] static std::uint64_t keyWord(std::string_view key, std::size_t wordIndex) noexcept {
        std::uint64_t word = 0;
        const auto offset = wordIndex * kKeyWordBytes;
        const auto remaining = key.size() - offset;
        const auto bytes = std::min(kKeyWordBytes, remaining);
        for (std::size_t i = 0; i < bytes; ++i) {
            word |= static_cast<std::uint64_t>(static_cast<unsigned char>(key[offset + i])) << (i * 8U);
        }
        return word;
    }

    static void writeKeyWords(Slot& slot, std::string_view key) noexcept {
        const auto words = keyWordCount(key.size());
        for (std::size_t i = 0; i < words; ++i) {
            slot.keyWords[i].store(keyWord(key, i), std::memory_order_relaxed);
        }
    }

    [[nodiscard]] static bool keyWordsEqual(const Slot& slot, std::string_view key) noexcept {
        const auto words = keyWordCount(key.size());
        for (std::size_t i = 0; i < words; ++i) {
            if (slot.keyWords[i].load(std::memory_order_relaxed) != keyWord(key, i)) {
                return false;
            }
        }
        return true;
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
        if (!keyWordsEqual(slot, key)) {
            return false;
        }
        return slot.keyHash.load(std::memory_order_acquire) == hash;
    }

    static void publishSlot(
        Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash,
        std::uint64_t resetAtMs,
        std::uint64_t initialCount) noexcept {
        writeKeyWords(slot, key);
        slot.scope.store(scope, std::memory_order_release);
        slot.keySize.store(static_cast<std::uint16_t>(key.size()), std::memory_order_release);
        slot.state.store(packState(resetAtMs, initialCount), std::memory_order_release);
        slot.keyHash.store(hash, std::memory_order_release);
    }

    [[nodiscard]] bool tryInstall(
        Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash,
        std::uint64_t resetAtMs,
        std::uint64_t expectedHash) noexcept {
        if (!slot.keyHash.compare_exchange_strong(
                expectedHash,
                kInstallingHash,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        publishSlot(slot, scope, key, hash, resetAtMs, 1);
        return true;
    }

    [[nodiscard]] bool tryReclaimExpired(
        Slot& slot,
        std::uintptr_t scope,
        std::string_view key,
        std::uint64_t hash,
        std::uint64_t observedHash,
        std::uint64_t resetAtMs,
        std::int64_t nowMs) noexcept {
        if (observedHash <= kInstallingHash) {
            return false;
        }
        if (!slot.keyHash.compare_exchange_strong(
                observedHash,
                kInstallingHash,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        auto observedState = slot.state.load(std::memory_order_acquire);
        for (;;) {
            if (!stateExpired(observedState, nowMs)) {
                slot.keyHash.store(observedHash, std::memory_order_release);
                return false;
            }
            if (slot.state.compare_exchange_weak(
                    observedState,
                    kReclaimingState,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                publishSlot(slot, scope, key, hash, resetAtMs, 1);
                return true;
            }
        }
    }

    struct ConsumeResult final {
        RateLimitCheck check{};
        bool retry{false};
    };

    [[nodiscard]] static ConsumeResult consume(
        Slot& slot,
        std::uint64_t hash,
        const RateLimitRule& rule,
        std::uint64_t resetAtMs,
        std::int64_t resetAfter) noexcept {
        for (;;) {
            if (slot.keyHash.load(std::memory_order_acquire) != hash) {
                return ConsumeResult{.retry = true};
            }
            auto observed = slot.state.load(std::memory_order_acquire);
            if (observed == kReclaimingState) {
                return ConsumeResult{.retry = true};
            }
            const auto observedResetAtMs = stateResetAtMs(observed);
            const auto observedCount = stateCount(observed);

            if (observedResetAtMs != resetAtMs) {
                const auto desired = packState(resetAtMs, 1);
                if (slot.state.compare_exchange_weak(
                        observed,
                        desired,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return ConsumeResult{.check = RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter}};
                }
                continue;
            }

            if (observedCount >= rule.maxRequests) {
                if (slot.keyHash.load(std::memory_order_acquire) != hash) {
                    return ConsumeResult{.retry = true};
                }
                return ConsumeResult{.check = RateLimitCheck{.allowed = false, .resetAfterMs = resetAfter}};
            }

            const auto desired = packState(resetAtMs, observedCount + 1);
            if (slot.state.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return ConsumeResult{.check = RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter}};
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
        const auto resetAtMs = currentResetAtMs(nowMs, rule);
        const auto resetAfter = resetAfterMs(nowMs, resetAtMs);
        const auto hash = keyHash(scope, key);
        const auto start = static_cast<std::size_t>(hash) & (slotCount_ - 1);
        Slot* expiredSlot = nullptr;
        std::uint64_t expiredHash = kEmptyHash;

        for (std::size_t probe = 0; probe < slotCount_; ++probe) {
            auto& slot = slots_[(start + probe) & (slotCount_ - 1)];
            const auto observedHash = slot.keyHash.load(std::memory_order_acquire);
            if (observedHash == hash && keyEquals(slot, scope, key, hash)) {
                const auto result = consume(slot, hash, rule, resetAtMs, resetAfter);
                if (!result.retry) {
                    return result.check;
                }
                continue;
            }
            if (observedHash == kInstallingHash) {
                std::this_thread::yield();
                --probe;
                continue;
            }
            if (observedHash == kEmptyHash) {
                if (expiredSlot != nullptr &&
                    tryReclaimExpired(*expiredSlot, scope, key, hash, expiredHash, resetAtMs, nowMs)) {
                    return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter};
                }
                if (tryInstall(slot, scope, key, hash, resetAtMs, observedHash)) {
                    return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter};
                }
                --probe;
                continue;
            }
            if (expiredSlot == nullptr &&
                probe >= kLazyReclaimProbeStart &&
                stateExpired(slot.state.load(std::memory_order_acquire), nowMs)) {
                expiredSlot = &slot;
                expiredHash = observedHash;
            }
        }

        if (expiredSlot != nullptr &&
            tryReclaimExpired(*expiredSlot, scope, key, hash, expiredHash, resetAtMs, nowMs)) {
            return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter};
        }

        const auto skippedProbes = std::min(kLazyReclaimProbeStart, slotCount_);
        for (std::size_t probe = 0; probe < skippedProbes; ++probe) {
            auto& slot = slots_[(start + probe) & (slotCount_ - 1)];
            const auto observedHash = slot.keyHash.load(std::memory_order_acquire);
            if (observedHash == kInstallingHash) {
                std::this_thread::yield();
                --probe;
                continue;
            }
            if (stateExpired(slot.state.load(std::memory_order_acquire), nowMs) &&
                tryReclaimExpired(slot, scope, key, hash, observedHash, resetAtMs, nowMs)) {
                return RateLimitCheck{.allowed = true, .resetAfterMs = resetAfter};
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
