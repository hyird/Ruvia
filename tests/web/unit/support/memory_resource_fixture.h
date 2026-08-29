#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>

namespace ruvia::test {

class RejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value = true) noexcept {
        rejecting_ = value;
    }

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        if (rejecting_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
    std::size_t allocationCount_{0};
};

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }

    [[nodiscard]] std::size_t deallocationCount() const noexcept {
        return deallocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* const storage = upstream_->allocate(bytes, alignment);
        ++allocationCount_;
        ++liveAllocations_;
        return storage;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
        --liveAllocations_;
        ++deallocationCount_;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_{std::pmr::get_default_resource()};
    std::size_t allocationCount_{0};
    std::size_t liveAllocations_{0};
    std::size_t deallocationCount_{0};
};

class TrackingResource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocatedAfterRelease() const noexcept {
        return deallocatedAfterRelease_;
    }

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        deallocatedAfterRelease_ = deallocatedAfterRelease_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocatedAfterRelease_{false};
    std::size_t allocationCount_{0};
};

}  // namespace ruvia::test
