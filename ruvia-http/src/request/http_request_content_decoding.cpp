#include "ruvia/http/http_request_content_decoding.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include "coding/http_content_coding.h"
#include "request/http_request_access.h"

namespace ruvia {

http_content_coding_field_result request_content_coding(
    const http_request& request, std::pmr::memory_resource* resource) {
    detail::http_content_coding_field_parser parser(detail::http_field_list_role::recipient, resource);
    if (!detail::request_has_known_header(request, detail::request_header_kind::content_encoding)) {
        return std::move(parser).finish();
    }
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (detail::http_request_access::header_kind(request, i) ==
            static_cast<std::uint8_t>(detail::request_header_kind::content_encoding)) {
            parser.update(headers[i].value());
        }
    }
    return std::move(parser).finish();
}

}  // namespace ruvia
