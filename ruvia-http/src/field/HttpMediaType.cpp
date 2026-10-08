#include "ruvia/http/HttpMediaType.h"

#include "field/HttpMediaType.h"

namespace ruvia {

std::string_view httpMediaTypeOnly(BorrowedText value) noexcept {
    return detail::httpMediaTypeOnly(value.view());
}

bool isValidHttpContentTypeFieldValue(std::string_view value) noexcept {
    return detail::isValidHttpContentTypeFieldValue(value);
}

}  // namespace ruvia
