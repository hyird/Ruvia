#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/attributes.h"
#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia::detail {
struct http_header_access;
}

namespace ruvia {

inline constexpr std::size_t max_http_header_fields = 64;

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class http_header final {
public:
    http_header(const http_header& other)
        : http_header(other.name(), other.value(), std::pmr::get_default_resource()) {}

    http_header(http_header&& other) noexcept
        : resource_(other.resource_),
          bytes_(std::exchange(other.bytes_, nullptr)),
          name_size_(std::exchange(other.name_size_, 0)),
          value_size_(std::exchange(other.value_size_, 0)) {}

    http_header& operator=(const http_header& other) {
        if (this != &other) {
            http_header replacement(other.name(), other.value(), resource_);
            swap_storage(replacement);
        }
        return *this;
    }

    http_header& operator=(http_header&& other) {
        if (this != &other) {
            if (resource_->is_equal(*other.resource_)) {
                release();
                bytes_ = std::exchange(other.bytes_, nullptr);
                name_size_ = std::exchange(other.name_size_, 0);
                value_size_ = std::exchange(other.value_size_, 0);
            } else {
                http_header replacement(other.name(), other.value(), resource_);
                swap_storage(replacement);
                other.release();
            }
        }
        return *this;
    }

    ~http_header() noexcept {
        release();
    }

    [[nodiscard]] static http_header copy_of(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource) {
        return http_header(name, value,
            resource != nullptr ? resource : std::pmr::get_default_resource());
    }

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return name_size_ != 0 ? std::string_view(bytes_, name_size_) : std::string_view{};
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_size_ != 0 ? std::string_view(bytes_ + name_size_, value_size_) : std::string_view{};
    }
    std::string_view value() const&& = delete;

private:
    friend struct detail::http_header_access;
    http_header(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource)
        : resource_(resource),
          name_size_(name.size()),
          value_size_(value.size()) {
        if (value.size() > (std::numeric_limits<std::size_t>::max)() - name.size()) {
            throw std::length_error("HTTP header storage size overflows size_t");
        }
        const auto size = name.size() + value.size();
        if (size != 0) {
            bytes_ = static_cast<char*>(resource_->allocate(size, alignof(char)));
            if (!name.empty()) {
                std::memcpy(bytes_, name.data(), name.size());
            }
            if (!value.empty()) {
                std::memcpy(bytes_ + name.size(), value.data(), value.size());
            }
        }
    }

    void release() noexcept {
        if (bytes_ != nullptr) {
            resource_->deallocate(bytes_, name_size_ + value_size_, alignof(char));
        }
        bytes_ = nullptr;
        name_size_ = 0;
        value_size_ = 0;
    }

    void swap_storage(http_header& other) noexcept {
        std::swap(bytes_, other.bytes_);
        std::swap(name_size_, other.name_size_);
        std::swap(value_size_, other.value_size_);
    }

    std::pmr::memory_resource* resource_;
    char* bytes_{nullptr};
    std::size_t name_size_{};
    std::size_t value_size_{};
};

class http_header_view final {
public:
    constexpr http_header_view() noexcept = default;

    constexpr http_header_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    http_header_view(name_type&& name, value_type&& value) = delete;

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

[[nodiscard]] bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_status_text(std::string_view value) noexcept;

}  // namespace ruvia
