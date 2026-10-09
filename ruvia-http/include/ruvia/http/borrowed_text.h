#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia {

// Zero-copy text held by a configuration or message value that outlives the
// call which set it -- cookie attributes, SSE fields, security header policies,
// Redis SCAN patterns, outbound request lines.
//
// It is a std::string_view that refuses to be built from an owning temporary.
// Those values are commonly stored and read back later, so binding one to a
// std::string rvalue would leave the stored view dangling at the point of use
// rather than at the point of the mistake. String literals, string_view values,
// and owning-string lvalues all remain valid inputs.
class borrowed_text final {
public:
    constexpr borrowed_text() noexcept = default;

    constexpr borrowed_text(std::string_view value) noexcept
        : value_(value) {}

    constexpr borrowed_text(const char* value) noexcept
        : value_(detail::http_borrowed_c_string_view(value)) {}

    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text(const std::basic_string<char, traits_type, allocator_type>& value) noexcept
        : value_(value) {}

    template <detail::http_temporary_owning_char_string string>
    borrowed_text(string&&) = delete;

    constexpr borrowed_text& operator=(std::string_view value) noexcept {
        value_ = value;
        return *this;
    }

    constexpr borrowed_text& operator=(const char* value) noexcept {
        value_ = detail::http_borrowed_c_string_view(value);
        return *this;
    }

    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text& operator=(
        const std::basic_string<char, traits_type, allocator_type>& value) noexcept {
        value_ = std::string_view(value);
        return *this;
    }

    template <detail::http_temporary_owning_char_string string>
    borrowed_text& operator=(string&&) = delete;

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr const char* data() const noexcept {
        return value_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return value_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return value_.empty();
    }

    friend constexpr bool operator==(borrowed_text left, borrowed_text right) noexcept {
        return left.value_ == right.value_;
    }

    friend constexpr bool operator==(borrowed_text left, std::string_view right) noexcept {
        return left.value_ == right;
    }

    friend constexpr bool operator==(borrowed_text left, const char* right) noexcept {
        return left.value_ == detail::http_borrowed_c_string_view(right);
    }

    template <typename traits_type, typename allocator_type>
    friend constexpr bool operator==(
        borrowed_text left, const std::basic_string<char, traits_type, allocator_type>& right) noexcept {
        return left.value_ == std::string_view(right);
    }

private:
    std::string_view value_;
};

static_assert(sizeof(borrowed_text) == sizeof(std::string_view));

}  // namespace ruvia
