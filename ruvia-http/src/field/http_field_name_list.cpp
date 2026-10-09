#include "ruvia/http/http_field_name_list.h"

#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_header.h"

namespace ruvia {

std::optional<std::string_view> http_field_name_list::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = http_trim_ows(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = true;
            }
            continue;
        }
        if (!is_valid_http_header_name(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

}  // namespace ruvia
