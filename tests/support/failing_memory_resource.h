#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>

class failing_memory_resource final : public std::pmr::memory_resource {
public:
    void fail_after(std::size_t successful_allocations) noexcept {
        armed_ = true;
        successful_allocations_ = successful_allocations;
    }

    void allow_allocations() noexcept {
        armed_ = false;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (armed_ && successful_allocations_ == 0) {
            armed_ = false;
            throw std::bad_alloc();
        }
        if (armed_) {
            --successful_allocations_;
        }
        auto* allocation = upstream_->allocate(bytes_value, alignment);
        ++live_allocations_;
        return allocation;
    }

    void do_deallocate(void* allocation, std::size_t bytes_value, std::size_t alignment) override {
        upstream_->deallocate(allocation, bytes_value, alignment);
        --live_allocations_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_{std::pmr::get_default_resource()};
    std::size_t successful_allocations_{0};
    std::size_t live_allocations_{0};
    bool armed_{false};
};
