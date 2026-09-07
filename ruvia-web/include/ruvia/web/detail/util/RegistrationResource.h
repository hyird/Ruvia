#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* registrationResource() noexcept;

[[nodiscard]] inline std::pmr::memory_resource* registrationResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? registrationResource() : resource;
}

[[nodiscard]] inline std::string_view retainRegistrationText(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto* stored =
        constructPmrObject<std::pmr::string>(registrationResource(), text, registrationResource());
    return std::string_view(*stored);
}

}  // namespace ruvia::detail
