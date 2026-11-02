#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <chrono>
#include <compare>
#include <span>
#include <string_view>

namespace ruvia::detail {

// 输入为去除两端空白后的完整 JSON number；不以机器浮点范围限制 NumericDate。
[[nodiscard]] bool jwt_is_numeric_date(std::string_view text) noexcept;

// 精确比较 date 与 now + offset，不先收窄到宿主时钟或浮点表示。
// date 必须已通过 jwt_is_numeric_date；offset 可正可负。
[[nodiscard]] std::strong_ordering jwt_compare_numeric_date(std::string_view date,
    std::chrono::system_clock::time_point now, std::chrono::seconds offset) noexcept;

// offset 为已经校验的非负签发时长；0 向下取整，正值向上取整到整秒。
// 不把相加结果收窄回宿主时钟；返回值借用 output。
[[nodiscard]] std::string_view jwt_signing_date(std::span<char, 21> output,
    std::chrono::system_clock::time_point now, std::chrono::seconds offset) noexcept;

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_JWT
