#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace ruvia {

enum class HttpTransferCoding : std::uint8_t { kGzip,
    kDeflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t kMaxTransferCodings = 8;

struct HttpTransferCodings final {
    explicit HttpTransferCodings(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : values(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    HttpTransferCodings(const HttpTransferCodings& other)
        : values(other.values.begin(), other.values.end(), other.values.get_allocator().resource()) {}
    HttpTransferCodings& operator=(const HttpTransferCodings& other) {
        if (this != &other) {
            values.assign(other.values.begin(), other.values.end());
        }
        return *this;
    }
    HttpTransferCodings(HttpTransferCodings&&) noexcept = default;
    HttpTransferCodings& operator=(HttpTransferCodings&&) = default;

    [[nodiscard]] bool empty() const noexcept {
        return values.empty();
    }

    std::pmr::vector<HttpTransferCoding> values;
};

}  // namespace ruvia
