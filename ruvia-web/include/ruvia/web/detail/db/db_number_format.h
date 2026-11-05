#pragma once

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

inline void append_db_number(std::pmr::string& output, double value) {
    // std::to_chars renders inf/nan as the literal words "inf"/"nan", which are
    // not valid SQL numeric literals and would be spliced unquoted into the
    // statement. Reject them up front with a clear error instead of letting the
    // server fail on malformed SQL.
    http_append_formatted_finite_number(output, value, "database double value must be finite",
        "database double value cannot be formatted");
}

}  // namespace ruvia::detail
