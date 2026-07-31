#include "ruvia/edge/detail/proxy/ForwardHeaders.h"

namespace ruvia::edge {

std::pmr::vector<HttpHeaderView> buildForwardHeaders(std::span<const HttpHeaderView> requestHeaders, std::string_view clientAddress, std::string_view host, bool tlsEnabled, const CachedResponse* staleEntry, ForwardMode mode, std::pmr::memory_resource* resource) {
    std::pmr::vector<HttpHeaderView> forwardHeaders(resource);
    for (const auto& field : requestHeaders) {
        const auto name = field.name();
        if (isConnectionOrFramingField(name) || connectionNominates(requestHeaders, name) || iequals(name, "host") || (mode == ForwardMode::kCache && isConditionalOrRangeField(name)) || iequals(name, "via") || iequals(name, "forwarded") || istartsWith(name, "x-forwarded-")) {
            continue;
        }
        forwardHeaders.push_back(field);
    }
    if (!clientAddress.empty()) {
        forwardHeaders.emplace_back(std::string_view("X-Forwarded-For"), std::string_view(clientAddress));
    }
    if (!host.empty()) {
        forwardHeaders.emplace_back(std::string_view("X-Forwarded-Host"), host);
    }
    forwardHeaders.emplace_back(std::string_view("X-Forwarded-Proto"), tlsEnabled ? std::string_view("https") : std::string_view("http"));
    forwardHeaders.emplace_back(std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

    // Revalidate a stale entry with a conditional request when it carries a
    // validator, so an unchanged resource comes back as a bodyless 304.
    if (staleEntry) {
        if (const auto etag = findHeaderValue(staleEntry->headers, "etag")) {
            forwardHeaders.emplace_back(std::string_view("If-None-Match"), *etag);
        } else if (const auto lastModified = findHeaderValue(staleEntry->headers, "last-modified")) {
            forwardHeaders.emplace_back(std::string_view("If-Modified-Since"), *lastModified);
        }
    }
    return forwardHeaders;
}

}  // namespace ruvia::edge
