#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

// Zero-copy text held by a configuration or message value that outlives the
// call which set it -- cookie attributes, SSE fields, security header policies,
// WebSocket subprotocols, Redis SCAN patterns, outbound request lines.
//
// It is a std::string_view that refuses to be built from an owning temporary.
// Those values are commonly stored and read back later, so binding one to a
// std::string rvalue would leave the stored view dangling at the point of use
// rather than at the point of the mistake. String literals, string_view values,
// and owning-string lvalues all remain valid inputs.
class BorrowedText final {
public:
    constexpr BorrowedText() noexcept = default;

    constexpr BorrowedText(std::string_view value) noexcept
        : value_(value) {}

    constexpr BorrowedText(const char* value) noexcept
        : value_(detail::httpBorrowedCStringView(value)) {}

    template <typename Traits, typename Allocator>
    constexpr BorrowedText(const std::basic_string<char, Traits, Allocator>& value) noexcept
        : value_(value) {}

    template <detail::HttpTemporaryOwningCharString String>
    BorrowedText(String&&) = delete;

    constexpr BorrowedText& operator=(std::string_view value) noexcept {
        value_ = value;
        return *this;
    }

    constexpr BorrowedText& operator=(const char* value) noexcept {
        value_ = detail::httpBorrowedCStringView(value);
        return *this;
    }

    template <typename Traits, typename Allocator>
    constexpr BorrowedText& operator=(
        const std::basic_string<char, Traits, Allocator>& value) noexcept {
        value_ = std::string_view(value);
        return *this;
    }

    template <detail::HttpTemporaryOwningCharString String>
    BorrowedText& operator=(String&&) = delete;

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

    friend constexpr bool operator==(BorrowedText left, BorrowedText right) noexcept {
        return left.value_ == right.value_;
    }

    friend constexpr bool operator==(BorrowedText left, std::string_view right) noexcept {
        return left.value_ == right;
    }

    friend constexpr bool operator==(BorrowedText left, const char* right) noexcept {
        return left.value_ == detail::httpBorrowedCStringView(right);
    }

    template <typename Traits, typename Allocator>
    friend constexpr bool operator==(
        BorrowedText left, const std::basic_string<char, Traits, Allocator>& right) noexcept {
        return left.value_ == std::string_view(right);
    }

private:
    std::string_view value_;
};

static_assert(sizeof(BorrowedText) == sizeof(std::string_view));

}  // namespace ruvia
