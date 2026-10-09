#include "ruvia/http/http_request_trailers.h"

#include <algorithm>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"

#include "parser/http_chunk_parser.h"

namespace ruvia {
http_request_trailers::http_request_trailers(std::pmr::memory_resource* resource)
    : fields_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

std::variant<std::monostate, http_request_trailer_error> http_request_trailers::append(std::string_view name, std::string_view value) {
    if (!detail::is_valid_http_header_name(name) || !std::ranges::all_of(value, [](unsigned char ch) { return detail::is_http_field_value_char(ch); }) || detail::http_trim_ows(value) != value) {
        return http_request_trailer_error::invalid_field;
    }
    if (detail::is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    if (fields_.size() == max_http_header_fields || bytes_ > max_http_header_bytes - 4 || name.size() > max_http_header_bytes - bytes_ - 4 ||
        value.size() > max_http_header_bytes - bytes_ - 4 - name.size()) {
        return http_request_trailer_error::section_too_large;
    }
    fields_.push_back(http_header::copy_of(name, value, fields_.get_allocator().resource()));
    bytes_ += name.size() + value.size() + 4;
    return {};
}

std::variant<std::monostate, http_request_trailer_error> http_request_trailers::append_http1(std::string_view block) {
    if (block.ends_with("\r\n\r\n")) {
        block.remove_suffix(4);
    } else if (block.ends_with("\r\n")) {
        block.remove_suffix(2);
    }
    detail::http_chunk_trailer_parser parser(block);
    for (;;) {
        const auto result_value = parser.next();
        if (const auto* failure = result_value.failure()) {
            return failure->error() == detail::http_chunk_scan_error::too_large ? http_request_trailer_error::section_too_large : http_request_trailer_error::invalid_field;
        }
        if (result_value.end()) {
            return {};
        }
        const auto* field = result_value.field();
        const auto appended = append(field->name(), field->value());
        if ((appended.index() != 0)) {
            return appended;
        }
    }
}

std::optional<std::string_view> http_request_trailers::field(std::string_view name) const& noexcept {
    for (auto field = fields_.rbegin(); field != fields_.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace ruvia
