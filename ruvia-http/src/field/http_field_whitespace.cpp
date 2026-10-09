#include "ruvia/http/http_field_whitespace.h"

#include "ruvia/http/detail/util/http_ows.h"

namespace ruvia {

std::string_view http_trim_ows(borrowed_text value) noexcept {
    return detail::http_trim_ows(value.view());
}

}  // namespace ruvia
