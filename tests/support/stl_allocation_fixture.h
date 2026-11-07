#pragma once

#include <gtest/gtest.h>

namespace ruvia::test {

// MSVC 调试 STL 的空容器构造和移动都可能在 noexcept 内分配迭代器代理。
// 无法按大小或分配序号可靠地区分代理与业务存储；完整失败扫描必须在无
// 调试元数据的、一致 ABI 的构建执行，Debug 仍执行成功路径及所有权检查。
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
inline constexpr bool stl_moves_allocate_debug_metadata = true;
#else
inline constexpr bool stl_moves_allocate_debug_metadata = false;
#endif

[[nodiscard]] inline bool stl_allocation_failure_injection_enabled() {
    if constexpr (stl_moves_allocate_debug_metadata) {
        ::testing::Test::RecordProperty("stl_allocation_failure_scan",
            "omitted: noexcept debug iterator metadata; run the matching Release suite for complete coverage");
        return false;
    } else {
        return true;
    }
}

}  // namespace ruvia::test
