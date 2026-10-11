#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>

namespace ruvia::test {

class rejecting_memory_resource final : public std::pmr::memory_resource {
public:
    void reject_allocations(bool value = true, std::size_t min_bytes = 0) noexcept {
        rejecting_ = value;
        min_bytes_ = min_bytes;
    }

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocation_count_;
        if (rejecting_ && bytes_value >= min_bytes_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
    std::size_t min_bytes_{0};
    std::size_t allocation_count_{0};
};

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocation_count_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        auto* const storage = upstream_->allocate(bytes_value, alignment);
        ++allocation_count_;
        ++live_allocations_;
        return storage;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes_value, alignment);
        --live_allocations_;
        ++deallocation_count_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_{std::pmr::get_default_resource()};
    std::size_t allocation_count_{0};
    std::size_t live_allocations_{0};
    std::size_t deallocation_count_{0};
};

class equivalent_tracking_resource final : public std::pmr::memory_resource {
public:
    explicit equivalent_tracking_resource(std::pmr::memory_resource& upstream)
        : upstream_(&upstream) {}

    void retire() noexcept {
        retired_ = true;
    }

    [[nodiscard]] std::size_t callbacks_after_retirement() const noexcept {
        return callbacks_after_retirement_;
    }

    void reject_allocations_after(std::size_t successful_allocations) noexcept {
        remaining_allocations_ = successful_allocations;
    }

    void allow_allocations() noexcept {
        remaining_allocations_.reset();
    }

    [[nodiscard]] std::size_t allocation_attempts() const noexcept {
        return allocation_attempts_;
    }

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        callbacks_after_retirement_ += retired_;
        ++allocation_attempts_;
        if (remaining_allocations_) {
            if (*remaining_allocations_ == 0) {
                throw std::bad_alloc();
            }
            --*remaining_allocations_;
        }
        return upstream_->allocate(size, alignment);
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        callbacks_after_retirement_ += retired_;
        upstream_->deallocate(pointer, size, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* resource = dynamic_cast<const equivalent_tracking_resource*>(&other);
        return resource != nullptr && resource->upstream_ == upstream_;
    }

    std::pmr::memory_resource* upstream_;
    bool retired_{false};
    std::size_t callbacks_after_retirement_{0};
    std::size_t allocation_attempts_{0};
    std::optional<std::size_t> remaining_allocations_;
};

class default_resource_guard final {
public:
    explicit default_resource_guard(std::pmr::memory_resource& resource) noexcept
        : previous_(std::pmr::set_default_resource(&resource)) {}

    default_resource_guard(const default_resource_guard&) = delete;
    default_resource_guard& operator=(const default_resource_guard&) = delete;

    ~default_resource_guard() {
        std::pmr::set_default_resource(previous_);
    }

private:
    std::pmr::memory_resource* previous_;
};

class tracking_resource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocated_after_release() const noexcept {
        return deallocated_after_release_;
    }

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocation_count_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        deallocated_after_release_ = deallocated_after_release_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocated_after_release_{false};
    std::size_t allocation_count_{0};
};

}  // namespace ruvia::test
