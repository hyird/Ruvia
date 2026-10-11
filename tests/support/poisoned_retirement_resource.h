#pragma once

#include <cstddef>
#include <cstring>
#include <exception>
#include <memory_resource>
#include <vector>

namespace ruvia::testing {

class poisoned_retirement_resource final : public std::pmr::memory_resource {
public:
    ~poisoned_retirement_resource() override {
        for (const auto& allocation : allocations_) {
            std::pmr::new_delete_resource()->deallocate(
                allocation.pointer_, allocation.bytes_, allocation.alignment_);
        }
    }

    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }

    [[nodiscard]] bool retired(const void* pointer) const noexcept {
        for (const auto& allocation : allocations_) {
            if (allocation.pointer_ == pointer) {
                return allocation.retired_;
            }
        }
        return false;
    }

private:
    struct allocation final {
        void* pointer_;
        std::size_t bytes_;
        std::size_t alignment_;
        bool retired_{false};
    };

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        try {
            allocations_.push_back({pointer, bytes, alignment});
        } catch (...) {
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
            throw;
        }
        live_bytes_ += bytes;
        return pointer;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        for (auto& allocation : allocations_) {
            if (allocation.pointer_ == pointer && !allocation.retired_) {
                if (allocation.bytes_ != bytes || allocation.alignment_ != alignment) {
                    std::terminate();
                }
                // Keep the backing allocation valid until this resource dies,
                // but expose reads from logically retired storage deterministically.
                std::memset(pointer, 0xdd, bytes);
                allocation.retired_ = true;
                live_bytes_ -= bytes;
                return;
            }
        }
        std::terminate();
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::vector<allocation> allocations_;
    std::size_t live_bytes_{};
};

}  // namespace ruvia::testing
