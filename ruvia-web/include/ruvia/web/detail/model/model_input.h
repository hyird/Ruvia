#pragma once

#include <functional>
#include <string_view>
#include <variant>

#include "ruvia/web/model.h"

namespace ruvia {
class request_name_value_list;
}  // namespace ruvia

namespace ruvia::detail {

// application/x-www-form-urlencoded body whose names and values are still
// percent-encoded.
struct form_body_input final {
    std::string_view body_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

// Borrowed input for form-style schema materialization: an encoded body, or
// request fields (query, path, headers, cookies) whose values are already
// decoded. Never a dynamic public model.
using model_input = std::variant<form_body_input, std::reference_wrapper<const request_name_value_list>>;

}  // namespace ruvia::detail
