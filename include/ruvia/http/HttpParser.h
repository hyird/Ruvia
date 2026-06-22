#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpParseTypes.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia {

struct HttpParseResult {
    HttpParseStatus status{HttpParseStatus::kIncomplete};
    HttpParseError error{HttpParseError::kNone};
    HttpRequest request;
    std::size_t consumedBytes{0};
};

class HttpParser final {
public:
    [[nodiscard]] HttpParseResult parse(std::string_view buffer) const noexcept;
};

}  // namespace ruvia
