#pragma once

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

#ifndef RUVIA_LIFETIMEBOUND
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(gnu::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[gnu::lifetimebound]]
#else
#define RUVIA_LIFETIMEBOUND
#endif
#else
#define RUVIA_LIFETIMEBOUND
#endif
#endif

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

// Zero-copy element span held by an option or message-view struct. Accepts
// borrowed spans and contiguous owning-container lvalues while rejecting
// temporary owning containers (std::vector, std::pmr::vector, std::array) and
// initializer_list values whose backing storage would expire before the struct
// is consumed.
template <typename element_type>
class borrowed_span final {
public:
    constexpr borrowed_span() noexcept = default;

    constexpr borrowed_span(std::span<const element_type> values) noexcept
        : values_(values) {}

    constexpr borrowed_span(std::span<element_type> values) noexcept
        : values_(values) {}

    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) &&
                std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
                std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span(range_type&& values) noexcept
        : values_(std::ranges::data(values), std::ranges::size(values)) {}

    template <detail::http_temporary_owning_range<element_type> range_type>
    borrowed_span(range_type&&) = delete;

    constexpr borrowed_span(std::initializer_list<element_type>) = delete;

    constexpr borrowed_span& operator=(std::span<const element_type> values) noexcept {
        values_ = values;
        return *this;
    }

    constexpr borrowed_span& operator=(std::span<element_type> values) noexcept {
        values_ = values;
        return *this;
    }

    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) &&
                std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
                std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span& operator=(range_type&& values) noexcept {
        values_ = std::span<const element_type>(std::ranges::data(values), std::ranges::size(values));
        return *this;
    }

    template <detail::http_temporary_owning_range<element_type> range_type>
    borrowed_span& operator=(range_type&&) = delete;

    constexpr borrowed_span& operator=(std::initializer_list<element_type>) = delete;

    [[nodiscard]] constexpr std::span<const element_type> view() const noexcept {
        return values_;
    }

    [[nodiscard]] constexpr operator std::span<const element_type>() const noexcept {
        return values_;
    }

    [[nodiscard]] constexpr const element_type* data() const noexcept {
        return values_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] constexpr auto begin() const noexcept {
        return values_.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept {
        return values_.end();
    }

    [[nodiscard]] constexpr const element_type& front() const noexcept {
        return values_.front();
    }

    [[nodiscard]] constexpr const element_type& back() const noexcept {
        return values_.back();
    }

    [[nodiscard]] constexpr const element_type& operator[](std::size_t index) const noexcept {
        return values_[index];
    }

private:
    std::span<const element_type> values_{};
};

}  // namespace ruvia
