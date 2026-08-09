#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/coding/HttpContentLength.h"
#include "ruvia/http/detail/coding/HttpTransferEncoding.h"
// The two steps between an HTTP/1 client response head on the wire and the plan
// the parser acts on: reading the status line and header fields into borrowed
// values, then deciding from them (and the request that produced them) how the
// body is framed and whether the connection survives.

namespace ruvia::detail {

struct Http1ClientParsedStatusLine final {
    HttpStatusCode statusCode;
    HttpProtocolVersion protocolVersion;
};

struct Http1ClientParsedResponseHead final {
    explicit Http1ClientParsedResponseHead(const Http1ClientParsedStatusLine& statusLine) noexcept
        : statusCode(statusLine.statusCode),
          protocolVersion(statusLine.protocolVersion) {}

    std::array<HttpHeaderView, kMaxHttpHeaderFields> headers;
    std::size_t headerCount{0};
    HttpStatusCode statusCode;
    HttpProtocolVersion protocolVersion;
    bool contentLengthFieldPresent{false};
    bool contentTypeFieldPresent{false};
    bool sawTransferEncoding{false};
    bool nonEmptyTrailerHeaderPresent{false};
    HttpConnectionOptions connectionOptions;
    HttpUpgradeProtocols upgradeProtocols;
    HttpContentLengthState contentLength;
    HttpTransferEncodingState transferEncoding;
};

using Http1ClientStatusLineParseResult = std::variant<Http1ClientParsedStatusLine, Http1ClientResponseParseError>;
using Http1ClientResponseHeadParseResult = std::variant<Http1ClientParsedResponseHead, Http1ClientResponseParseError>;
using Http1ClientResponsePlanningResult = std::variant<Http1ClientResponsePlan, Http1ClientResponseParseError>;

// Parse the status line and header fields of one complete head section.
[[nodiscard]] Http1ClientResponseHeadParseResult parseHttp1ClientResponseHeadFields(std::string_view headSection, const Http1ClientRequestContext& request) noexcept;

// Decide the response plan a parsed head implies for this request.
[[nodiscard]] Http1ClientResponsePlanningResult planHttp1ClientResponse(const Http1ClientRequestContext& request, const Http1ClientParsedResponseHead& response, Http1ClientRequestContentPhase requestContentPhase) noexcept;

}  // namespace ruvia::detail
