#include "ruvia/http/detail/parser/http_chunk_framing.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_limits.h"

#include "parser/http_chunk_parser.h"

namespace ruvia::detail {

chunk_framing_result http_chunk_framing::decode(std::string_view available, std::size_t max_body_bytes) noexcept {
    if ((state_.index() != 0)) {
        return chunk_framing_failure{0, std::get<1>(state_)};
    }

    std::size_t cursor_value = 0;
    for (;;) {
        switch (std::get<0>(state_)) {
            case progress::size_line: {
                const auto line_end = available.find("\r\n", cursor_value);
                if (line_end == std::string_view::npos) {
                    // A future CRLF can begin at most one byte before the end.
                    if (available.size() - cursor_value >= max_http_header_bytes) {
                        return fail(cursor_value, chunk_framing_error::framing_limit_exceeded);
                    }
                    return chunk_framing_need_more{cursor_value};
                }
                if (line_end - cursor_value + 2 > max_http_header_bytes) {
                    return fail(cursor_value, chunk_framing_error::framing_limit_exceeded);
                }
                std::size_t chunk_size = 0;
                switch (parse_http_chunk_size_line(available.substr(cursor_value, line_end - cursor_value), chunk_size)) {
                    case chunk_size_line_status::ok:
                        break;
                    case chunk_size_line_status::invalid_size:
                        return fail(cursor_value, chunk_framing_error::invalid_size);
                    case chunk_size_line_status::overflow:
                        return fail(cursor_value, chunk_framing_error::size_overflow);
                    case chunk_size_line_status::invalid_extension:
                        return fail(cursor_value, chunk_framing_error::invalid_extension);
                }
                cursor_value = line_end + 2;
                if (chunk_size == 0) {
                    state_ = progress::trailers;
                    trailer_search_offset_ = 0;
                } else {
                    if (config_.body_limit_.addition_exceeds(decoded_bytes_, chunk_size)) {
                        return fail(cursor_value, chunk_framing_error::body_limit_exceeded);
                    }
                    decoded_bytes_ += chunk_size;
                    remaining_ = chunk_size;
                    state_ = progress::body;
                }
                break;
            }
            case progress::body: {
                const auto bytes_value = std::min({remaining_, available.size() - cursor_value, max_body_bytes});
                if (bytes_value == 0) {
                    return chunk_framing_need_more{cursor_value};
                }
                const auto body = available.substr(cursor_value, bytes_value);
                remaining_ -= bytes_value;
                cursor_value += bytes_value;
                if (remaining_ == 0) {
                    if (available.size() - cursor_value >= 2) {
                        if (const auto error = consume_delimiter(available.substr(cursor_value))) {
                            return fail(cursor_value, *error);
                        }
                        cursor_value += 2;
                        state_ = progress::size_line;
                    } else {
                        state_ = progress::delimiter;
                    }
                }
                return chunk_framing_body{cursor_value, body};
            }
            case progress::delimiter:
                if (available.size() - cursor_value < 2) {
                    return chunk_framing_need_more{cursor_value};
                }
                if (const auto error = consume_delimiter(available.substr(cursor_value))) {
                    return fail(cursor_value, *error);
                }
                cursor_value += 2;
                state_ = progress::size_line;
                break;
            case progress::trailers: {
                const auto trailers = available.substr(cursor_value);
                if (trailers.starts_with("\r\n")) {
                    state_ = progress::complete;
                    return chunk_framing_complete{cursor_value + 2, {}};
                }
                const auto trailer_end = trailers.find("\r\n\r\n", trailer_search_offset_);
                if (trailer_end == std::string_view::npos) {
                    // Keep the final three bytes as a possible delimiter prefix.
                    if (trailers.size() >= max_http_header_bytes) {
                        return fail(cursor_value, chunk_framing_error::framing_limit_exceeded);
                    }
                    trailer_search_offset_ = trailers.size() > 3 ? trailers.size() - 3 : 0;
                    return chunk_framing_need_more{cursor_value};
                }
                const auto trailer_bytes = trailer_end + 4;
                if (config_.trailer_section_limit_.exceeds(trailer_bytes)) {
                    return fail(cursor_value, chunk_framing_error::trailer_limit_exceeded);
                }
                if (const auto error = validate_trailers(trailers.substr(0, trailer_end))) {
                    return fail(cursor_value, *error);
                }
                state_ = progress::complete;
                return chunk_framing_complete{cursor_value + trailer_bytes, trailers.substr(0, trailer_end)};
            }
            case progress::complete:
                return chunk_framing_complete{cursor_value, {}};
        }
    }
}

std::optional<chunk_framing_error> http_chunk_framing::consume_delimiter(std::string_view available) noexcept {
    if (!available.starts_with("\r\n")) {
        return chunk_framing_error::invalid_crlf;
    }
    return std::nullopt;
}

std::optional<chunk_framing_error> http_chunk_framing::validate_trailers(std::string_view trailers) const noexcept {
    if (config_.trailer_role_ == chunk_trailer_role::response) {
        return http_response_trailer_block_valid(trailers)
                   ? std::nullopt
                   : std::optional(chunk_framing_error::invalid_trailer);
    }
    if (const auto error = validate_http_chunk_trailers(trailers)) {
        return *error == http_chunk_scan_error::too_large
                   ? chunk_framing_error::trailer_limit_exceeded
                   : chunk_framing_error::invalid_trailer;
    }
    return std::nullopt;
}

chunk_framing_result http_chunk_framing::fail(std::size_t consumed_bytes, chunk_framing_error error) noexcept {
    state_ = error;
    return chunk_framing_failure{consumed_bytes, error};
}

}  // namespace ruvia::detail
