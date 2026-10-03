#include "ruvia/http/HttpOrigin.h"

#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

namespace ruvia {

bool is_valid_http_serialized_origin(std::string_view value) noexcept {
    return detail::isValidHttpSerializedOrigin(value);
}

}  // namespace ruvia
