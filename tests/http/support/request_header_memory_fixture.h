#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>

namespace ruvia::test {
class HeaderMemory final : public std::pmr::memory_resource {
public:
    std::size_t liveBytes{0};
    std::size_t allocations{0};
    bool reject{false};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        liveBytes += bytes;
        ++allocations;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace ruvia::test
