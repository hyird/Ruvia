#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ruvia {

namespace detail {

template <typename T>
inline constexpr bool kIsHttpHeaderOwningString = false;

template <typename Traits, typename Allocator>
inline constexpr bool kIsHttpHeaderOwningString<
    std::basic_string<char, Traits, Allocator>> = true;

}  // namespace detail

inline constexpr std::size_t kMaxHttpHeaderFields = 64;

class HttpHeaderView final {
public:
    constexpr HttpHeaderView() noexcept = default;

    constexpr HttpHeaderView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename Name, typename Value>
        requires(
            (detail::kIsHttpHeaderOwningString<std::remove_cvref_t<Name>> &&
             !std::is_lvalue_reference_v<Name&&>) ||
            (detail::kIsHttpHeaderOwningString<std::remove_cvref_t<Value>> &&
             !std::is_lvalue_reference_v<Value&&>))
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
