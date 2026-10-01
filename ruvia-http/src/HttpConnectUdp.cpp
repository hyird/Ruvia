#include "ruvia/http/HttpConnectUdp.h"

#include <array>
#include <charconv>

#include "ruvia/http/HttpDatagram.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/parser/HttpUriGrammar.h"

namespace ruvia {
namespace {
constexpr std::string_view prefix{"/.well-known/masque/udp/"};
bool validHost(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos &&
           (host.find(':') != std::string_view::npos ? detail::isValidIpv6Literal(host) : detail::isValidRegName(host));
}
bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}
struct HandshakeFields {
    bool capsule{false}, upgrade{false}, connection{false}, connectionField{false}, host{false}, forbidden{false};
};
std::expected<HandshakeFields, HttpConnectUdpError> fields(std::span<const HttpHeaderView> headers) {
    HandshakeFields found;
    for (const auto& field : headers) {
        if (!isValidHttpHeaderName(field.name()) || !isValidHttpHeaderValue(field.value())) {
            return std::unexpected(HttpConnectUdpError::kInvalidRequest);
        }
        if (same(field.name(), "capsule-protocol")) {
            const auto value = parseHttpCapsuleProtocol(field.value());
            if (found.capsule || !value || !*value) {
                return std::unexpected(HttpConnectUdpError::kInvalidCapsuleProtocol);
            }
            found.capsule = true;
        } else if (same(field.name(), "upgrade")) {
            if (found.upgrade || !same(field.value(), "connect-udp")) {
                return std::unexpected(HttpConnectUdpError::kInvalidRequest);
            }
            found.upgrade = true;
        } else if (same(field.name(), "connection")) {
            found.connectionField = true;
            detail::HttpConnectionOptions options;
            if (options.parseField(field.value(), detail::HttpFieldListRole::kRecipient) != detail::HttpFieldListParseStatus::kOk) {
                return std::unexpected(HttpConnectUdpError::kInvalidRequest);
            }
            found.connection = found.connection || options.contains(detail::HttpConnectionOption::kUpgrade);
        } else if (same(field.name(), "host")) {
            if (found.host || field.value().empty()) {
                return std::unexpected(HttpConnectUdpError::kInvalidRequest);
            }
            found.host = true;
        } else if (same(field.name(), "content-length") || same(field.name(), "transfer-encoding") ||
                   same(field.name(), "content-type") || same(field.name(), "content-encoding") || same(field.name(), "trailer")) {
            found.forbidden = true;
        }
    }
    return found;
}
}  // namespace
std::expected<std::pmr::string, HttpConnectUdpError> encodeHttpConnectUdpPath(HttpConnectUdpTargetView target, std::pmr::memory_resource* resource) {
    if (!validHost(target.host) || !target.port) {
        return std::unexpected(HttpConnectUdpError::kInvalidTarget);
    }
    std::pmr::string result(prefix, resource ? resource : std::pmr::get_default_resource());
    constexpr char hex[]{"0123456789ABCDEF"};
    for (const unsigned char c : target.host) {
        if (detail::isUnreservedByte(c)) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        }
    }
    std::array<char, 5> port{};
    const auto number = std::to_chars(port.data(), port.data() + port.size(), target.port);
    result.push_back('/');
    result.append(port.data(), number.ptr);
    result.push_back('/');
    return result;
}
std::expected<HttpConnectUdpTarget, HttpConnectUdpError> parseHttpConnectUdpPath(std::string_view path, std::pmr::memory_resource* resource) {
    if (!path.starts_with(prefix) || !path.ends_with('/')) {
        return std::unexpected(HttpConnectUdpError::kInvalidPath);
    }
    path.remove_prefix(prefix.size());
    path.remove_suffix(1);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return std::unexpected(HttpConnectUdpError::kInvalidPath);
    }
    const auto rawHost = path.substr(0, slash);
    // Colons in IPv6 addresses must be escaped in this URI-template variable.
    if (rawHost.find(':') != std::string_view::npos) {
        return std::unexpected(HttpConnectUdpError::kInvalidTarget);
    }
    auto host = decodeUrlComponent(rawHost, {.resource = resource});
    const auto port = detail::parsePortValue(path.substr(slash + 1));
    if (!host || !validHost(*host) || !port || !*port) {
        return std::unexpected(HttpConnectUdpError::kInvalidTarget);
    }
    return HttpConnectUdpTarget{std::move(*host), *port};
}
std::expected<void, HttpConnectUdpError> validateHttpConnectUdpRequest(HttpConnectUdpRequestView request) noexcept {
    const auto authority = parseHttpAuthorityHost(request.authority);
    if (!authority || authority->empty() || !isValidHttpOriginFormTarget(request.path)) {
        return std::unexpected(HttpConnectUdpError::kInvalidRequest);
    }
    const auto parsed = fields(request.headers);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->capsule || parsed->forbidden || request.authority.empty() || request.path.empty()) {
        return std::unexpected(HttpConnectUdpError::kInvalidRequest);
    }
    if (request.version == HttpProtocolVersion::kHttp10) {
        return std::unexpected(HttpConnectUdpError::kInvalidRequest);
    }
    if (request.version != HttpProtocolVersion::kHttp11) {
        if (request.method != "CONNECT" || request.protocol != "connect-udp" || request.scheme.empty() || parsed->connectionField || parsed->upgrade) {
            return std::unexpected(HttpConnectUdpError::kInvalidRequest);
        }
    } else if (request.method != "GET" || !parsed->upgrade || !parsed->connection || !parsed->host) {
        return std::unexpected(HttpConnectUdpError::kInvalidRequest);
    }
    return {};
}
std::expected<void, HttpConnectUdpError> validateHttpConnectUdpResponse(HttpProtocolVersion version, std::uint16_t status, std::span<const HttpHeaderView> headers) noexcept {
    if (version == HttpProtocolVersion::kHttp10) {
        return std::unexpected(HttpConnectUdpError::kInvalidResponse);
    }
    const bool extendedConnect = version != HttpProtocolVersion::kHttp11;
    const auto parsed = fields(headers);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->capsule || parsed->forbidden ||
        (extendedConnect ? status < 200 || status >= 300 || parsed->upgrade || parsed->connectionField : status != 101 || !parsed->upgrade || !parsed->connection)) {
        return std::unexpected(HttpConnectUdpError::kInvalidResponse);
    }
    return {};
}
}  // namespace ruvia
