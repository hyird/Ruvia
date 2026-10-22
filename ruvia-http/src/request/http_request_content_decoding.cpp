#include <cstddef>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_request.h"

#include "coding/http_content_coding.h"
#include "request/http_request_access.h"

namespace ruvia {

http_content_coding_field_result request_content_coding(
    const http_request& request) noexcept {
    detail::http_content_coding_field_parser parser;
    if (!detail::request_has_known_header(request, detail::request_header_kind::content_encoding)) {
        return parser.finish();
    }
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (detail::http_request_access::header_kind(request, i) ==
            static_cast<std::uint8_t>(detail::request_header_kind::content_encoding)) {
            parser.update(headers[i].value());
        }
    }
    return parser.finish();
}

std::pmr::string decode_http_request_content(
    std::span<const http_content_coding> codings, std::string_view body,
    http_content_decode_options options) {
    auto result_value = decode_http_content(codings, body, options);
    if (auto* decoded = std::get_if<std::pmr::string>(&result_value)) {
        return std::move(*decoded);
    }
    switch (std::get<http_content_decode_error>(result_value)) {
        case http_content_decode_error::unsupported_coding:
            throw http_protocol_error(http_status::unsupported_media_type,
                "unsupported_content_coding", "request Content-Encoding is not supported");
        case http_content_decode_error::invalid_content:
            throw http_protocol_error(http_status::bad_request, "invalid_content_coding",
                "failed to decode request body");
        case http_content_decode_error::decoded_size_exceeded:
            throw http_request_body_protocol_error(http_request_body_failure::too_large);
        case http_content_decode_error::decoder_failure:
            throw std::runtime_error("request content decoder failed");
    }
    throw std::runtime_error("request content decoder failed");
}

}  // namespace ruvia
