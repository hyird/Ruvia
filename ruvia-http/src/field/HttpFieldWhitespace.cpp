#include "ruvia/http/HttpFieldWhitespace.h"

#include "ruvia/http/detail/util/HttpOws.h"

namespace ruvia {

std::string_view httpTrimOws(BorrowedText value) noexcept {
    return detail::httpTrimOws(value.view());
}

}  // namespace ruvia
