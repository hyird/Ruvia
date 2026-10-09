#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>

namespace ruvia::test {
class header_memory final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{0};
    std::size_t allocations_{0};
    bool reject_{false};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++allocations_;
        return result_value;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace ruvia::test
