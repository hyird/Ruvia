#include "ruvia/http/Http1InterimResponseWriter.h"

#include <cstring>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpInterimResponseValidation.h"

namespace ruvia::detail {

struct Http1InterimResponsePrepareResultAccess final {
    [[nodiscard]] static constexpr Http1InterimResponsePrepareResult bufferTooSmall(
        std::size_t requiredHeadBytes) noexcept {
        return Http1InterimResponsePrepareResult(
            Http1InterimResponseBufferTooSmall(requiredHeadBytes));
    }

    [[nodiscard]] static constexpr Http1InterimResponsePrepareResult failure(
        Http1InterimResponsePrepareError error) noexcept {
        return Http1InterimResponsePrepareResult(Http1InterimResponsePrepareFailure(error));
    }

    [[nodiscard]] static constexpr Http1InterimResponsePrepareResult prepared(
        std::string_view head, Http1InterimConnectionDisposition connectionDisposition) noexcept {
        return Http1InterimResponsePrepareResult(
            PreparedHttp1InterimResponse(head, connectionDisposition));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view kHttp11StatusPrefix = "HTTP/1.1 ";
constexpr std::string_view kCrlf = "\r\n";

struct Http1InterimHeaderFacts final {
    std::size_t wireBytes{0};
    detail::HttpConnectionOptions connectionOptions;
    detail::HttpUpgradeProtocols upgradeProtocols;
};

[[nodiscard]] bool addHeadBytes(std::size_t& total, std::size_t bytes) noexcept {
    if (bytes > kMaxHttpHeaderBytes - total) {
        return false;
    }
    total += bytes;
    return true;
}

[[nodiscard]] Http1InterimResponsePrepareError commonValidationError(
    detail::HttpInterimResponseHeaderValidationStatus status) noexcept {
    switch (status) {
        case detail::HttpInterimResponseHeaderValidationStatus::kInvalidHeader:
            return Http1InterimResponsePrepareError::kInvalidHeader;
        case detail::HttpInterimResponseHeaderValidationStatus::kContentLengthForbidden:
            return Http1InterimResponsePrepareError::kContentLengthForbidden;
        case detail::HttpInterimResponseHeaderValidationStatus::kTransferEncodingForbidden:
            return Http1InterimResponsePrepareError::kTransferEncodingForbidden;
        case detail::HttpInterimResponseHeaderValidationStatus::kTrailerForbidden:
            return Http1InterimResponsePrepareError::kTrailerForbidden;
        case detail::HttpInterimResponseHeaderValidationStatus::kRepeatedSingleton:
            return Http1InterimResponsePrepareError::kRepeatedSingleton;
        case detail::HttpInterimResponseHeaderValidationStatus::kOk:
            break;
    }
    return Http1InterimResponsePrepareError::kInvalidHeader;
}

[[nodiscard]] bool analyzeHttp1Fields(const HttpInterimResponseHead& response,
    Http1InterimHeaderFacts& facts, Http1InterimResponsePrepareError& error) noexcept {
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        if (detail::httpAsciiEqualsIgnoreCase(name, "Connection")) {
            if (facts.connectionOptions.parseField(header.value(),
                    detail::HttpFieldListRole::kSender, [](std::string_view option) noexcept {
                        return !detail::httpConnectionOptionConflictsWithManagedField(option);
                    }) != detail::HttpFieldListParseStatus::kOk) {
                error = Http1InterimResponsePrepareError::kInvalidConnection;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
            if (facts.upgradeProtocols.parseField(header.value(),
                    detail::HttpFieldListRole::kSender,
                    [](const detail::HttpUpgradeProtocol&) noexcept { return true; }) !=
                detail::HttpFieldListParseStatus::kOk) {
                error = Http1InterimResponsePrepareError::kInvalidUpgrade;
                return false;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(name, "TE")) {
            error = Http1InterimResponsePrepareError::kTeFieldForbidden;
            return false;
        }

        if (!addHeadBytes(facts.wireBytes, name.size()) || !addHeadBytes(facts.wireBytes, 2) ||
            !addHeadBytes(facts.wireBytes, header.value().size()) ||
            !addHeadBytes(facts.wireBytes, kCrlf.size())) {
            error = Http1InterimResponsePrepareError::kHeaderTooLarge;
            return false;
        }
    }
    if (facts.upgradeProtocols.hasField() && !facts.connectionOptions.upgrade()) {
        error = Http1InterimResponsePrepareError::kUpgradeConnectionOptionRequired;
        return false;
    }
    return true;
}

void appendView(char*& cursor, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(cursor, value.data(), value.size());
        cursor += value.size();
    }
}

}  // namespace

std::string_view http1InterimResponsePrepareErrorMessage(
    Http1InterimResponsePrepareError error) noexcept {
    switch (error) {
        case Http1InterimResponsePrepareError::kInvalidHeader:
            return "invalid HTTP/1 interim response header";
        case Http1InterimResponsePrepareError::kTooManyHeaders:
            return "too many HTTP/1 interim response headers";
        case Http1InterimResponsePrepareError::kContentLengthForbidden:
            return "Content-Length is forbidden on an interim response";
        case Http1InterimResponsePrepareError::kTransferEncodingForbidden:
            return "Transfer-Encoding is forbidden on an interim response";
        case Http1InterimResponsePrepareError::kTrailerForbidden:
            return "Trailer is forbidden on an interim response";
        case Http1InterimResponsePrepareError::kTeFieldForbidden:
            return "TE is not a response field";
        case Http1InterimResponsePrepareError::kRepeatedSingleton:
            return "repeated singleton interim response header";
        case Http1InterimResponsePrepareError::kInvalidConnection:
            return "invalid HTTP/1 interim response Connection field";
        case Http1InterimResponsePrepareError::kInvalidUpgrade:
            return "invalid HTTP/1 interim response Upgrade field";
        case Http1InterimResponsePrepareError::kUpgradeConnectionOptionRequired:
            return "HTTP/1 interim Upgrade requires Connection: Upgrade";
        case Http1InterimResponsePrepareError::kHeaderTooLarge:
            return "HTTP/1 interim response head is too large";
    }
    return "invalid HTTP/1 interim response";
}

Http1InterimResponsePrepareResult Http1InterimResponseWriter::prepare(
    const HttpInterimResponseHead& response, std::span<char> headBuffer) const noexcept {
    if (response.headers().size() > kMaxHttpHeaderFields) {
        return detail::Http1InterimResponsePrepareResultAccess::failure(
            Http1InterimResponsePrepareError::kTooManyHeaders);
    }
    const auto commonValidation = detail::validateHttpInterimResponseHeaders(response);
    if (commonValidation != detail::HttpInterimResponseHeaderValidationStatus::kOk) {
        return detail::Http1InterimResponsePrepareResultAccess::failure(
            commonValidationError(commonValidation));
    }

    const auto reasonPhrase = httpReasonPhrase(response.status());
    const auto statusToken = detail::httpStatusCodeToken(response.status());
    Http1InterimHeaderFacts facts;
    facts.wireBytes =
        kHttp11StatusPrefix.size() + statusToken.size() + 1 + reasonPhrase.size() + kCrlf.size();
    Http1InterimResponsePrepareError error = Http1InterimResponsePrepareError::kInvalidHeader;
    if (!analyzeHttp1Fields(response, facts, error)) {
        return detail::Http1InterimResponsePrepareResultAccess::failure(error);
    }
    if (!addHeadBytes(facts.wireBytes, kCrlf.size())) {
        return detail::Http1InterimResponsePrepareResultAccess::failure(
            Http1InterimResponsePrepareError::kHeaderTooLarge);
    }
    if (headBuffer.size() < facts.wireBytes) {
        return detail::Http1InterimResponsePrepareResultAccess::bufferTooSmall(facts.wireBytes);
    }

    char* cursor = headBuffer.data();
    appendView(cursor, kHttp11StatusPrefix);
    appendView(cursor, detail::httpStatusCodeTokenView(statusToken));
    *cursor++ = ' ';
    appendView(cursor, reasonPhrase);
    appendView(cursor, kCrlf);
    for (const auto& header : response.headers()) {
        appendView(cursor, header.name());
        appendView(cursor, ": ");
        appendView(cursor, header.value());
        appendView(cursor, kCrlf);
    }
    appendView(cursor, kCrlf);

    return detail::Http1InterimResponsePrepareResultAccess::prepared(
        std::string_view(headBuffer.data(), facts.wireBytes),
        facts.connectionOptions.close()
            ? Http1InterimConnectionDisposition::kCloseAfterInterimResponse
            : Http1InterimConnectionDisposition::kUnchanged);
}

}  // namespace ruvia
