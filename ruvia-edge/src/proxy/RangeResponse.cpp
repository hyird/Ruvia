#include "ruvia/edge/detail/proxy/RangeResponse.h"

#include <string>

#include "ruvia/edge/detail/proxy/ByteRange.h"

namespace ruvia::edge {

std::optional<CachedRangeResponse> cachedRangeResponse(const CachedResponse& entry, std::string_view rangeHeader) {
    const auto range = parseSingleByteRange(rangeHeader, entry.body.size());
    if (range.unsatisfiable) {
        CachedRangeResponse response;
        response.status = 416;
        response.headers.emplace_back("Content-Range", "bytes */" + std::to_string(entry.body.size()));
        return response;
    }
    if (!range.satisfiable) {
        return std::nullopt;
    }

    CachedRangeResponse response;
    response.status = 206;
    response.headers = entry.headers;
    response.headers.emplace_back("Content-Range", "bytes " + std::to_string(range.start) + "-" + std::to_string(range.end) + "/" + std::to_string(entry.body.size()));
    response.body = std::string_view(entry.body).substr(range.start, range.end - range.start + 1);
    response.withAge = true;
    return response;
}

}  // namespace ruvia::edge
