#include "ruvia/http/HttpCorsFields.h"

#include "ruvia/http/HttpFieldWhitespace.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

namespace ruvia {

bool isValidHttpSerializedOrigin(std::string_view value) noexcept {
    return detail::isValidHttpSerializedOrigin(value);
}

std::optional<std::string_view> HttpCorsRequestHeaderNames::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = httpTrimOws(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = sawName_;
            }
            continue;
        }
        if (!isValidHttpHeaderName(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        sawName_ = true;
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

}  // namespace ruvia
