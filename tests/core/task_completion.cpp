#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/task.h"

namespace ruvia::test::task_completion_tests {

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
bool check_cold_frame(bool cross_thread) {
    allocation_counts counts;
    bool ran = false;
    std::optional<ruvia::task<result_type>> task_value;
    auto create = [&] {
        task_value.emplace(cold_frame<result_type>(frame_allocation(counts), ran));
    };
    if (cross_thread) {
        std::thread(create).join();
    } else {
        create();
    }
    if (ran || counts.allocations_ == counts.deallocations_) {
        return false;
    }
    if (cross_thread) {
        std::thread([&] { task_value.reset(); }).join();
    } else {
        task_value.reset();
    }
    return !ran && counts.allocations_ == counts.deallocations_;
}

}  // namespace ruvia::test::task_completion_tests

int main() {
    for (const bool cross_thread : {false, true}) {
        if (!ruvia::test::task_completion_tests::check_cold_frame<void>(cross_thread) ||
            !ruvia::test::task_completion_tests::check_cold_frame<int>(cross_thread)) {
            return 1;
        }
    }
    return 0;
}
