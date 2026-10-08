#include <array>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/mpsc_ring_queue.h"
#include "ruvia/core/worker_cancellation.h"

#include "test_harness.h"

namespace {

struct release_observation final {
    bool destroyed_{false};
    bool reentered_{false};
    bool idle_after_destruction_{false};
    bool ran_{false};
};

struct reentrant_payload final {
    ruvia::detail::WorkerDispatcher& dispatcher_;
    release_observation& observation_;

    ~reentrant_payload() noexcept {
        observation_.destroyed_ = true;
        // Outside a running owner this query acquires the dispatcher mutex,
        // proving payload destruction is not occurring under that mutex.
        observation_.reentered_ = !dispatcher_.isCurrent();
    }
};

ruvia::MoveOnlyFunction<void()> make_payload(
    ruvia::detail::WorkerDispatcher& dispatcher, release_observation& observation) {
    dispatcher.whenIdle([&observation] {
        observation.idle_after_destruction_ = observation.destroyed_ && observation.reentered_;
    });
    auto payload = std::make_unique<reentrant_payload>(dispatcher, observation);
    return [payload = std::move(payload), &observation] { observation.ran_ = true; };
}

}  // namespace

RUVIA_TEST(mpsc_ring_queue_delivers_each_producer_in_order_across_wraparound) {
    struct item final {
        std::size_t producer{};
        std::size_t sequence{};
    };
    constexpr std::size_t producers = 4;
    constexpr std::size_t count = 2000;
    ruvia::mpsc_ring_queue<item> queue(7);
    std::atomic<bool> stop{};
    std::vector<std::jthread> threads;
    for (std::size_t producer = 0; producer < producers; ++producer) {
        threads.emplace_back([&, producer] {
            for (std::size_t sequence = 0; sequence < count; ++sequence) {
                while (!queue.try_push(item{producer, sequence})) {
                    if (stop.load()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    std::array<std::size_t, producers> expected{};
    std::size_t received{};
    bool ordered = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < producers * count && std::chrono::steady_clock::now() < deadline) {
        item value;
        if (queue.try_pop(value)) {
            if (value.producer >= producers || value.sequence != expected[value.producer]++) {
                ordered = false;
                break;
            }
            ++received;
        } else {
            std::this_thread::yield();
        }
    }
    stop.store(true);
    threads.clear();
    RUVIA_CHECK(ordered);
    RUVIA_CHECK_EQ(received, producers * count);
}

RUVIA_TEST(mpsc_ring_queue_preserves_full_input_and_recovers_failed_construction) {
    ruvia::mpsc_ring_queue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(3);
    auto second = std::make_unique<int>(7);
    RUVIA_CHECK(queue.try_push(std::move(first)));
    RUVIA_CHECK(!queue.try_push(std::move(second)));
    RUVIA_CHECK(second && *second == 7);
    RUVIA_CHECK(queue.try_pop(first));
    RUVIA_CHECK_EQ(*first, 3);
    RUVIA_CHECK(queue.try_push(std::move(second)));
    RUVIA_CHECK(queue.try_pop(first));
    RUVIA_CHECK_EQ(*first, 7);

    struct value final {
        explicit value(bool fail) {
            if (fail) {
                throw std::runtime_error("construction failed");
            }
        }
        value(value&&) = default;
    };
    ruvia::mpsc_ring_queue<value> throwing(1);
    bool failed = false;
    try {
        (void)throwing.lock().try_emplace(true);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    RUVIA_CHECK(failed);
    auto access = throwing.lock();
    RUVIA_CHECK(access.try_emplace(false));
    RUVIA_CHECK(access.front() != nullptr);
    access.pop();
    RUVIA_CHECK(access.front() == nullptr);
}

RUVIA_TEST(worker_cancellation_registration_retires_before_slot_reuse) {
    struct owner final {
        std::uint64_t identity{};
        unsigned accepted{};
        unsigned stale{};
        void cancelOperationById(std::uint64_t id) noexcept {
            if (id == identity) {
                ++accepted;
            } else {
                ++stale;
            }
        }
    } state;
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    using registration = ruvia::worker_cancellation_registration<ruvia::worker_cancellation_target<owner>>;
    std::optional<registration> current;
    ruvia::StopSource old_stop;
    ruvia::StopSource new_stop;
    std::uint64_t old_id{};
    bool identity_cleared{};
    RUVIA_CHECK(runtime.handle().post([&] {
                                    current.emplace(target, state.identity);
                                    old_id = current->id();
                                    current->arm(old_stop.token());
                                    // Foreign cancellation is queued behind this owner turn.
                                    std::jthread requester([&] { old_stop.requestStop(); });
                                    requester.join();
                                    current.reset();
                                    identity_cleared = state.identity == 0;
                                    current.emplace(target, state.identity);
                                    current->arm(new_stop.token());
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(identity_cleared);
    RUVIA_CHECK(state.identity != 0 && state.identity != old_id);
    RUVIA_CHECK_EQ(state.accepted, 0u);
    RUVIA_CHECK_EQ(state.stale, 1u);
    RUVIA_CHECK(runtime.handle().post([&] {
                                    new_stop.requestStop();
                                    current.reset();
                                    target->detach(state);
                                })
            .accepted());
    context.restart();
    runtime.run();
    RUVIA_CHECK_EQ(state.accepted, 1u);
    RUVIA_CHECK_EQ(state.identity, std::uint64_t{0});
}

RUVIA_TEST(worker_cancellation_registration_handles_precancel_and_exception_unwind) {
    struct owner final {
        std::uint64_t identity{};
        unsigned cancelled{};
        void cancelOperationById(std::uint64_t id) noexcept {
            if (id == identity) {
                ++cancelled;
            }
        }
    } state;
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    ruvia::StopSource source;
    source.requestStop();
    bool unwound = false;
    RUVIA_CHECK(runtime.handle().post([&] {
                                    try {
                                        ruvia::worker_cancellation_registration registration(target, state.identity);
                                        registration.arm(source.token());
                                        throw std::runtime_error("operation failed");
                                    } catch (const std::runtime_error&) {
                                        unwound = state.identity == 0;
                                    }
                                    target->detach(state);
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(unwound);
    RUVIA_CHECK_EQ(state.cancelled, 1u);
    RUVIA_CHECK_EQ(target.use_count(), 1L);
}

RUVIA_TEST(mpsc_ring_queue_releases_queued_values_and_pmr_storage) {
    struct resource final : std::pmr::memory_resource {
        std::size_t outstanding{};
        void* do_allocate(std::size_t bytes, std::size_t alignment) override {
            auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
            outstanding += bytes;
            return result;
        }
        void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
            outstanding -= bytes;
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    } memory;
    struct payload final {
        unsigned& destroyed;
        ~payload() {
            ++destroyed;
        }
    };
    unsigned destroyed{};
    {
        ruvia::mpsc_ring_queue<std::unique_ptr<payload>> queue(3, &memory);
        RUVIA_CHECK(memory.outstanding != 0);
        RUVIA_CHECK(queue.try_push(std::make_unique<payload>(destroyed)));
        RUVIA_CHECK_EQ(destroyed, 0u);
    }
    RUVIA_CHECK_EQ(destroyed, 1u);
    RUVIA_CHECK_EQ(memory.outstanding, std::size_t{0});
}

RUVIA_TEST(worker_queue_abandoned_payload_reenters_before_idle_completion) {
    // Queued work and an unpublished factory result use the same retirement
    // sequence, despite claiming their releasing nodes at different boundaries.
    for (const bool detach_in_factory : {false, true}) {
        asio::io_context context;
        auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
        release_observation observation;
        const auto status = dispatcher->postFactory([&] {
            auto payload = make_payload(*dispatcher, observation);
            if (detach_in_factory) {
                dispatcher->detachContext();
            }
            return payload;
        });
        RUVIA_CHECK(status == ruvia::PostStatus::kAccepted);
        if (!detach_in_factory) {
            dispatcher->detachContext();
        }
        dispatcher->waitForReservations();
        context.run();
        RUVIA_CHECK(!observation.ran_);
        RUVIA_CHECK(observation.destroyed_);
        RUVIA_CHECK(observation.reentered_);
        RUVIA_CHECK(observation.idle_after_destruction_);
    }
}

RUVIA_TEST(worker_queue_failed_publication_recovers_capacity_and_destroys_payload_before_idle) {
    asio::io_context context;
    // Without a shared owner publication cannot acquire the drain's lifetime.
    // Repeated attempts must recover the single reservation rather than fill it.
    ruvia::detail::WorkerDispatcher dispatcher(context, 1);
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        release_observation observation;
        bool failed = false;
        try {
            (void)dispatcher.postFactory([&] { return make_payload(dispatcher, observation); });
        } catch (const std::bad_weak_ptr&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        dispatcher.waitForReservations();
        RUVIA_CHECK(!observation.ran_);
        RUVIA_CHECK(observation.destroyed_);
        RUVIA_CHECK(observation.reentered_);
        RUVIA_CHECK(observation.idle_after_destruction_);
    }
    dispatcher.detachContext();
}
