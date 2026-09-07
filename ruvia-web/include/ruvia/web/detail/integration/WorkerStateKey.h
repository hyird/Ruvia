#pragma once

namespace ruvia::detail {

// One inline tag per T gives a process-wide unique key that links the App
// registration to the Context accessor across translation units.
template <typename T>
inline constexpr char workerStateTypeTag = 0;

template <typename T>
[[nodiscard]] const void* workerStateTypeKey() noexcept {
    return &workerStateTypeTag<T>;
}

}  // namespace ruvia::detail
