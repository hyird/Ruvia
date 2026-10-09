#pragma once

namespace ruvia::detail {

// One inline tag per T gives a process-wide unique key that links the application
// registration to the context accessor across translation units.
template <typename t_type>
inline constexpr char worker_state_type_tag = 0;

template <typename t_type>
[[nodiscard]] const void* worker_state_type_key() noexcept {
    return &worker_state_type_tag<t_type>;
}

}  // namespace ruvia::detail
