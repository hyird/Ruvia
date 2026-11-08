#pragma once

#include <concepts>
#include <cstdint>
#include <string>

#include "ruvia/http/http_number_format.h"

namespace ruvia::detail {

inline void append_db_number(std::pmr::string& output, std::int64_t value) {
    http_append_signed(output, value);
}

inline void append_db_number(std::pmr::string& output, std::uint64_t value) {
    http_append_unsigned(output, value);
}

template <std::floating_point number_type>
inline void append_db_number(std::pmr::string& output, number_type value) {
    // SQL numeric literals cannot contain the inf/nan spellings of to_chars.
    http_append_formatted_finite_number(output, value, "database floating-point value must be finite",
        "database floating-point value cannot be formatted");
}

}  // namespace ruvia::detail
