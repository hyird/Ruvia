#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/detail/parser/HttpUriGrammar.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool isLowerAlpha(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool isLowerAlphaNumeric(char value) noexcept {
    return isLowerAlpha(value) || isDecimalDigit(value);
}

[[nodiscard]] bool isLowerHexDigit(char value) noexcept {
    return isDecimalDigit(value) || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool isValidSerializedOriginScheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !isLowerAlpha(scheme.front())) {
        return false;
    }
    for (const auto value : scheme.substr(1)) {
        if (!isLowerAlphaNumeric(value) && value != '+' && value != '-' && value != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidSerializedOriginDomain(std::string_view domain) noexcept {
    if (domain.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < domain.size()) {
        const auto separator = domain.find('.', offset);
        const auto label = domain.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset);
        if (label.empty() || !isLowerAlphaNumeric(label.front()) || !isLowerAlphaNumeric(label.back())) {
            return false;
        }
        for (const auto value : label) {
            if (!isLowerAlphaNumeric(value) && value != '-') {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
    return false;
}

[[nodiscard]] bool isValidSerializedOriginH16(std::string_view group) noexcept {
    if (group.empty() || group.size() > 4 || (group.size() > 1 && group.front() == '0')) {
        return false;
    }
    return std::ranges::all_of(group, isLowerHexDigit);
}

[[nodiscard]] bool countSerializedOriginIpv6Groups(std::string_view side, std::size_t& count) noexcept {
    count = 0;
    if (side.empty()) {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = side.find(':', offset);
        const auto group = side.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset);
        if (!isValidSerializedOriginH16(group)) {
            return false;
        }
        ++count;
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
        if (offset == side.size()) {
            return false;
        }
    }
}

[[nodiscard]] bool isValidSerializedOriginIpv6(std::string_view literal) noexcept {
    const auto compression = literal.find("::");
    if (compression == std::string_view::npos) {
        std::size_t groups = 0;
        return countSerializedOriginIpv6Groups(literal, groups) && groups == 8;
    }
    if (literal.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    std::size_t leftGroups = 0;
    std::size_t rightGroups = 0;
    return countSerializedOriginIpv6Groups(literal.substr(0, compression), leftGroups) && countSerializedOriginIpv6Groups(literal.substr(compression + 2), rightGroups) && leftGroups + rightGroups <= 6;
}

[[nodiscard]] bool parseSerializedOriginPort(std::string_view value, std::uint16_t& port) noexcept {
    // A serialized URL port is the shortest decimal form of the URL record's
    // 16-bit port. Merely accepting five digits admits values such as 99999,
    // while accepting leading zeroes admits spellings no serializer can emit.
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    return parsePortValue(value, port);
}

[[nodiscard]] std::optional<std::uint16_t> serializedOriginDefaultPort(std::string_view scheme) noexcept {
    if (scheme == "ftp") {
        return 21;
    }
    if (scheme == "http" || scheme == "ws") {
        return 80;
    }
    if (scheme == "https" || scheme == "wss") {
        return 443;
    }
    return std::nullopt;
}

}  // namespace

bool isValidHttpSerializedOrigin(std::string_view value) noexcept {
    const auto schemeEnd = value.find("://");
    const auto scheme = value.substr(0, schemeEnd);
    if (schemeEnd == std::string_view::npos || !isValidSerializedOriginScheme(scheme)) {
        return false;
    }

    const auto authority = value.substr(schemeEnd + 3);
    if (authority.empty()) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool hasPort = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        host = authority.substr(1, close - 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return false;
            }
            hasPort = true;
            port = remainder.substr(1);
        }
        if (!isValidSerializedOriginIpv6(host)) {
            return false;
        }
    } else {
        const auto portSeparator = authority.find(':');
        if (portSeparator == std::string_view::npos) {
            host = authority;
        } else {
            if (authority.find(':', portSeparator + 1) != std::string_view::npos) {
                return false;
            }
            host = authority.substr(0, portSeparator);
            hasPort = true;
            port = authority.substr(portSeparator + 1);
        }
        if (!parseIpv4Address(host) && !isValidSerializedOriginDomain(host)) {
            return false;
        }
    }
    if (!hasPort) {
        return true;
    }
    std::uint16_t portValue = 0;
    if (!parseSerializedOriginPort(port, portValue)) {
        return false;
    }
    const auto defaultPort = serializedOriginDefaultPort(scheme);
    return !defaultPort.has_value() || portValue != *defaultPort;
}

}  // namespace ruvia::detail
