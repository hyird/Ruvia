#include "ruvia/http/HttpClientRequestTarget.h"

#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia {

bool isValidHttpClientOriginTarget(std::string_view target) noexcept {
    return detail::isValidOriginFormTarget(target);
}

}  // namespace ruvia
