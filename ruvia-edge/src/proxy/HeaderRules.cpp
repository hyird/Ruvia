#include "ruvia/edge/detail/proxy/HeaderRules.h"

#include <algorithm>

#include "ruvia/http/detail/HttpConnectionFields.h"

namespace ruvia::edge {

namespace {

[[nodiscard]] std::string lowerCopy(std::string_view value) {
    std::string lower(value);
    for (auto& c : lower) {
        c = toLowerAscii(c);
    }
    return lower;
}

}  // namespace

bool isConnectionOrFramingField(std::string_view lowerName) noexcept {
    return lowerName == "connection" || lowerName == "keep-alive" ||
        lowerName == "proxy-authenticate" || lowerName == "proxy-authorization" ||
        lowerName == "te" || lowerName == "trailer" ||
        lowerName == "transfer-encoding" || lowerName == "upgrade" ||
        lowerName == "content-length";
}

bool connectionNominates(
    std::span<const HttpHeaderView> headers,
    std::string_view fieldName) noexcept {
    ruvia::detail::HttpConnectionOptions options;
    bool nominated = false;
    for (const auto& field : headers) {
        if (!iequals(field.name(), "connection")) {
            continue;
        }
        (void)options.parseField(
            field.value(),
            ruvia::detail::HttpFieldListRole::kRecipient,
            [&](std::string_view option) noexcept {
                nominated = nominated || iequals(option, fieldName);
                return true;
            });
    }
    return nominated;
}

bool connectionNominates(
    const Headers& headers,
    std::string_view fieldName) noexcept {
    ruvia::detail::HttpConnectionOptions options;
    bool nominated = false;
    for (const auto& [name, value] : headers) {
        if (!iequals(name, "connection")) {
            continue;
        }
        (void)options.parseField(
            value,
            ruvia::detail::HttpFieldListRole::kRecipient,
            [&](std::string_view option) noexcept {
                nominated = nominated || iequals(option, fieldName);
                return true;
            });
    }
    return nominated;
}

Headers endToEndResponseHeaders(const Headers& headers) {
    Headers result;
    result.reserve(headers.size());
    for (const auto& field : headers) {
        const std::string lower = lowerCopy(field.first);
        const bool standardHopByHop =
            isConnectionOrFramingField(lower) && lower != "content-length";
        if (standardHopByHop || connectionNominates(headers, field.first)) {
            continue;
        }
        result.push_back(field);
    }
    return result;
}

Headers mergeStoredHeaders(const Headers& stored, const Headers& updates) {
    Headers merged = endToEndResponseHeaders(stored);
    const Headers sanitizedUpdates = endToEndResponseHeaders(updates);
    for (const auto& [name, value] : sanitizedUpdates) {
        if (isConnectionOrFramingField(lowerCopy(name))) {
            continue;
        }
        std::erase_if(merged, [&](const auto& field) { return iequals(field.first, name); });
        merged.emplace_back(name, value);
    }
    return merged;
}

bool cacheableUnderVary(const Headers& headers) {
    for (const auto& [name, value] : headers) {
        if (!iequals(name, "vary")) {
            continue;
        }
        const std::string_view vary(value);
        std::size_t start = 0;
        while (start <= vary.size()) {
            const std::size_t comma = vary.find(',', start);
            const std::string_view token = vary.substr(
                start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            start = comma == std::string_view::npos ? vary.size() + 1 : comma + 1;
            std::string field;
            for (const char c : token) {
                if (c != ' ' && c != '\t') {
                    field.push_back(toLowerAscii(c));
                }
            }
            if (field.empty()) {
                continue;
            }
            if (field != "accept-encoding") {
                return false;  // "*" or a field the cache key does not cover
            }
        }
    }
    return true;
}

std::optional<std::string_view> findHeaderValue(
    const Headers& headers,
    std::string_view name) {
    for (const auto& [n, v] : headers) {
        if (iequals(n, name)) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> findRequestHeader(
    std::span<const HttpHeaderView> headers,
    std::string_view name) {
    for (const auto& field : headers) {
        if (iequals(field.name(), name)) {
            return field.value();
        }
    }
    return std::nullopt;
}

std::optional<std::string> combinedRequestFieldValue(
    std::span<const HttpHeaderView> headers,
    std::string_view name) {
    std::optional<std::string> combined;
    for (const auto& field : headers) {
        if (!iequals(field.name(), name)) {
            continue;
        }
        if (!combined) {
            combined.emplace();
        } else {
            combined->append(", ");
        }
        combined->append(field.value());
    }
    return combined;
}

std::string_view hostWithoutPort(std::string_view host) noexcept {
    if (host.empty()) {
        return host;
    }
    if (host.front() == '[') {
        const auto close = host.find(']');
        return close == std::string_view::npos ? host : host.substr(0, close + 1);
    }
    const auto colon = host.rfind(':');
    return colon == std::string_view::npos ? host : host.substr(0, colon);
}

}  // namespace ruvia::edge
