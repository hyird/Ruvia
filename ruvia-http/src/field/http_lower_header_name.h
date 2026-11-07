#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

#include "ruvia/http/http_ascii.h"

namespace ruvia::detail {

// Borrow unchanged names; request writable storage only at the first uppercase
// byte. The provider must keep the input alive and return name.size() bytes.
template <typename prepare_type>
[[nodiscard]] inline std::string_view http_lower_header_name(
    std::string_view name, const prepare_type& prepare) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        auto* target = prepare(name.size());
        if (i != 0) {
            std::memcpy(target, name.data(), i);
        }
        for (std::size_t index = i; index < name.size(); ++index) {
            target[index] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(name[index])));
        }
        return std::string_view(target, name.size());
    }
    return name;
}

}  // namespace ruvia::detail
