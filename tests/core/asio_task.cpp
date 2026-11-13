#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop.h"
namespace ruvia::test::asio_task_tests {

struct alignas(4096) aligned_result final {
    explicit aligned_result(int value)
        : value_(std::make_unique<int>(value)),
          addresses_aligned_(is_aligned(this)) {}
    aligned_result(aligned_result&& other) noexcept
        : value_(std::move(other.value_)),
          addresses_aligned_(other.addresses_aligned_ && is_aligned(&other) && is_aligned(this)) {}

    std::unique_ptr<int> value_;
    bool addresses_aligned_;
    // Fill the alignment boundary explicitly; C4324 is an error under MSVC /WX.
    std::array<std::byte, 4096 - sizeof(std::unique_ptr<int>) - sizeof(bool)> padding_;

private:
    static bool is_aligned(const void* value) noexcept {
        return reinterpret_cast<std::uintptr_t>(value) % alignof(aligned_result) == 0;
    }
};

ruvia::task<aligned_result> make_value(ruvia::worker_ref worker_value,
    std::unique_ptr<aligned_result> result_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    // Keep the over-aligned object outside coroutine locals, whose alignment
    // is independently broken by GCC 13/14.
    co_return std::move(*result_value);
}

ruvia::task<void> complete_void(ruvia::worker_ref worker_value, bool& completed) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    completed = true;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("root task failure");
    co_return;
}

ruvia::task<int> answer() {
    co_return 7;
}

template <typename t_type>
// Not final: Asio executors keep an allocator as an empty base.
struct counting_allocator {
    using value_type = t_type;

    explicit counting_allocator(std::size_t& allocations) noexcept
        : allocations_(&allocations) {}
    template <typename other_type>
    counting_allocator(const counting_allocator<other_type>& other) noexcept
        : allocations_(other.allocations_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) {
        ++*allocations_;
        return std::allocator<t_type>().allocate(count);
    }
    void deallocate(t_type* pointer, std::size_t count) noexcept {
        std::allocator<t_type>().deallocate(pointer, count);
    }
    template <typename other_type>
    friend bool operator==(const counting_allocator& left, const counting_allocator<other_type>& right) noexcept {
        return left.allocations_ == right.allocations_;
    }

    std::size_t* allocations_;
};

}  // namespace ruvia::test::asio_task_tests

TEST(Ruvia, AsioTaskLifecycle) {
    {
        auto ok_void = ruvia::asio_completion<void>::completed({});
        auto err_void = ruvia::asio_completion<void>::completed(std::make_error_code(std::errc::invalid_argument));
        auto ok_val = ruvia::asio_completion<int>::completed({}, 17);
        const auto& const_val = ok_val;
        EXPECT_TRUE(ok_void.succeeded());
        EXPECT_FALSE(ok_void.failed());
        EXPECT_FALSE(err_void.succeeded());
        EXPECT_TRUE(err_void.failed());
        EXPECT_TRUE(ok_val.succeeded());
        EXPECT_FALSE(ok_val.failed());
        EXPECT_EQ(const_val.result(), 17);
        EXPECT_EQ(ok_val.result(), 17);
        EXPECT_EQ(std::move(ok_val).take_result(), 17);
    }
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    bool void_completed = false;

    auto value = loop.start(ruvia::test::asio_task_tests::make_value(
        worker_value, std::make_unique<ruvia::test::asio_task_tests::aligned_result>(42)));
    auto no_value = loop.start(ruvia::test::asio_task_tests::complete_void(worker_value, void_completed));
    auto failure = loop.start(ruvia::test::asio_task_tests::fail());
    std::promise<bool> same_loop_get_rejected;
    auto same_loop_get_result = same_loop_get_rejected.get_future();
    const auto probe_posted = loop.worker().post([&] {
        try {
            static_cast<void>(std::move(value).get());
            same_loop_get_rejected.set_value(false);
        } catch (const std::logic_error&) {
            same_loop_get_rejected.set_value(value.valid());
        }
    });
    ASSERT_TRUE(probe_posted.accepted());

    loops.start();
    bool valid = false;
    std::optional<ruvia::test::asio_task_tests::aligned_result> result_value;
    try {
        const auto same_loop_guard_held = same_loop_get_result.get();
        result_value.emplace(std::move(value).get());
        std::move(no_value).get();
        bool failure_observed = false;
        try {
            std::move(failure).get();
        } catch (const std::runtime_error& error) {
            failure_observed = std::string_view(error.what()) == "root task failure";
        }
        valid = same_loop_guard_held && void_completed && failure_observed;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    valid = valid && result_value && result_value->addresses_aligned_ &&
            result_value->value_ != nullptr && *result_value->value_ == 42;
    EXPECT_TRUE(valid);
}

TEST(Ruvia, AsioTaskAbandonedFailureReachesJoin) {
    ruvia::event_loop_pool abandoned_pool({.loop_count_ = 1});
    {
        auto unobserved = abandoned_pool.loop(0).start(ruvia::test::asio_task_tests::fail());
    }
    abandoned_pool.start();
    abandoned_pool.stop();
    try {
        abandoned_pool.join();
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string_view(error.what()), "root task failure");
        return;
    }
    ADD_FAILURE() << "Expected abandoned root task failure";
}

TEST(Ruvia, AsyncStartUsesHandlerExecutorAndAllocator) {
    asio::io_context io_context;
    std::size_t allocations = 0;
    std::optional<int> delivered;
    bool failure_delivered = false;
    ruvia::async_start(ruvia::test::asio_task_tests::answer(),
        asio::bind_allocator(ruvia::test::asio_task_tests::counting_allocator<int>(allocations),
            asio::bind_executor(io_context.get_executor(),
                [&delivered](ruvia::task_completion_result<int>&& result_value) {
                    if (result_value.index() == 0) {
                        delivered = std::get<0>(result_value);
                    }
                })));
    ruvia::async_start(ruvia::test::asio_task_tests::fail(),
        asio::bind_executor(io_context.get_executor(), [&failure_delivered](std::exception_ptr failure) {
            failure_delivered = failure != nullptr;
        }));
    // Both tasks complete inline; delivery waits for the handler's executor.
    EXPECT_FALSE(delivered.has_value());
    EXPECT_FALSE(failure_delivered);
    io_context.run();
    EXPECT_EQ(delivered, 7);
    EXPECT_TRUE(failure_delivered);
    EXPECT_GT(allocations, 0U);
}

TEST(Ruvia, EventLoopTryStartReturnsRejectedTask) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    auto accepted = loop.try_start(ruvia::test::asio_task_tests::answer());
    ASSERT_TRUE(accepted.accepted());
    EXPECT_EQ(accepted.rejected(), nullptr);
    auto root = std::move(accepted).take_value();

    auto full = loop.try_start(ruvia::test::asio_task_tests::answer());
    EXPECT_EQ(full.status(), ruvia::post_status::queue_full);
    ASSERT_NE(full.rejected(), nullptr);
    EXPECT_THROW(static_cast<void>(full.value()), std::logic_error);
    auto returned = std::move(full).take_rejected();
    try {
        static_cast<void>(loop.start(std::move(returned)));
        ADD_FAILURE() << "a full queue must reject start()";
    } catch (const ruvia::event_loop_start_rejected& error) {
        EXPECT_EQ(error.status(), ruvia::post_status::queue_full);
    }

    EXPECT_FALSE(root.ready());
    EXPECT_FALSE(root.wait_for(ruvia::operation_timeout(std::chrono::steady_clock::now())));
    EXPECT_FALSE(root.wait_for(ruvia::operation_timeout(std::chrono::steady_clock::time_point{})));
    EXPECT_FALSE(root.wait_for(std::chrono::milliseconds(1)));
    EXPECT_THROW(static_cast<void>(root.wait_for(std::chrono::milliseconds(0))), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(root.wait_for(std::chrono::milliseconds(-1))), std::invalid_argument);
    loops.start();
    EXPECT_TRUE(root.wait_for(std::chrono::seconds(30)));
    EXPECT_TRUE(root.ready());
    EXPECT_EQ(std::move(root).get(), 7);
    EXPECT_FALSE(root.ready());

    loops.stop();
    loops.join();
    auto stopped = loop.try_start(ruvia::test::asio_task_tests::answer());
    EXPECT_EQ(stopped.status(), ruvia::post_status::worker_stopping);
    EXPECT_NE(stopped.rejected(), nullptr);
    try {
        static_cast<void>(loop.start(std::move(stopped).take_rejected()));
        ADD_FAILURE() << "a retired loop must reject start()";
    } catch (const ruvia::event_loop_start_rejected& error) {
        EXPECT_EQ(error.status(), ruvia::post_status::worker_stopping);
    }
}
