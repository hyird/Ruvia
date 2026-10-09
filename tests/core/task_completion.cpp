#include <concepts>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/task.h"

namespace {

struct allocation_counts final {
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

template <typename t_type>
class counting_allocator {
public:
    using value_type = t_type;

    explicit counting_allocator(allocation_counts& counts) noexcept
        : counts_(&counts) {}

    template <typename u_type>
    counting_allocator(const counting_allocator<u_type>& other) noexcept
        : counts_(other.counts()) {}

    [[nodiscard]] t_type* allocate(std::size_t count) {
        ++counts_->allocations_;
        return std::allocator<t_type>{}.allocate(count);
    }

    void deallocate(t_type* pointer, std::size_t count) noexcept {
        ++counts_->deallocations_;
        std::allocator<t_type>{}.deallocate(pointer, count);
    }

    [[nodiscard]] allocation_counts* counts() const noexcept {
        return counts_;
    }

    template <typename u_type>
    [[nodiscard]] bool operator==(const counting_allocator<u_type>& other) const noexcept {
        return counts_ == other.counts();
    }

private:
    allocation_counts* counts_;
};

struct frame_allocation final {
    explicit frame_allocation(allocation_counts& counts)
        : values_(256, 0, counting_allocator<int>(counts)) {}

    std::vector<int, counting_allocator<int>> values_;
};

ruvia::task<void> complete_void_with_frame(frame_allocation frame) {
    frame.values_.resize(256);
    co_return;
}

ruvia::task<std::unique_ptr<int>> make_value() {
    co_return std::make_unique<int>(42);
}

ruvia::task<int> fail_value() {
    throw std::runtime_error("value failure");
    co_return 0;
}

ruvia::task<void> complete_void() {
    co_return;
}

ruvia::task<void> fail_void() {
    throw std::runtime_error("void failure");
    co_return;
}

bool has_failure_message(
    const ruvia::detail::task_completion_failure& failure, std::string_view expected) {
    try {
        std::rethrow_exception(failure.exception());
    } catch (const std::runtime_error& error) {
        return error.what() == expected;
    } catch (...) {
        return false;
    }
}

template <typename t_type>
ruvia::task<t_type> finish_with_frame(frame_allocation frame, bool fail) {
    frame.values_.front() = 42;
    if (fail) {
        throw std::runtime_error("frame failure");
    }
    if constexpr (std::is_void_v<t_type>) {
        co_return;
    } else {
        co_return frame.values_.front();
    }
}

template <typename t_type>
bool check_frame_completion(bool fail) {
    allocation_counts counts;
    asio::io_context context_value(1);
    bool called = false;
    bool valid = false;
    ruvia::detail::async_start_task(
        finish_with_frame<t_type>(frame_allocation(counts), fail),
        asio::bind_executor(context_value.get_executor(),
            asio::bind_allocator(counting_allocator<std::byte>(counts),
                [&](auto result_value) {
                    called = true;
                    valid = counts.allocations_ != 0 && counts.allocations_ == counts.deallocations_;
                    if (fail) {
                        valid = valid && result_value.failure() != nullptr &&
                                has_failure_message(*result_value.failure(), "frame failure");
                    } else {
                        valid = valid && result_value.success() != nullptr;
                        if constexpr (!std::is_void_v<t_type>) {
                            if (result_value.success() != nullptr) {
                                valid = valid && std::move(*result_value.success()).take_value() == 42;
                            }
                        }
                    }
                })));
    context_value.run();
    return called && valid && counts.allocations_ == counts.deallocations_;
}

template <typename result_type>
ruvia::task<result_type> cold_frame(frame_allocation frame, bool& ran) {
    ran = true;
    if constexpr (std::is_void_v<result_type>) {
        co_return;
    } else {
        co_return static_cast<int>(frame.values_.size());
    }
}

template <typename result_type>
bool check_cold_frame() {
    allocation_counts counts;
    bool ran = false;
    {
        auto task_value = cold_frame<result_type>(frame_allocation(counts), ran);
        if (ran || counts.allocations_ == counts.deallocations_) {
            return false;
        }
    }
    return !ran && counts.allocations_ == counts.deallocations_;
}

struct throwing_result final {
    explicit throwing_result(int) {
        throw std::runtime_error("return construction failed");
    }
    throwing_result(throwing_result&&) noexcept = default;
};

ruvia::task<throwing_result> fail_return_construction(frame_allocation frame) {
    co_return static_cast<int>(frame.values_.size());
}

bool check_failed_return_construction() {
    allocation_counts counts;
    asio::io_context context_value(1);
    bool observed_value = false;
    ruvia::detail::async_start_task(fail_return_construction(frame_allocation(counts)),
        asio::bind_executor(context_value.get_executor(),
            asio::bind_allocator(counting_allocator<std::byte>(counts), [&](auto result_value) {
                observed_value = result_value.failure() != nullptr && result_value.success() == nullptr &&
                                 has_failure_message(*result_value.failure(), "return construction failed") &&
                                 counts.allocations_ == counts.deallocations_;
            })));
    context_value.run();
    return observed_value && counts.allocations_ != 0 && counts.allocations_ == counts.deallocations_;
}

struct value_deleter final {
    allocation_counts* counts_{nullptr};

    void operator()(int* value) const noexcept {
        std::destroy_at(value);
        counting_allocator<int>(*counts_).deallocate(value, 1);
    }
};

using counted_value_type = std::unique_ptr<int, value_deleter>;

ruvia::task<counted_value_type> make_retained_value(frame_allocation frame, allocation_counts& values) {
    auto* storage = counting_allocator<int>(values).allocate(1);
    std::construct_at(storage, static_cast<int>(frame.values_.size()));
    co_return counted_value_type(storage, value_deleter{&values});
}

bool check_retained_value() {
    allocation_counts frames;
    allocation_counts values;
    asio::io_context context_value(1);
    counted_value_type retained;
    bool valid = true;
    ruvia::detail::async_start_task(make_retained_value(frame_allocation(frames), values),
        asio::bind_executor(context_value.get_executor(),
            asio::bind_allocator(counting_allocator<std::byte>(frames),
                [&](auto result_value) {
                    valid = frames.allocations_ == frames.deallocations_ && result_value.success() != nullptr;
                    if (result_value.success() != nullptr) {
                        retained = std::move(*result_value.success()).take_value();
                    }
                    valid = valid && values.allocations_ == 1 && values.deallocations_ == 0;
                })));
    context_value.run();
    valid = check_frame_completion<int>(false) && valid;
    valid = valid && retained != nullptr && *retained == 256 && values.deallocations_ == 0;
    retained.reset();
    return valid && frames.allocations_ == frames.deallocations_ && values.allocations_ == values.deallocations_;
}

}  // namespace

int main() {
    asio::io_context io_context(1);
    int completed = 0;
    bool valid = true;

    ruvia::detail::async_start_task(make_value(),
        asio::bind_executor(io_context.get_executor(), [&completed, &valid](auto result_value) {
            auto* success = result_value.success();
            valid = valid && success != nullptr && result_value.failure() == nullptr;
            if (success != nullptr) {
                auto value = std::move(*success).take_value();
                valid = valid && value != nullptr && *value == 42;
            }
            ++completed;
        }));
    ruvia::detail::async_start_task(fail_value(),
        asio::bind_executor(io_context.get_executor(), [&completed, &valid](auto result_value) {
            const auto* failure = result_value.failure();
            valid = valid && result_value.success() == nullptr && failure != nullptr;
            if (failure != nullptr) {
                valid = valid && has_failure_message(*failure, "value failure");
            }
            ++completed;
        }));
    ruvia::detail::async_start_task(complete_void(),
        asio::bind_executor(io_context.get_executor(), [&completed, &valid](auto result_value) {
            valid = valid && result_value.success() != nullptr && result_value.failure() == nullptr;
            ++completed;
        }));
    ruvia::detail::async_start_task(fail_void(),
        asio::bind_executor(io_context.get_executor(), [&completed, &valid](auto result_value) {
            const auto* failure = result_value.failure();
            valid = valid && result_value.success() == nullptr && failure != nullptr;
            if (failure != nullptr) {
                valid = valid && has_failure_message(*failure, "void failure");
            }
            ++completed;
        }));

    io_context.run();

    // The task-to-Asio bridge is itself an asynchronous operation. Its state
    // and queued delivery must therefore use the completion handler's associated
    // allocator, just like the surrounding co_spawn operation does.
    allocation_counts success_counts;
    asio::io_context allocated_context(1);
    ruvia::detail::async_start_task(
        complete_void_with_frame(frame_allocation(success_counts)),
        asio::bind_executor(allocated_context.get_executor(),
            asio::bind_allocator(counting_allocator<std::byte>(success_counts),
                [&completed, &valid, &success_counts](auto result_value) {
                    const bool released = success_counts.allocations_ == success_counts.deallocations_;
                    if (!released) {
                        std::fprintf(stderr, "completion retained %zu intermediate allocations\n",
                            success_counts.allocations_ - success_counts.deallocations_);
                    }
                    valid = valid && released;
                    if (result_value.success() != nullptr) {
                        ++completed;
                    }
                })));
    valid = valid && success_counts.allocations_ != 0;
    allocated_context.run();
    valid = valid && success_counts.allocations_ == success_counts.deallocations_;

    // User completion code is allowed to propagate through io_context::run().
    // The adapter must still release its state and completed task frame while
    // that exception unwinds.
    allocation_counts throwing_counts;
    asio::io_context throwing_context(1);
    ruvia::detail::async_start_task(
        complete_void_with_frame(frame_allocation(throwing_counts)), asio::bind_executor(throwing_context.get_executor(),
                                                                         asio::bind_allocator(counting_allocator<std::byte>(throwing_counts),
                                                                             [&throwing_counts, &valid](auto) {
                                                                                 valid = valid && throwing_counts.allocations_ == throwing_counts.deallocations_;
                                                                                 throw std::runtime_error("completion failed");
                                                                             })));
    bool throwing_handler_observed = false;
    try {
        throwing_context.run();
    } catch (const std::runtime_error& error) {
        throwing_handler_observed = std::string_view(error.what()) == "completion failed";
    }
    valid = valid && throwing_handler_observed && throwing_counts.allocations_ != 0 &&
            throwing_counts.allocations_ == throwing_counts.deallocations_;

    valid = check_frame_completion<int>(false) && valid;
    valid = check_frame_completion<int>(true) && valid;
    valid = check_frame_completion<void>(true) && valid;
    valid = check_retained_value() && valid;

    valid = check_cold_frame<void>() && valid;
    valid = check_cold_frame<int>() && valid;
    valid = check_failed_return_construction() && valid;

    return valid && completed == 5 ? 0 : 1;
}
