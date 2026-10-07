#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "ruvia/core/spsc_ring_queue.h"

namespace ruvia {

#if defined(_MSC_VER)
#pragma warning(push)
// Cache-line isolation deliberately pads the lifecycle and its producer state.
#pragma warning(disable : 4324)
#endif

// One producer/consumer retirement handshake, independent of data capacity.
// request_stop and peer observations may cross threads in cross_thread mode.
// try_admit/close and every admission lease retirement are producer-affine;
// finalize is consumer-affine. Local mode confines all operations to one thread.
// Admission races stop at its stop observation: admitted operations still finish.
// close stops new admissions and publishes producer_closed only after existing
// admissions retire. A reservation must retain admission through commit/cancel;
// an asynchronous producer operation retains it through its final completion.
// Before finalize the consumer must observe producer_closed, drain descriptors,
// return all borrowed buffers, and join I/O. The owner additionally checks its
// queues and pool credits before destroying their storage. Notifications remain
// an independent capability; this state never uses a data slot to publish ACK.
template <ring_synchronization synchronization>
class basic_channel_lifecycle final {
    using flag = std::conditional_t<synchronization == ring_synchronization::cross_thread, std::atomic<bool>, bool>;

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
        explicit admission_lease(basic_channel_lifecycle& owner) noexcept
            : owner_(&owner) {}
        basic_channel_lifecycle* owner_;
        friend class basic_channel_lifecycle;
    };

    basic_channel_lifecycle() noexcept = default;
    basic_channel_lifecycle(const basic_channel_lifecycle&) = delete;
    basic_channel_lifecycle& operator=(const basic_channel_lifecycle&) = delete;
    basic_channel_lifecycle(basic_channel_lifecycle&&) = delete;
    basic_channel_lifecycle& operator=(basic_channel_lifecycle&&) = delete;
    ~basic_channel_lifecycle() {
        if (producer_.admissions != 0 || (producer_.closing && !consumer_finalized())) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<admission_lease> try_admit() noexcept {
        if (stop_requested()) {
            return std::nullopt;
        }
        ++producer_.admissions;
        return admission_lease(*this);
    }

    void request_stop() noexcept {
        publish(stop_, true);
    }
    [[nodiscard]] bool stop_requested() const noexcept {
        return observe(stop_);
    }

    void close() noexcept {
        request_stop();
        producer_.closing = true;
        if (producer_.admissions == 0) {
            publish(producer_.closed, true);
        }
    }
    [[nodiscard]] bool producer_closed() const noexcept {
        return observe(producer_.closed);
    }

    void finalize() noexcept {
        if (!producer_closed()) {
            std::terminate();
        }
        publish(finalized_, true);
    }
    [[nodiscard]] bool consumer_finalized() const noexcept {
        return observe(finalized_);
    }

private:
    [[nodiscard]] static bool observe(const flag& value) noexcept {
        if constexpr (synchronization == ring_synchronization::cross_thread) {
            return value.load(std::memory_order_acquire);
        } else {
            return value;
        }
    }
    static void publish(flag& target, bool value) noexcept {
        if constexpr (synchronization == ring_synchronization::cross_thread) {
            target.store(value, std::memory_order_release);
        } else {
            target = value;
        }
    }
    void retire_admission() noexcept {
        if (producer_.admissions == 0) {
            std::terminate();
        }
        if (--producer_.admissions == 0 && producer_.closing) {
            publish(producer_.closed, true);
        }
    }

    struct alignas(64) producer_state final {
        flag closed{false};
        std::size_t admissions{0};
        bool closing{false};
    };

    alignas(64) flag stop_{false};
    producer_state producer_;
    alignas(64) flag finalized_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using spsc_channel_lifecycle = basic_channel_lifecycle<ring_synchronization::cross_thread>;
using local_channel_lifecycle = basic_channel_lifecycle<ring_synchronization::local>;

}  // namespace ruvia
