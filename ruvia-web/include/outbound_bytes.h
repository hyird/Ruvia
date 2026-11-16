#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "borrowed_text.h"
#include "byte_view.h"

namespace ruvia {

// 构造完成即拥有全部出站字节：借用输入复制一次，临时输入同样安全；
// 精确的 std::string 右值直接移动（零拷贝接管），空输入不分配数据块。
class outbound_bytes final {
public:
    outbound_bytes() noexcept = default;

    template <std::same_as<std::string> string_type>
    outbound_bytes(string_type&& owned) noexcept :
        value_(std::move(owned)) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> && !std::same_as<source_type, std::string> && std::convertible_to<source_type&&, std::string_view>)
    outbound_bytes(source_type&& text) :
        value_(static_cast<std::string_view>(std::forward<source_type>(text))) {}

    template <typename source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, outbound_bytes> && !std::convertible_to<source_type&&, std::string_view> && std::convertible_to<source_type&&, std::span<const std::byte>>)
    outbound_bytes(source_type&& bytes) :
        value_(as_chars(std::span<const std::byte>(std::forward<source_type>(bytes)))) {}

    outbound_bytes(const outbound_bytes&) = delete;
    outbound_bytes(outbound_bytes&&) noexcept = default;
    outbound_bytes& operator=(const outbound_bytes&) = delete;
    outbound_bytes& operator=(outbound_bytes&&) noexcept = default;
    ~outbound_bytes() = default;

    [[nodiscard]] std::size_t size() const noexcept {
        return value_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    std::string_view view() const&& = delete;

    // 消费已拥有的存储，不再次复制。
    [[nodiscard]] std::string into_string() && noexcept {
        return std::move(value_);
    }

private:
    std::string value_;
};

}  // namespace ruvia
