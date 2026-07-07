#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "HeaderAcceptUtils.h"
#include "HttpRequestFlags.h"
#include "HttpTransferCoding.h"
#include "ruvia/http/HttpParseTypes.h"
#include "ruvia/http/HttpParser.h"
#include "HttpRequestInternal.h"

namespace ruvia::detail {

struct HttpParseResultAccess final {
    [[nodiscard]] static HttpParseResult make(
        HttpParseStatus status,
        HttpParseError error,
        HttpRequest request,
        std::size_t consumedBytes) noexcept {
        return HttpParseResult(status, error, std::move(request), consumedBytes);
    }

    [[nodiscard]] static HttpRequest& request(HttpParseResult& result) noexcept {
        return result.request_;
    }
};

struct HttpServerParseResult {
    HttpParseStatus status{HttpParseStatus::kIncomplete};
    HttpParseError error{HttpParseError::kNone};
    HttpRequest request{HttpRequestAccess::make()};
    std::size_t headerBytes{0};
    std::size_t contentLength{0};
    std::size_t consumedBytes{0};
    bool chunked{false};
    HttpContentCoding responseCoding{HttpContentCoding::kNone};
    HttpTransferCodings transferCodings;
    HttpRequestFlags flags;
};

class HttpServerParser final {
public:
    // In-place parsing entry points: `result` is reset and reused across
    // calls so the request hot path never copies or re-zeroes the ~2.5KB
    // parse result per read iteration.
    void parseHeaders(
        std::string_view buffer,
        HttpServerParseResult& result,
        std::size_t headerSearchOffset = 0) const noexcept;
    void parseBody(std::string_view buffer, HttpServerParseResult& result) const noexcept;
    [[nodiscard]] HttpServerParseResult parse(std::string_view buffer) const noexcept;

private:
    static void parseRequestHead(
        std::string_view buffer,
        std::size_t headerSearchOffset,
        HttpServerParseResult& result) noexcept;
};

}  // namespace ruvia::detail
