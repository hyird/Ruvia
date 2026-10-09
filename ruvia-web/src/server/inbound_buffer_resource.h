#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <new>
#include <stdexcept>

namespace ruvia::detail {

class inbound_buffer_limit_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "inbound buffer memory budget exhausted";
    }
};

// Worker-affine live-allocation accounting. Chaining a connection resource to
// the worker resource enforces both limits before allocation. Pool-cached memory
// is excluded, while container capacity and transient reallocation overlap count.
// The upstream resource and this owner must outlive all allocations.
class inbound_buffer_resource final : public std::pmr::memory_resource {
public:
    inbound_buffer_resource(std::pmr::memory_resource* upstream, std::size_t limit)
        : upstream_(upstream),
          limit_(limit) {
        if (upstream_ == nullptr || limit_ == 0) {
            throw std::invalid_argument("inbound buffer resource requires an upstream and a positive limit");
        }
    }
    ~inbound_buffer_resource() override {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_resource(const inbound_buffer_resource&) = delete;
    inbound_buffer_resource& operator=(const inbound_buffer_resource&) = delete;

    [[nodiscard]] std::size_t used() const noexcept {
        return used_;
    }
    [[nodiscard]] std::size_t limit() const noexcept {
        return limit_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value > limit_ - used_) {
            throw inbound_buffer_limit_error();
        }
        auto* allocation = upstream_->allocate(bytes_value, alignment);
        used_ += bytes_value;
        return allocation;
    }
    void do_deallocate(void* allocation, std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value > used_) {
            std::terminate();
        }
        upstream_->deallocate(allocation, bytes_value, alignment);
        used_ -= bytes_value;
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::pmr::memory_resource* upstream_;
    const std::size_t limit_;
    std::size_t used_{};
};

}  // namespace ruvia::detail
