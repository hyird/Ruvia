#include "ruvia/http/http_expectations.h"

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/http_ascii.h"

#include "field/http_expect_token.h"

namespace ruvia {

void http_request_expectations::parse_field(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
        if (http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
            flags_ |= continue_value;
        } else {
            flags_ |= unsupported;
        }
        return true;
    });
}

}  // namespace ruvia
