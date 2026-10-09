#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* registration_resource() noexcept;

[[nodiscard]] inline std::pmr::memory_resource* registration_resource_or_default(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? registration_resource() : resource;
}

[[nodiscard]] inline std::string_view retain_registration_text(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto* stored =
        construct_pmr_object<std::pmr::string>(registration_resource(), text, registration_resource());
    return std::string_view(*stored);
}

}  // namespace ruvia::detail
