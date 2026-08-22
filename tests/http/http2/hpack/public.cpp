#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>

#include "ruvia/http/Hpack.h"

namespace {

class ToggleAllocationResource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept { reject_ = value; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject_) throw std::bad_alloc();
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

}  // namespace

#if !defined(_MSC_VER)
RUVIA_TEST(hpack_public_callback_exception_precedes_continuation_allocation_failure) {
    ToggleAllocationResource resource;
    ruvia::HpackDecoder decoder({.resource = &resource});

    std::string block;
    block.push_back(static_cast<char>(0x82));  // indexed :method: GET
    block.push_back(static_cast<char>(0x40));  // incremental literal, new name
    block.push_back(static_cast<char>(100));
    block.append(100, 'x');
    block.push_back(1);
    block.push_back('y');

    bool sawOriginal = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            resource.reject();
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        sawOriginal = std::string_view(error.what()) == "original callback failure";
    }
    RUVIA_CHECK(sawOriginal);
}
#endif
