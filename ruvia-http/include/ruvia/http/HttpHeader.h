#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/Attributes.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {
struct HttpHeaderAccess;
}

namespace ruvia {

inline constexpr std::size_t kMaxHttpHeaderFields = 64;

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class HttpHeader final {
public:
    HttpHeader(const HttpHeader& other)
        : HttpHeader(other.name(), other.value(), std::pmr::get_default_resource()) {}

    HttpHeader(HttpHeader&& other) noexcept
        : resource_(other.resource_),
          bytes_(std::exchange(other.bytes_, nullptr)),
          name_size_(std::exchange(other.name_size_, 0)),
          value_size_(std::exchange(other.value_size_, 0)) {}

    HttpHeader& operator=(const HttpHeader& other) {
        if (this != &other) {
            HttpHeader replacement(other.name(), other.value(), resource_);
            swap_storage(replacement);
        }
        return *this;
    }

    HttpHeader& operator=(HttpHeader&& other) {
        if (this != &other) {
            if (resource_->is_equal(*other.resource_)) {
                release();
                bytes_ = std::exchange(other.bytes_, nullptr);
                name_size_ = std::exchange(other.name_size_, 0);
                value_size_ = std::exchange(other.value_size_, 0);
            } else {
                HttpHeader replacement(other.name(), other.value(), resource_);
                swap_storage(replacement);
                other.release();
            }
        }
        return *this;
    }

    ~HttpHeader() noexcept {
        release();
    }

    [[nodiscard]] static HttpHeader copyOf(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource) {
        return HttpHeader(name, value,
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
    friend struct detail::HttpHeaderAccess;
    HttpHeader(std::string_view name, std::string_view value,
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

    void swap_storage(HttpHeader& other) noexcept {
        std::swap(bytes_, other.bytes_);
        std::swap(name_size_, other.name_size_);
        std::swap(value_size_, other.value_size_);
    }

    std::pmr::memory_resource* resource_;
    char* bytes_{nullptr};
    std::size_t name_size_{};
    std::size_t value_size_{};
};

class HttpHeaderView final {
public:
    constexpr HttpHeaderView() noexcept = default;

    constexpr HttpHeaderView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename Name, typename Value>
        requires(detail::HttpTemporaryOwningCharString<Name> ||
                    detail::HttpTemporaryOwningCharString<Value>)
    HttpHeaderView(Name&& name, Value&& value) = delete;

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

[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
