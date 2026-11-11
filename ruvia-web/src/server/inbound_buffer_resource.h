#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

class inbound_buffer_limit_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "inbound buffer memory budget exhausted";
    }
};

// A worker's live inbound buffer bytes, shared by the inbound_buffer_resource of
// each of its connections. Worker-affine, no synchronization. This owner must
// outlive every connection resource.
class inbound_buffer_budget final {
public:
    explicit inbound_buffer_budget(std::size_t limit)
        : limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer budget requires a positive limit");
        }
    }
    ~inbound_buffer_budget() {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_budget(const inbound_buffer_budget&) = delete;
    inbound_buffer_budget& operator=(const inbound_buffer_budget&) = delete;

private:
    friend class inbound_buffer_resource;
    const std::size_t limit_;
    std::size_t used_{};
};

// One connection's inbound buffers. Each allocation is checked against the
// connection's limit and its worker's budget, both charged together, and then
// taken straight from process_resource(): one wrapper between a buffer and
// mimalloc. Counts live allocations: container capacity and transient
// reallocation overlap count. Every allocation must be returned before this
// owner is destroyed.
class inbound_buffer_resource final : public std::pmr::memory_resource {
public:
    inbound_buffer_resource(inbound_buffer_budget& worker, std::size_t limit)
        : worker_(worker),
          limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer resource requires a positive limit");
        }
    }
    ~inbound_buffer_resource() override {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_resource(const inbound_buffer_resource&) = delete;
    inbound_buffer_resource& operator=(const inbound_buffer_resource&) = delete;

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value > limit_ - used_ || bytes_value > worker_.limit_ - worker_.used_) {
            throw inbound_buffer_limit_error();
        }
        auto* allocation = process_resource()->allocate(bytes_value, alignment);
        used_ += bytes_value;
        worker_.used_ += bytes_value;
        return allocation;
    }
    void do_deallocate(void* allocation, std::size_t bytes_value, std::size_t alignment) override {
        if (bytes_value > used_ || bytes_value > worker_.used_) {
            std::terminate();
        }
        process_resource()->deallocate(allocation, bytes_value, alignment);
        used_ -= bytes_value;
        worker_.used_ -= bytes_value;
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    inbound_buffer_budget& worker_;
    const std::size_t limit_;
    std::size_t used_{};
};

}  // namespace ruvia::detail
