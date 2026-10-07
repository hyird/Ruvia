#include "ruvia/http/Http3ClientRequestHead.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/HttpMediaType.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/coding/HttpContentLength.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/field/HttpOriginFields.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/http3/Http3FieldSectionEncoder.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia {
namespace {

Http3ClientRequestHeadFailure failure(Http3ClientRequestHeadError kind) noexcept {
    return {kind};
}
Http3ClientRequestHeadFailure fieldFailure(Http3FieldSectionError error) noexcept {
    return {Http3ClientRequestHeadError::kFieldSectionError, error};
}

bool authorityValid(std::string_view authority, std::string_view scheme) noexcept {
    if (httpAsciiEqualsIgnoreCase(scheme, "http") || httpAsciiEqualsIgnoreCase(scheme, "https")) {
        const auto host = parseHttpAuthorityHost(authority);
        return host && !host->empty();
    }
    if (authority.empty()) {
        return false;
    }
    for (const unsigned char ch : authority) {
        if (ch <= 0x20 || ch == 0x7f || ch == '/' || ch == '?' || ch == '#') {
            return false;
        }
    }
    return true;
}

}  // namespace

static std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeRequestHead(
    Http3ClientRequestHeadView view, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource, Http3QpackEncoder* encoder, std::uint64_t streamId) {
    auto* memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    const bool connect = view.method == "CONNECT";
    const bool extendedConnect = !view.protocol.empty();
    const bool trace = view.method == "TRACE";
    if (!detail::isValidHttpHeaderName(view.method)) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidMethod));
    }
    if (extendedConnect && (!connect || !detail::isValidHttpHeaderName(view.protocol))) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidProtocol));
    }
    if (extendedConnect && !view.peerEnableConnectProtocol) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kConnectProtocolDisabled));
    }
    if (connect && !extendedConnect) {
        const auto tunnel = detail::parseHttpAuthority(view.authority);
        if (!tunnel || tunnel->portKind() != detail::HttpAuthorityPortKind::kValue || *tunnel->port() == 0 ||
            !view.scheme.empty() || !view.path.empty()) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidTarget));
        }
        // CONNECT carries tunnel bytes, not an HTTP representation with a
        // declared Content-Length (RFC 9110, section 9.3.6).
        if (view.bodyLength) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
        }
    } else if (!detail::isValidUriScheme(view.scheme) || !authorityValid(view.authority, view.scheme) ||
               !((view.path == "*" && view.method == "OPTIONS") || isValidHttpOriginFormTarget(view.path))) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidTarget));
    }

    if (connect && view.bodyLength) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
    }
    if (trace && view.bodyLength && *view.bodyLength != 0) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
    }

    std::size_t projectedCount = (connect && !extendedConnect ? 2 : 4) + (extendedConnect ? 1 : 0);
    // Keep the original error precedence: overflow is checked while projecting
    // each field, but the configured byte limit follows the field-count check.
    detail::HttpHeaderSectionSize sectionSize(std::numeric_limits<std::size_t>::max());
    if ((extendedConnect && !sectionSize.add(":protocol", view.protocol)) || !sectionSize.add(":method", view.method) ||
        ((!connect || extendedConnect) && !sectionSize.add(":scheme", view.scheme)) ||
        !sectionSize.add(":authority", view.authority) ||
        ((!connect || extendedConnect) && !sectionSize.add(":path", view.path))) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
    }
    std::size_t lowercaseBytes = 0;
    bool hostSeen = false;
    bool contentTypeSeen = false;
    detail::HttpContentLengthState<std::uint64_t> contentLength;
    for (const auto& field : view.fields) {
        if (!detail::isValidHttpHeaderName(field.name)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (!detail::is_valid_http_field_value_bytes(field.value) ||
            (httpAsciiEqualsIgnoreCase(field.name, "origin") &&
                !detail::is_valid_http_origin_field_value(field.value)) ||
            (httpAsciiEqualsIgnoreCase(field.name, "access-control-request-method") &&
                !detail::isValidHttpCorsRequestMethod(field.value)) ||
            (httpAsciiEqualsIgnoreCase(field.name, "access-control-request-headers") &&
                !detail::isValidHttpCorsRequestHeaderNames(field.value)) ||
            (httpAsciiEqualsIgnoreCase(field.name, "expect") &&
                !detail::isValidHttpExpectFieldValue(field.value))) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        // RFC 9110 section 9.3.8: never generate known credential/cookie
        // fields in TRACE. Callers must also omit application-specific secrets.
        if (trace && (httpAsciiEqualsIgnoreCase(field.name, "authorization") ||
                         httpAsciiEqualsIgnoreCase(field.name, "proxy-authorization") ||
                         httpAsciiEqualsIgnoreCase(field.name, "cookie"))) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "te") && !httpAsciiEqualsIgnoreCase(field.value, "trailers")) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "host")) {
            if (hostSeen || field.value != view.authority) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidAuthority));
            }
            hostSeen = true;
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "content-type")) {
            if (contentTypeSeen || !isValidHttpContentTypeFieldValue(field.value)) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
            }
            contentTypeSeen = true;
        } else if (httpAsciiEqualsIgnoreCase(field.name, "content-encoding") &&
                   !detail::isValidHttpContentEncodingFieldValue(
                       field.value, detail::HttpFieldListRole::kSender)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "trailer") &&
            !detail::isValidHttpRequestTrailerFieldValue(
                field.value, detail::HttpFieldListRole::kSender)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (httpAsciiEqualsIgnoreCase(field.name, "content-length")) {
            if (connect) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
            }
            if (contentLength.value() ||
                contentLength.parse_single_value(field.value) != detail::HttpContentLengthParseStatus::kOk ||
                (trace && *contentLength.value() != 0) ||
                (view.bodyLength && *contentLength.value() != *view.bodyLength)) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
            }
        }
        const bool hasUppercase = std::any_of(field.name.begin(), field.name.end(),
            [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
        if (hasUppercase) {
            if (field.name.size() > std::numeric_limits<std::size_t>::max() - lowercaseBytes) {
                return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
            }
            lowercaseBytes += field.name.size();
        }
        if (projectedCount == std::numeric_limits<std::size_t>::max() ||
            !sectionSize.add(field.name, field.value)) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
        ++projectedCount;
        if (projectedCount > limits.maxFields) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kTooManyFields));
        }
        if (sectionSize.bytes() > limits.maxDecodedBytes) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
    }
    const bool emitLength = view.emit_content_length && view.bodyLength && !contentLength.value();
    std::array<char, 20> lengthBytes{};
    std::size_t lengthSize = 0;
    if (emitLength) {
        auto [end, ec] = std::to_chars(lengthBytes.data(), lengthBytes.data() + lengthBytes.size(), *view.bodyLength);
        if (ec != std::errc{}) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
        }
        lengthSize = static_cast<std::size_t>(end - lengthBytes.data());
        if (projectedCount == std::numeric_limits<std::size_t>::max() ||
            !sectionSize.add("content-length", {lengthBytes.data(), lengthSize})) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
        ++projectedCount;
    }
    if (projectedCount > limits.maxFields) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kTooManyFields));
    }
    if (sectionSize.bytes() > limits.maxDecodedBytes) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
    }

    std::pmr::vector<char> lowercase(memory);
    lowercase.reserve(lowercaseBytes);
    std::pmr::vector<Http3FieldSectionFieldView> fields(memory);
    fields.reserve(projectedCount);
    fields.push_back({":method", view.method, false});
    if (extendedConnect) {
        fields.push_back({":protocol", view.protocol, false});
    }
    if (!connect || extendedConnect) {
        fields.push_back({":scheme", view.scheme, false});
        fields.push_back({":authority", view.authority, false});
        fields.push_back({":path", view.path, false});
    } else {
        fields.push_back({":authority", view.authority, false});
    }
    for (const auto& field : view.fields) {
        std::string_view name = field.name;
        const bool hasUppercase = std::any_of(name.begin(), name.end(),
            [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
        if (hasUppercase) {
            const auto offset = lowercase.size();
            for (const unsigned char ch : name) {
                lowercase.push_back(static_cast<char>(httpAsciiToLower(ch)));
            }
            name = std::string_view(lowercase.data() + offset, field.name.size());
        }
        fields.push_back({name, field.value, false});
    }
    if (emitLength) {
        fields.push_back({"content-length", {lengthBytes.data(), lengthSize}, false});
    }
    auto encoded = detail::encodeHttp3Fields(fields, memory, limits, encoder, streamId);
    if (!encoded) {
        return std::unexpected(fieldFailure(encoded.error()));
    }
    if (encoded->size() > limits.maxEncodedBytes) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldSectionTooLarge));
    }

    Http3ClientRequestHead result(memory);
    result.fieldSection = std::move(*encoded);
    result.bodyPlan.expectedLength = trace             ? std::optional<std::uint64_t>{0}
                                     : view.bodyLength ? view.bodyLength
                                                       : contentLength.value();
    return result;
}

std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeHttp3ClientRequestHead(
    Http3ClientRequestHeadView view, Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeRequestHead(view, limits, resource, nullptr, 0);
}
std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encodeHttp3ClientRequestHead(
    Http3QpackEncoder& encoder, std::uint64_t streamId, Http3ClientRequestHeadView view,
    Http3FieldSectionLimits limits, std::pmr::memory_resource* resource) {
    return encodeRequestHead(view, limits, resource, &encoder, streamId);
}

}  // namespace ruvia
