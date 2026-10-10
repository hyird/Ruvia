#include "parser/http_chunk_parser.h"

#include <exception>
#include <limits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_chunk_framing.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

http_chunk_trailer_parse_result http_chunk_trailer_parser::fail(http_chunk_scan_error error) noexcept {
    failure_ = error;
    return http_chunk_trailer_parse_result(http_chunk_trailer_failure(error));
}

http_chunk_trailer_parse_result http_chunk_trailer_parser::next() noexcept {
    if (failure_) {
        return http_chunk_trailer_parse_result(http_chunk_trailer_failure(*failure_));
    }
    if (cursor_ == 0 && trailers_.size() > max_http_header_bytes) {
        return fail(http_chunk_scan_error::too_large);
    }
    if (cursor_ == trailers_.size()) {
        return http_chunk_trailer_parse_result(http_chunk_trailer_end());
    }
    if (field_count_ == max_http_header_fields) {
        return fail(http_chunk_scan_error::too_large);
    }
    ++field_count_;

    const auto line_end = trailers_.find("\r\n", cursor_);
    const auto line = line_end == std::string_view::npos
                          ? trailers_.substr(cursor_)
                          : trailers_.substr(cursor_, line_end - cursor_);
    const auto colon = http_token_prefix_size(line);
    if (colon == 0 || colon == line.size() || line[colon] != ':') {
        return fail(http_chunk_scan_error::invalid_trailer);
    }
    const auto name = line.substr(0, colon);
    const auto value = http_trim_ows(line.substr(colon + 1));
    // The prefix scan proved the name syntax; trimming removed boundary OWS.
    if (!is_valid_http_field_value_bytes(value) ||
        is_forbidden_http_request_trailer_name(name)) {
        return fail(http_chunk_scan_error::invalid_trailer);
    }
    cursor_ = line_end == std::string_view::npos ? trailers_.size() : line_end + 2;
    return http_chunk_trailer_parse_result(http_chunk_trailer_field(name, value));
}

std::optional<http_chunk_scan_error> validate_http_chunk_trailers(std::string_view trailers) noexcept {
    if (trailers.size() > max_http_header_bytes) {
        return http_chunk_scan_error::too_large;
    }
    http_chunk_trailer_parser parser(trailers);
    for (;;) {
        const auto result_value = parser.next();
        if (const auto* failure = result_value.failure()) {
            return failure->error();
        }
        if (result_value.end()) {
            return std::nullopt;
        }
    }
}

http_chunk_scan_result scan_http_chunked_body(std::string_view body) noexcept {
    http_chunk_framing framing({
        .body_limit_ = protocol_byte_limit::limited(default_max_buffered_body_bytes),
        .trailer_section_limit_ = protocol_byte_limit::limited(max_http_header_bytes),
        .trailer_role_ = chunk_trailer_role::request,
    });
    std::size_t consumed = 0;
    for (;;) {
        const auto result_value = framing.decode(body.substr(consumed), std::numeric_limits<std::size_t>::max());
        if (const auto* complete = std::get_if<chunk_framing_complete>(&result_value)) {
            return http_chunk_scan_result::make_complete(consumed + complete->consumed_bytes_);
        }
        if (const auto* failure = std::get_if<chunk_framing_failure>(&result_value)) {
            switch (failure->error_) {
                case chunk_framing_error::invalid_size:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::invalid_size);
                case chunk_framing_error::size_overflow:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::size_overflow);
                case chunk_framing_error::invalid_extension:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::invalid_extension);
                case chunk_framing_error::invalid_crlf:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::invalid_crlf);
                case chunk_framing_error::invalid_trailer:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::invalid_trailer);
                case chunk_framing_error::trailer_limit_exceeded:
                case chunk_framing_error::body_limit_exceeded:
                case chunk_framing_error::framing_limit_exceeded:
                    return http_chunk_scan_result::make_failure(http_chunk_scan_error::too_large);
            }
            std::terminate();
        }
        if (std::holds_alternative<chunk_framing_need_more>(result_value)) {
            return http_chunk_scan_result::make_need_more();
        }
        consumed += std::get<chunk_framing_body>(result_value).consumed_bytes_;
    }
}

}  // namespace ruvia::detail
