#include "ruvia/http/HttpRequestContentDecoding.h"

#include <cstddef>
#include <utility>

#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {

HttpContentCodingFieldResult requestContentCoding(
    const HttpRequest& request, std::pmr::memory_resource* resource) {
    detail::HttpContentCodingFieldParser parser(detail::HttpFieldListRole::kRecipient, resource);
    if (!detail::requestHasKnownHeader(request, detail::RequestHeaderKind::kContentEncoding)) {
        return std::move(parser).finish();
    }
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (detail::HttpRequestAccess::headerKind(request, i) ==
            std::to_underlying(detail::RequestHeaderKind::kContentEncoding)) {
            parser.update(headers[i].value());
        }
    }
    return std::move(parser).finish();
}

}  // namespace ruvia
