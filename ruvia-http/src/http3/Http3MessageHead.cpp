#include "ruvia/http/Http3MessageHead.h"

#include <limits>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/HttpMediaType.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia {
namespace {

bool isTokenChar(unsigned char ch) noexcept {
    if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
        return true;
    }
    constexpr std::string_view extra{"!#$%&'*+-.^_`|~"};
    return extra.find(static_cast<char>(ch)) != std::string_view::npos;
}

bool isToken(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!isTokenChar(ch)) {
            return false;
        }
    }
    return true;
}

bool validValue(std::string_view value) noexcept {
    for (const unsigned char ch : value) {
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool equalsAsciiCaseInsensitive(std::string_view value, std::string_view expected) noexcept {
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i] >= 'A' && value[i] <= 'Z' ? static_cast<char>(value[i] + ('a' - 'A')) : value[i];
        if (ch != expected[i]) {
            return false;
        }
    }
    return true;
}

bool validPath(std::string_view value, std::string_view method) noexcept {
    return (value == "*" && method == "OPTIONS") || isValidHttpOriginFormTarget(value);
}

bool validAuthority(std::string_view value, std::string_view scheme) noexcept {
    if (equalsAsciiCaseInsensitive(scheme, "http") ||
        equalsAsciiCaseInsensitive(scheme, "https")) {
        const auto host = parseHttpAuthorityHost(value);
        return host.has_value() && !host->empty();
    }
    for (const unsigned char ch : value) {
        if (ch <= 0x20 || ch == 0x7f || ch == '/' || ch == '?' || ch == '#' ||
            (ch == '@' && (scheme.empty() || equalsAsciiCaseInsensitive(scheme, "http") ||
                              equalsAsciiCaseInsensitive(scheme, "https")))) {
            return false;
        }
    }
    return !value.empty();
}

bool validScheme(std::string_view value) noexcept {
    if (value.empty() || !((value[0] >= 'a' && value[0] <= 'z') ||
                             (value[0] >= 'A' && value[0] <= 'Z'))) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool parseLength(std::string_view value, std::uint64_t& length) noexcept {
    value = trimOws(value);
    if (value.empty()) {
        return false;
    }
    length = 0;
    for (const unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (length > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        length = length * 10 + digit;
    }
    return true;
}

struct DecodeState final {
    DecodeState(Http3MessageHead& messageHead, Http3MessageHeadKind messageKind,
        std::pmr::memory_resource* resource, std::size_t maximumSize)
        : head(messageHead),
          kind(messageKind),
          maxSize(maximumSize),
          host(resource) {}

    Http3MessageHead& head;
    Http3MessageHeadKind kind;
    Http3MessageHeadError error{Http3MessageHeadError::kMessageError};
    std::size_t size{0};
    std::size_t maxSize;
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
    bool contentLengthSeen{false};
    std::uint64_t contentLength{0};
};

bool fail(DecodeState& state) noexcept {
    state.error = Http3MessageHeadError::kMessageError;
    state.callbackRejected = true;
    return false;
}

bool receiveField(void* opaque, Http3FieldSectionFieldView field) {
    auto& state = *static_cast<DecodeState*>(opaque);
    if (field.name.empty() || !validValue(field.value)) {
        return fail(state);
    }
    if (state.size > state.maxSize || field.name.size() > state.maxSize - state.size ||
        field.value.size() > state.maxSize - state.size - field.name.size() ||
        32 > state.maxSize - state.size - field.name.size() - field.value.size()) {
        state.error = Http3MessageHeadError::kFieldSectionTooLarge;
        state.callbackRejected = true;
        return false;
    }
    state.size += field.name.size() + field.value.size() + 32;

    for (const unsigned char ch : field.name) {
        if (ch >= 'A' && ch <= 'Z') {
            return fail(state);
        }
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
                if (!isToken(field.value) || !setText(state.head.method, state.methodSeen)) {
                    return fail(state);
                }
            } else if (field.name == ":protocol") {
                if (!isToken(field.value) ||
                    !setText(state.head.protocol, state.protocolSeen)) {
                    return fail(state);
                }
            } else if (field.name == ":scheme") {
                if (!validScheme(field.value) || !setText(state.head.scheme, state.schemeSeen)) {
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
    if (!isToken(field.name)) {
        return fail(state);
    }
    if (field.name == "connection" || field.name == "keep-alive" || field.name == "proxy-connection" ||
        field.name == "transfer-encoding" || field.name == "upgrade") {
        return fail(state);
    }
    if (field.name == "te" &&
        (state.kind != Http3MessageHeadKind::kRequest ||
            !equalsAsciiCaseInsensitive(trimOws(field.value), "trailers"))) {
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
        if ((field.name == "origin" && !detail::isValidHttpOriginFieldValue(field.value)) ||
            (field.name == "access-control-request-method" &&
                !detail::isValidHttpCorsRequestMethod(field.value)) ||
            (field.name == "access-control-request-headers" &&
                !detail::isValidHttpCorsRequestHeaderNames(field.value))) {
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
    if (field.name == "content-length") {
        std::string_view remaining = field.value;
        bool hadValue = false;
        while (true) {
            const auto comma = remaining.find(',');
            const auto part = comma == std::string_view::npos ? remaining : remaining.substr(0, comma);
            std::uint64_t parsed = 0;
            if (!parseLength(part, parsed) || (state.contentLengthSeen && parsed != state.contentLength)) {
                return fail(state);
            }
            state.contentLengthSeen = true;
            state.contentLength = parsed;
            hadValue = true;
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
        if (!hadValue) {
            return fail(state);
        }
    }
    state.head.headers.emplace_back(field.name, field.value, state.head.headers.get_allocator().resource());
    return true;
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
    if (kind == Http3MessageHeadKind::kRequest) {
        if (!state.methodSeen) {
            return std::unexpected(Http3MessageHeadError::kMessageError);
        }
        if (head.method == "CONNECT") {
            if (state.protocolSeen) {
                if (!state.schemeSeen || !state.authoritySeen || !state.pathSeen ||
                    !validPath(head.path, head.method) || !validAuthority(head.authority, head.scheme)) {
                    return std::unexpected(Http3MessageHeadError::kMessageError);
                }
            } else {
                const auto tunnel = detail::parseHttpAuthority(head.authority);
                if (state.schemeSeen || state.pathSeen || !state.authoritySeen || !tunnel ||
                    tunnel->portKind() != detail::HttpAuthorityPortKind::kValue ||
                    *tunnel->port() == 0) {
                    return std::unexpected(Http3MessageHeadError::kMessageError);
                }
            }
        } else {
            if (state.protocolSeen || !state.schemeSeen || !state.pathSeen || !validPath(head.path, head.method)) {
                return std::unexpected(Http3MessageHeadError::kMessageError);
            }
            if (state.authoritySeen && state.hostSeen && head.authority != state.host) {
                return std::unexpected(Http3MessageHeadError::kMessageError);
            }
            const bool authorityRequired = equalsAsciiCaseInsensitive(head.scheme, "http") ||
                                           equalsAsciiCaseInsensitive(head.scheme, "https");
            if ((state.authoritySeen && !validAuthority(head.authority, head.scheme)) ||
                (state.hostSeen && !validAuthority(state.host, head.scheme))) {
                return std::unexpected(Http3MessageHeadError::kMessageError);
            }
            if (authorityRequired && !state.authoritySeen && !state.hostSeen) {
                return std::unexpected(Http3MessageHeadError::kMessageError);
            }
            if (!state.authoritySeen && state.hostSeen) {
                head.authority.assign(state.host);
            }
        }
    } else if (!state.statusSeen) {
        return std::unexpected(Http3MessageHeadError::kMessageError);
    }
    if (state.contentLengthSeen) {
        head.contentLength = state.contentLength;
    }
    return head;
}

}  // namespace ruvia
