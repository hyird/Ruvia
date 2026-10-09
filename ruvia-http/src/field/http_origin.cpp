#include "ruvia/http/http_origin.h"

#include "parser/http_serialized_origin.h"

namespace ruvia {

bool is_valid_http_serialized_origin(std::string_view value) noexcept {
    return detail::is_valid_http_serialized_origin(value);
}

}  // namespace ruvia
