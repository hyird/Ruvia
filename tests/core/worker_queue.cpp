#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/mpsc_ring_queue.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"

RUVIA_TEST(mpsc_ring_queue_delivers_each_producer_in_order_across_wraparound) {
    struct item final {
        std::size_t producer_{};
        std::size_t sequence_{};
    };
    constexpr std::size_t producers = 4;
    constexpr std::size_t count = 2000;
    ruvia::mpsc_ring_queue<item> queue(7);
    std::atomic<bool> stop{};
    std::vector<std::jthread> threads;
    for (std::size_t producer_value = 0; producer_value < producers; ++producer_value) {
        threads.emplace_back([&, producer_value] {
            for (std::size_t sequence = 0; sequence < count; ++sequence) {
                while (!queue.try_push(item{producer_value, sequence})) {
                    if (stop.load()) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    std::array<std::size_t, producers> expected{};
    std::size_t received_value{};
    bool ordered = true;
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received_value < producers * count && std::chrono::steady_clock::now() < deadline_value) {
        item value;
        if (queue.try_pop(value)) {
            if (value.producer_ >= producers || value.sequence_ != expected[value.producer_]++) {
                ordered = false;
                break;
            }
            ++received_value;
        } else {
            std::this_thread::yield();
        }
    }
    stop.store(true);
    threads.clear();
    RUVIA_CHECK(ordered);
    RUVIA_CHECK_EQ(received_value, producers * count);
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
        std::uint64_t identity_{};
        unsigned accepted_{};
        unsigned stale_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++accepted_;
            } else {
                ++stale_;
            }
        }
    } state;
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    using registration = ruvia::worker_cancellation_registration<ruvia::worker_cancellation_target<owner>>;
    std::optional<registration> current;
    ruvia::stop_source old_stop;
    ruvia::stop_source new_stop;
    std::uint64_t old_id{};
    bool identity_cleared{};
    RUVIA_CHECK(runtime.handle().post([&] {
                                    current.emplace(target, state.identity_);
                                    old_id = current->id();
                                    current->arm(old_stop.token());
                                    // Foreign cancellation is queued behind this owner turn.
                                    std::jthread requester([&] { old_stop.request_stop(); });
                                    requester.join();
                                    current.reset();
                                    identity_cleared = state.identity_ == 0;
                                    current.emplace(target, state.identity_);
                                    current->arm(new_stop.token());
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(identity_cleared);
    RUVIA_CHECK(state.identity_ != 0 && state.identity_ != old_id);
    RUVIA_CHECK_EQ(state.accepted_, 0u);
    RUVIA_CHECK_EQ(state.stale_, 1u);
    RUVIA_CHECK(runtime.handle().post([&] {
                                    new_stop.request_stop();
                                    current.reset();
                                    target->detach(state);
                                })
            .accepted());
    context.restart();
    runtime.run();
    RUVIA_CHECK_EQ(state.accepted_, 1u);
    RUVIA_CHECK_EQ(state.identity_, std::uint64_t{0});
}

RUVIA_TEST(worker_cancellation_registration_handles_precancel_and_exception_unwind) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned cancelled_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++cancelled_;
            }
        }
    } state;
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 1);
    auto target = ruvia::make_worker_cancellation_target(state, runtime.handle());
    ruvia::stop_source source;
    source.request_stop();
    bool unwound = false;
    RUVIA_CHECK(runtime.handle().post([&] {
                                    try {
                                        ruvia::worker_cancellation_registration registration(target, state.identity_);
                                        registration.arm(source.token());
                                        throw std::runtime_error("operation failed");
                                    } catch (const std::runtime_error&) {
                                        unwound = state.identity_ == 0;
                                    }
                                    target->detach(state);
                                })
            .accepted());
    runtime.run();
    RUVIA_CHECK(unwound);
    RUVIA_CHECK_EQ(state.cancelled_, 1u);
}

RUVIA_TEST(mpsc_ring_queue_releases_queued_values_and_pmr_storage) {
    struct resource final : std::pmr::memory_resource {
        std::size_t outstanding_{};
        void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
            auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
            outstanding_ += bytes_value;
            return result_value;
        }
        void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
            outstanding_ -= bytes_value;
            std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    } memory;
    struct payload final {
        unsigned& destroyed_;
        ~payload() {
            ++destroyed_;
        }
    };
    unsigned destroyed{};
    {
        ruvia::mpsc_ring_queue<std::unique_ptr<payload>> queue(3, &memory);
        RUVIA_CHECK(memory.outstanding_ != 0);
        RUVIA_CHECK(queue.try_push(std::make_unique<payload>(destroyed)));
        RUVIA_CHECK_EQ(destroyed, 0u);
    }
    RUVIA_CHECK_EQ(destroyed, 1u);
    RUVIA_CHECK_EQ(memory.outstanding_, std::size_t{0});
}
