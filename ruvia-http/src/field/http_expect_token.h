#pragma once

#include <string_view>

namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

}  // namespace ruvia::detail
