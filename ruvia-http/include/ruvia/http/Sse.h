#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/BorrowedView.h"

namespace ruvia {

struct SseMessage final {
    // SSE messages may be retained before they are formatted. Keep their text
    // zero-copy while preventing a temporary owning string from leaving an
    // already-dangling view in the saved message.
    class BorrowedText final {
    public:
        constexpr BorrowedText() noexcept = default;

        constexpr BorrowedText(std::string_view value) noexcept
            : value_(value) {}

        constexpr BorrowedText(const char* value) noexcept
            : value_(value) {}

        template <typename Traits, typename Allocator>
        constexpr BorrowedText(
            const std::basic_string<char, Traits, Allocator>& value) noexcept
            : value_(value.data(), value.size()) {}

        template <detail::HttpTemporaryOwningCharString String>
        BorrowedText(String&&) = delete;

        constexpr BorrowedText& operator=(std::string_view value) noexcept {
            value_ = value;
            return *this;
        }

        constexpr BorrowedText& operator=(const char* value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <typename Traits, typename Allocator>
        constexpr BorrowedText& operator=(
            const std::basic_string<char, Traits, Allocator>& value) noexcept {
            value_ = std::string_view(value.data(), value.size());
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

        [[nodiscard]] constexpr bool empty() const noexcept {
            return value_.empty();
        }

        [[nodiscard]] constexpr const char* data() const noexcept {
            return value_.data();
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return value_.size();
        }

        [[nodiscard]] constexpr std::size_t find(
            char value,
            std::size_t offset = 0) const noexcept {
            return value_.find(value, offset);
        }

        [[nodiscard]] constexpr std::size_t find_first_of(
            std::string_view values,
            std::size_t offset = 0) const noexcept {
            return value_.find_first_of(values, offset);
        }

        friend constexpr bool operator==(
            BorrowedText left,
            BorrowedText right) noexcept {
            return left.value_ == right.value_;
        }

        friend constexpr bool operator==(
            BorrowedText left,
            std::string_view right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(
            std::string_view left,
            BorrowedText right) noexcept {
            return left == right.value_;
        }

        friend constexpr bool operator==(
            BorrowedText left,
            const char* right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(
            const char* left,
            BorrowedText right) noexcept {
            return left == right.value_;
        }

        template <typename Traits, typename Allocator>
        friend constexpr bool operator==(
            BorrowedText left,
            const std::basic_string<char, Traits, Allocator>& right) noexcept {
            return left.value_ == std::string_view(right.data(), right.size());
        }

        template <typename Traits, typename Allocator>
        friend constexpr bool operator==(
            const std::basic_string<char, Traits, Allocator>& left,
            BorrowedText right) noexcept {
            return std::string_view(left.data(), left.size()) == right.value_;
        }

    private:
        std::string_view value_;
    };

    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<BorrowedText> data;
    BorrowedText event;
    std::optional<BorrowedText> id;
    std::optional<std::uint32_t> retry;
};

static_assert(sizeof(SseMessage::BorrowedText) == sizeof(std::string_view));

namespace detail {
void formatSseMessage(std::pmr::string& frame, const SseMessage& message);
}  // namespace detail

}  // namespace ruvia
