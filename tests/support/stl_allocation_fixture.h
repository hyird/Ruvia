#pragma once

#include <memory_resource>

namespace ruvia::test {

// MSVC 的调试 STL 会在 noexcept 移动中分配迭代器代理。这些构建仍验证值、
// 所有权与回收，但不能向标准库的移动操作注入分配失败或要求零分配。
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
inline constexpr bool stl_moves_allocate_debug_metadata = true;
#else
inline constexpr bool stl_moves_allocate_debug_metadata = false;
#endif

}  // namespace ruvia::test
