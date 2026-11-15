#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "byte_view.h"

namespace ruvia {

// 交给一次出站写操作的字节。所有消费者在返回冷 task 之前取得所有权：
// 借用视图在调用处复制一次（临时值与任意生命周期均安全）；
// 精确的 std::string 右值直接移动（零拷贝接管）。
class outbound_bytes final {
public:
    outbound_bytes() noexcept = default;

    template <std::same_as<std::string> string_type>
    outbound_bytes(string_type&& owned) noexcept :
        value_(std::in_place_type<std::string>, std::move(owned)) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> && !std::same_as<source_type, std::string> && std::convertible_to<source_type&&, std::string_view>)
    outbound_bytes(source_type&& text) noexcept :
        value_(std::in_place_type<std::string_view>, static_cast<std::string_view>(std::forward<source_type>(text))) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> && !std::convertible_to<source_type&&, std::string_view> && std::convertible_to<source_type&&, std::span<const std::byte>>)
    outbound_bytes(source_type&& bytes) noexcept :
        value_(std::in_place_type<std::string_view>, as_chars(std::span<const std::byte>(std::forward<source_type>(bytes)))) {}

    outbound_bytes(const outbound_bytes&) = delete;
    outbound_bytes(outbound_bytes&&) noexcept = default;
    outbound_bytes& operator=(const outbound_bytes&) = delete;
    outbound_bytes& operator=(outbound_bytes&&) noexcept = default;
    ~outbound_bytes() = default;

    // true 表示持有调用方移交的 std::string。
    [[nodiscard]] bool owns() const noexcept {
        return std::holds_alternative<std::string>(value_);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* owned = std::get_if<std::string>(&value_)) {
            return *owned;
        }
        return std::get<std::string_view>(value_);
    }
    std::string_view view() const&& = delete;

    // 持有：移动；借用：复制一次。
    [[nodiscard]] std::string into_string() && {
        if (auto* owned = std::get_if<std::string>(&value_)) {
            return std::move(*owned);
        }
        return std::string(std::get<std::string_view>(value_));
    }

private:
    std::variant<std::string_view, std::string> value_;
};

}  // namespace ruvia
