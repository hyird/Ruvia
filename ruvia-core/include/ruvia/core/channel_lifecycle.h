#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

namespace ruvia {

#if defined(_MSC_VER)
#pragma warning(push)
// Cache-line isolation deliberately pads the lifecycle and its producer state.
#pragma warning(disable : 4324)
#endif

// One producer/consumer retirement handshake, independent of data capacity.
// request_stop and peer observations may cross threads. try_admit/close and
// every admission lease retirement are producer-affine; finalize is
// consumer-affine.
// Admission races stop at its stop observation: admitted operations still finish.
// close stops new admissions and publishes producer_closed only after existing
// admissions retire. A reservation must retain admission through commit/cancel;
// an asynchronous producer operation retains it through its final completion.
// Before finalize the consumer must observe producer_closed, drain descriptors,
// return all borrowed buffers, and join I/O. The owner additionally checks its
// queues and pool credits before destroying their storage. Notifications remain
// an independent capability; this state never uses a data slot to publish ACK.
class spsc_channel_lifecycle final {
public:
    class admission_lease final {
    public:
        admission_lease(const admission_lease&) = delete;
        admission_lease& operator=(const admission_lease&) = delete;
        admission_lease(admission_lease&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        admission_lease& operator=(admission_lease&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        ~admission_lease() {
            reset();
        }
        void reset() noexcept {
            if (auto* owner = std::exchange(owner_, nullptr)) {
                owner->retire_admission();
            }
        }

    private:
        explicit admission_lease(spsc_channel_lifecycle& owner_value) noexcept
            : owner_(&owner_value) {}
        spsc_channel_lifecycle* owner_;
        friend class spsc_channel_lifecycle;
    };

    spsc_channel_lifecycle() noexcept = default;
    spsc_channel_lifecycle(const spsc_channel_lifecycle&) = delete;
    spsc_channel_lifecycle& operator=(const spsc_channel_lifecycle&) = delete;
    spsc_channel_lifecycle(spsc_channel_lifecycle&&) = delete;
    spsc_channel_lifecycle& operator=(spsc_channel_lifecycle&&) = delete;
    ~spsc_channel_lifecycle() {
        if (producer_.admissions_ != 0 || (producer_.closing_ && !consumer_finalized())) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<admission_lease> try_admit() noexcept {
        if (stop_requested()) {
            return std::nullopt;
        }
        ++producer_.admissions_;
        return admission_lease(*this);
    }

    void request_stop() noexcept {
        stop_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_.load(std::memory_order_acquire);
    }

    void close() noexcept {
        request_stop();
        producer_.closing_ = true;
        if (producer_.admissions_ == 0) {
            producer_.closed_.store(true, std::memory_order_release);
        }
    }
    [[nodiscard]] bool producer_closed() const noexcept {
        return producer_.closed_.load(std::memory_order_acquire);
    }

    void finalize() noexcept {
        if (!producer_closed()) {
            std::terminate();
        }
        finalized_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool consumer_finalized() const noexcept {
        return finalized_.load(std::memory_order_acquire);
    }

private:
    void retire_admission() noexcept {
        if (producer_.admissions_ == 0) {
            std::terminate();
        }
        if (--producer_.admissions_ == 0 && producer_.closing_) {
            producer_.closed_.store(true, std::memory_order_release);
        }
    }

    struct alignas(64) producer_state final {
        std::atomic<bool> closed_{false};
        std::size_t admissions_{0};
        bool closing_{false};
    };

    alignas(64) std::atomic<bool> stop_{false};
    producer_state producer_;
    alignas(64) std::atomic<bool> finalized_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace ruvia
