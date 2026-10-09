#include "ruvia/http/Http3MessageHead.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/HttpMediaType.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/HttpOws.h"

#include "coding/HttpContentCoding.h"
#include "coding/HttpContentLength.h"
#include "field/HttpCorsFields.h"
#include "field/HttpOriginFields.h"
#include "field/binary_field_name.h"
#include "parser/HttpRequestTarget.h"

namespace ruvia {
namespace {

bool validPath(std::string_view value, std::string_view method) noexcept {
    return (value == "*" && method == "OPTIONS") || isValidHttpOriginFormTarget(value);
}

bool validAuthority(std::string_view value, std::string_view scheme) noexcept {
    if (httpAsciiEqualsIgnoreCase(scheme, "http") ||
        httpAsciiEqualsIgnoreCase(scheme, "https")) {
        const auto host = parseHttpAuthorityHost(value);
        return host.has_value() && !host->empty();
    }
    for (const unsigned char ch : value) {
        if (ch <= 0x20 || ch == 0x7f || ch == '/' || ch == '?' || ch == '#' ||
            (ch == '@' && (scheme.empty() || httpAsciiEqualsIgnoreCase(scheme, "http") ||
                              httpAsciiEqualsIgnoreCase(scheme, "https")))) {
            return false;
        }
    }
    return !value.empty();
}

struct DecodeState final {
    DecodeState(Http3MessageHead& messageHead, Http3MessageHeadKind messageKind,
        std::pmr::memory_resource* resource, std::size_t maximumSize)
        : head(messageHead),
          kind(messageKind),
          sectionSize(maximumSize),
          host(resource) {}

    Http3MessageHead& head;
    Http3MessageHeadKind kind;
    Http3MessageHeadError error{Http3MessageHeadError::kMessageError};
    detail::HttpHeaderSectionSize sectionSize;
    bool ordinarySeen{false};
    bool methodSeen{false};
    bool protocolSeen{false};
    bool schemeSeen{false};
    bool authoritySeen{false};
    bool pathSeen{false};
    bool statusSeen{false};
    bool hostSeen{false};
    bool callbackRejected{false};
    std::pmr::string host;
    bool contentTypeSeen{false};
    detail::HttpContentLengthState<std::uint64_t> contentLength;
};

bool fail(DecodeState& state) noexcept {
    state.error = Http3MessageHeadError::kMessageError;
    state.callbackRejected = true;
    return false;
}

bool receiveField(void* opaque, Http3FieldSectionFieldView field) {
    auto& state = *static_cast<DecodeState*>(opaque);
    if (field.name.empty() || !detail::is_valid_http_field_value_bytes(field.value)) {
        return fail(state);
    }
    if (!state.sectionSize.add(field.name, field.value)) {
        state.error = Http3MessageHeadError::kFieldSectionTooLarge;
        state.callbackRejected = true;
        return false;
    }

    if (field.name.front() == ':') {
        if (state.ordinarySeen) {
            return fail(state);
        }
        const auto setText = [&](std::pmr::string& target, bool& seen) {
            if (seen) {
                return false;
            }
            target.assign(field.value);
            seen = true;
            return true;
        };
        if (state.kind == Http3MessageHeadKind::kRequest) {
            if (field.name == ":method") {
                if (!detail::isValidHttpHeaderName(field.value) || !setText(state.head.method, state.methodSeen)) {
                    return fail(state);
                }
            } else if (field.name == ":protocol") {
                if (!detail::isValidHttpHeaderName(field.value) ||
                    !setText(state.head.protocol, state.protocolSeen)) {
                    return fail(state);
                }
            } else if (field.name == ":scheme") {
                if (!detail::isValidUriScheme(field.value) || !setText(state.head.scheme, state.schemeSeen)) {
                    return fail(state);
                }
            } else if (field.name == ":authority") {
                if (field.value.empty() || !setText(state.head.authority, state.authoritySeen)) {
                    return fail(state);
                }
            } else if (field.name == ":path") {
                if (field.value.empty() || !setText(state.head.path, state.pathSeen)) {
                    return fail(state);
                }
            } else {
                return fail(state);
            }
        } else {
            if (field.name != ":status" || state.statusSeen || field.value.size() != 3 ||
                field.value[0] < '1' || field.value[0] > '5' ||
                field.value[1] < '0' || field.value[1] > '9' ||
                field.value[2] < '0' || field.value[2] > '9') {
                return fail(state);
            }
            state.head.status = static_cast<std::uint16_t>((field.value[0] - '0') * 100 +
                                                           (field.value[1] - '0') * 10 + (field.value[2] - '0'));
            state.statusSeen = true;
        }
        return true;
    }

    state.ordinarySeen = true;
    if (!detail::is_valid_binary_field_name(field.name)) {
        return fail(state);
    }
    if (detail::is_forbidden_http_binary_connection_field(field.name)) {
        return fail(state);
    }
    if (field.name == "te" &&
        (state.kind != Http3MessageHeadKind::kRequest ||
            !httpAsciiEqualsIgnoreCase(detail::httpTrimOws(field.value), "trailers"))) {
        return fail(state);
    }
    if (field.name == "host") {
        if (state.kind != Http3MessageHeadKind::kRequest || field.value.empty() ||
            state.hostSeen ||
            (state.authoritySeen && state.head.authority != field.value)) {
            return fail(state);
        }
        state.host.assign(field.value);
        state.hostSeen = true;
    }
    if (state.kind == Http3MessageHeadKind::kRequest) {
        if ((field.name == "origin" && !detail::is_valid_http_origin_field_value(field.value)) ||
            (field.name == "access-control-request-method" &&
                !detail::isValidHttpCorsRequestMethod(field.value)) ||
            (field.name == "access-control-request-headers" &&
                !detail::isValidHttpCorsRequestHeaderNames(field.value)) ||
            (field.name == "expect" &&
                !detail::isValidReceivedHttpExpectFieldValue(field.value))) {
            return fail(state);
        }
    }
    if (field.name == "content-type") {
        if (state.contentTypeSeen || !isValidHttpContentTypeFieldValue(field.value)) {
            return fail(state);
        }
        state.contentTypeSeen = true;
    } else if (field.name == "content-encoding" &&
               !detail::isValidHttpContentEncodingFieldValue(
                   field.value, detail::HttpFieldListRole::kRecipient)) {
        return fail(state);
    }
    if (field.name == "trailer" &&
        !(state.kind == Http3MessageHeadKind::kRequest
                ? detail::isValidHttpRequestTrailerFieldValue(
                      field.value, detail::HttpFieldListRole::kRecipient)
                : detail::isValidHttpResponseTrailerFieldValue(
                      field.value, detail::HttpFieldListRole::kRecipient))) {
        return fail(state);
    }
    if (field.name == "content-length") {
        if (state.contentLength.parseField(field.value) != detail::HttpContentLengthParseStatus::kOk) {
            return fail(state);
        }
    }
    state.head.headers.emplace_back(field.name, field.value, state.head.headers.get_allocator().resource());
    return true;
}

std::optional<Http3MessageHeadError> finishHead(DecodeState& state) {
    auto& head = state.head;
    if (state.kind == Http3MessageHeadKind::kRequest) {
        if (!state.methodSeen) {
            return Http3MessageHeadError::kMessageError;
        }
        if (head.method == "CONNECT") {
            if (state.protocolSeen) {
                if (!state.schemeSeen || !state.authoritySeen || !state.pathSeen ||
                    !validPath(head.path, head.method) || !validAuthority(head.authority, head.scheme)) {
                    return Http3MessageHeadError::kMessageError;
                }
            } else {
                const auto tunnel = detail::parseHttpAuthority(head.authority);
                if (state.schemeSeen || state.pathSeen || !state.authoritySeen || !tunnel ||
                    tunnel->portKind() != detail::HttpAuthorityPortKind::kValue ||
                    *tunnel->port() == 0) {
                    return Http3MessageHeadError::kMessageError;
                }
            }
        } else {
            if (state.protocolSeen || !state.schemeSeen || !state.pathSeen || !validPath(head.path, head.method)) {
                return Http3MessageHeadError::kMessageError;
            }
            if (state.authoritySeen && state.hostSeen && head.authority != state.host) {
                return Http3MessageHeadError::kMessageError;
            }
            const bool authorityRequired = httpAsciiEqualsIgnoreCase(head.scheme, "http") ||
                                           httpAsciiEqualsIgnoreCase(head.scheme, "https");
            if ((state.authoritySeen && !validAuthority(head.authority, head.scheme)) ||
                (state.hostSeen && !validAuthority(state.host, head.scheme))) {
                return Http3MessageHeadError::kMessageError;
            }
            if (authorityRequired && !state.authoritySeen && !state.hostSeen) {
                return Http3MessageHeadError::kMessageError;
            }
            if (!state.authoritySeen && state.hostSeen) {
                head.authority.assign(state.host);
            }
        }
    } else if (!state.statusSeen) {
        return Http3MessageHeadError::kMessageError;
    }
    head.contentLength = state.contentLength.value();
    return {};
}

}  // namespace

Http3MessageHeader::Http3MessageHeader(std::pmr::memory_resource* resource)
    : name(resource),
      value(resource) {}

Http3MessageHeader::Http3MessageHeader(
    std::string_view headerName, std::string_view headerValue, std::pmr::memory_resource* resource)
    : name(headerName, resource),
      value(headerValue, resource) {}

Http3MessageHead::Http3MessageHead(std::pmr::memory_resource* resource)
    : method(resource),
      protocol(resource),
      scheme(resource),
      authority(resource),
      path(resource),
      headers(resource) {}

std::expected<Http3MessageHead, Http3MessageHeadError> decodeHttp3MessageHead(
    std::span<const char> fieldSection, Http3MessageHeadKind kind, std::pmr::memory_resource* resource,
    Http3MessageHeadLimits limits) {
    auto* owner = resource != nullptr ? resource : std::pmr::get_default_resource();
    Http3MessageHead head(owner);
    DecodeState state{head, kind, owner, limits.maxFieldSectionSize};
    const Http3FieldSectionLimits decoderLimits{limits.maxEncodedBytes, limits.maxFieldSectionSize,
        limits.maxFields};
    const auto decoded = decodeHttp3FieldSection(fieldSection, receiveField, &state, decoderLimits, owner);
    if (!decoded) {
        if (decoded.error() == Http3FieldSectionError::kFieldListTooLarge ||
            decoded.error() == Http3FieldSectionError::kFieldSectionTooLarge ||
            decoded.error() == Http3FieldSectionError::kTooManyFields) {
            return std::unexpected(Http3MessageHeadError::kFieldSectionTooLarge);
        }
        if (state.callbackRejected) {
            return std::unexpected(state.error);
        }
        return std::unexpected(Http3MessageHeadError::kQpackDecompressionFailed);
    }
    if (auto error = finishHead(state)) {
        return std::unexpected(*error);
    }
    return head;
}

std::expected<Http3DecodedMessageHead, Http3MessageHeadError> decodeHttp3MessageHead(
    Http3QpackDecoder& decoder, std::uint64_t streamId, std::span<const char> fieldSection,
    Http3MessageHeadKind kind, std::pmr::memory_resource* resource, Http3MessageHeadLimits limits) {
    auto* owner = resource ? resource : std::pmr::get_default_resource();
    Http3MessageHead head(owner);
    DecodeState state{head, kind, owner, limits.maxFieldSectionSize};
    if (fieldSection.size() > limits.maxEncodedBytes) {
        return std::unexpected(Http3MessageHeadError::kFieldSectionTooLarge);
    }
    const auto result = decoder.decode(streamId, fieldSection, receiveField, &state);
    if (!result) {
        return std::unexpected(result.error() == Http3QpackConnectionError::kLimit
                                   ? Http3MessageHeadError::kFieldSectionTooLarge
                                   : Http3MessageHeadError::kQpackDecompressionFailed);
    }
    if (result->status == Http3QpackDecodeStatus::kBlocked) {
        return Http3DecodedMessageHead{Http3QpackBlocked{}};
    }
    if (state.callbackRejected) {
        return std::unexpected(state.error);
    }
    if (result->fields > limits.maxFields) {
        return std::unexpected(Http3MessageHeadError::kFieldSectionTooLarge);
    }
    if (auto error = finishHead(state)) {
        return std::unexpected(*error);
    }
    return Http3DecodedMessageHead{std::move(head)};
}

}  // namespace ruvia
