#include "ruvia/http/HttpRequestContentDecoding.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {

HttpContentCodingFieldResult requestContentCoding(const HttpRequest& request) noexcept {
    detail::HttpContentCodingFieldParser parser;
    if (!detail::requestHasKnownHeader(request, detail::RequestKnownHeader::kContentEncoding)) {
        return parser.finish();
    }
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (detail::HttpRequestAccess::headerKind(request, i) ==
            std::to_underlying(detail::RequestHeaderKind::kContentEncoding)) {
            parser.update(headers[i].value());
        }
    }
    return parser.finish();
}

}  // namespace ruvia
