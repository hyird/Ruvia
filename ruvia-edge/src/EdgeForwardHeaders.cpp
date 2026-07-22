#include "ruvia/edge/detail/EdgeForwardHeaders.h"

namespace ruvia::edge {

std::pmr::vector<HttpHeaderView> buildOriginForwardHeaders(
    std::span<const HttpHeaderView> requestHeaders,
    std::string_view clientAddress,
    std::string_view host,
    bool tlsEnabled,
    const CachedResponse* staleEntry,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<HttpHeaderView> forwardHeaders(resource);
    for (const auto& field : requestHeaders) {
        std::pmr::string lower(resource);
        lower.reserve(field.name().size());
        for (const char c : field.name()) {
            lower.push_back(toLowerAscii(c));
        }
        if (isConnectionOrFramingField(lower) ||
            connectionNominates(requestHeaders, field.name()) ||
            lower == "host" ||
            lower == "range" || lower == "if-none-match" ||
            lower == "if-modified-since" || lower == "if-match" ||
            lower == "if-unmodified-since" || lower == "if-range" ||
            lower == "via" || lower == "forwarded" ||
            lower.starts_with("x-forwarded-")) {
            continue;
        }
        forwardHeaders.push_back(field);
    }
    if (!clientAddress.empty()) {
        forwardHeaders.emplace_back(
            std::string_view("X-Forwarded-For"),
            std::string_view(clientAddress));
    }
    if (!host.empty()) {
        forwardHeaders.emplace_back(
            std::string_view("X-Forwarded-Host"), host);
    }
    forwardHeaders.emplace_back(
        std::string_view("X-Forwarded-Proto"),
        tlsEnabled ? std::string_view("https") : std::string_view("http"));
    forwardHeaders.emplace_back(
        std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

    // Revalidate a stale entry with a conditional request when it carries a
    // validator, so an unchanged resource comes back as a bodyless 304.
    if (staleEntry) {
        if (const auto etag = findHeaderValue(staleEntry->headers, "etag")) {
            forwardHeaders.emplace_back(std::string_view("If-None-Match"), *etag);
        } else if (const auto lastModified =
                       findHeaderValue(staleEntry->headers, "last-modified")) {
            forwardHeaders.emplace_back(
                std::string_view("If-Modified-Since"), *lastModified);
        }
    }
    return forwardHeaders;
}

}  // namespace ruvia::edge
