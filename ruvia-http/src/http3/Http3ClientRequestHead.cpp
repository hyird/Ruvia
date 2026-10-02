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
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/http3/Http3FieldSectionEncoder.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia {
namespace {

bool token(std::string_view text) noexcept {
    constexpr std::string_view extra{"!#$%&'*+-.^_`|~"};
    if (text.empty()) {
        return false;
    }
    for (const unsigned char ch : text) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || extra.find(static_cast<char>(ch)) != std::string_view::npos)) {
            return false;
        }
    }
    return true;
}

bool equalIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto ch = static_cast<unsigned char>(a[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch + ('a' - 'A'));
        }
        if (ch != static_cast<unsigned char>(b[i])) {
            return false;
        }
    }
    return true;
}

Http3ClientRequestHeadFailure failure(Http3ClientRequestHeadError kind) noexcept {
    return {kind};
}
Http3ClientRequestHeadFailure fieldFailure(Http3FieldSectionError error) noexcept {
    return {Http3ClientRequestHeadError::kFieldSectionError, error};
}

bool validValue(std::string_view value) noexcept {
    for (const unsigned char ch : value) {
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool parseLength(std::string_view value, std::uint64_t& result) noexcept {
    if (value.empty()) {
        return false;
    }
    result = 0;
    for (const unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    return true;
}

bool validScheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !((scheme.front() >= 'a' && scheme.front() <= 'z') ||
                              (scheme.front() >= 'A' && scheme.front() <= 'Z'))) {
        return false;
    }
    for (const unsigned char ch : scheme) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

bool authorityValid(std::string_view authority, std::string_view scheme) noexcept {
    if (equalIgnoreCase(scheme, "http") || equalIgnoreCase(scheme, "https")) {
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
    if (!token(view.method)) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidMethod));
    }
    if (extendedConnect && (!connect || !token(view.protocol))) {
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
    } else if (!validScheme(view.scheme) || !authorityValid(view.authority, view.scheme) ||
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
    std::size_t decodedBytes = 0;
    const auto countField = [&](std::string_view name, std::string_view value) noexcept {
        if (name.size() > std::numeric_limits<std::size_t>::max() - 32 ||
            value.size() > std::numeric_limits<std::size_t>::max() - name.size() - 32 ||
            decodedBytes > std::numeric_limits<std::size_t>::max() - name.size() - value.size() - 32) {
            return false;
        }
        decodedBytes += name.size() + value.size() + 32;
        return true;
    };
    if ((extendedConnect && !countField(":protocol", view.protocol)) || !countField(":method", view.method) ||
        ((!connect || extendedConnect) && !countField(":scheme", view.scheme)) ||
        !countField(":authority", view.authority) ||
        ((!connect || extendedConnect) && !countField(":path", view.path))) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
    }
    std::size_t lowercaseBytes = 0;
    bool hostSeen = false;
    bool contentTypeSeen = false;
    bool lengthSeen = false;
    std::uint64_t contentLength = 0;
    for (const auto& field : view.fields) {
        if (!token(field.name)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (!validValue(field.value) ||
            (equalIgnoreCase(field.name, "origin") &&
                !detail::isValidHttpOriginFieldValue(field.value)) ||
            (equalIgnoreCase(field.name, "access-control-request-method") &&
                !detail::isValidHttpCorsRequestMethod(field.value)) ||
            (equalIgnoreCase(field.name, "access-control-request-headers") &&
                !detail::isValidHttpCorsRequestHeaderNames(field.value)) ||
            (equalIgnoreCase(field.name, "expect") &&
                !detail::isValidHttpExpectFieldValue(field.value))) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        // RFC 9110 section 9.3.8: never generate known credential/cookie
        // fields in TRACE. Callers must also omit application-specific secrets.
        if (trace && (equalIgnoreCase(field.name, "authorization") ||
                         equalIgnoreCase(field.name, "proxy-authorization") ||
                         equalIgnoreCase(field.name, "cookie"))) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (equalIgnoreCase(field.name, "connection") || equalIgnoreCase(field.name, "keep-alive") ||
            equalIgnoreCase(field.name, "proxy-connection") || equalIgnoreCase(field.name, "transfer-encoding") ||
            equalIgnoreCase(field.name, "upgrade")) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (equalIgnoreCase(field.name, "te") && !equalIgnoreCase(field.value, "trailers")) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kForbiddenField));
        }
        if (equalIgnoreCase(field.name, "host")) {
            if (hostSeen || field.value != view.authority) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidAuthority));
            }
            hostSeen = true;
        }
        if (equalIgnoreCase(field.name, "content-type")) {
            if (contentTypeSeen || !isValidHttpContentTypeFieldValue(field.value)) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
            }
            contentTypeSeen = true;
        } else if (equalIgnoreCase(field.name, "content-encoding") &&
                   !detail::isValidHttpContentEncodingFieldValue(
                       field.value, detail::HttpFieldListRole::kSender)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (equalIgnoreCase(field.name, "trailer") &&
            !detail::isValidHttpRequestTrailerFieldValue(
                field.value, detail::HttpFieldListRole::kSender)) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidField));
        }
        if (equalIgnoreCase(field.name, "content-length")) {
            if (connect) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
            }
            std::uint64_t parsed{};
            if (lengthSeen || !parseLength(field.value, parsed) || (trace && parsed != 0) ||
                (view.bodyLength && parsed != *view.bodyLength)) {
                return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
            }
            lengthSeen = true;
            contentLength = parsed;
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
            !countField(field.name, field.value)) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
        ++projectedCount;
        if (projectedCount > limits.maxFields) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kTooManyFields));
        }
        if (decodedBytes > limits.maxDecodedBytes) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
    }
    if (view.bodyLength && lengthSeen && contentLength != *view.bodyLength) {
        return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
    }
    const bool emitLength = view.emit_content_length && view.bodyLength && !lengthSeen;
    std::array<char, 20> lengthBytes{};
    std::size_t lengthSize = 0;
    if (emitLength) {
        auto [end, ec] = std::to_chars(lengthBytes.data(), lengthBytes.data() + lengthBytes.size(), *view.bodyLength);
        if (ec != std::errc{}) {
            return std::unexpected(failure(Http3ClientRequestHeadError::kInvalidContentLength));
        }
        lengthSize = static_cast<std::size_t>(end - lengthBytes.data());
        if (projectedCount == std::numeric_limits<std::size_t>::max() ||
            !countField("content-length", {lengthBytes.data(), lengthSize})) {
            return std::unexpected(fieldFailure(Http3FieldSectionError::kFieldListTooLarge));
        }
        ++projectedCount;
    }
    if (projectedCount > limits.maxFields) {
        return std::unexpected(fieldFailure(Http3FieldSectionError::kTooManyFields));
    }
    if (decodedBytes > limits.maxDecodedBytes) {
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
                lowercase.push_back(ch >= 'A' && ch <= 'Z'
                                        ? static_cast<char>(ch + ('a' - 'A'))
                                        : static_cast<char>(ch));
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
                                     : lengthSeen      ? std::optional{contentLength}
                                                       : std::nullopt;
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
