#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/edge/detail/cache/EdgeCache.h"
#include "ruvia/edge/detail/proxy/HeaderRules.h"

namespace ruvia::edge {

// The header list the edge sends upstream, built from the client's: hop-by-hop
// fields, Host and the client's conditionals and Range are dropped (the edge
// regenerates Host and owns revalidation), and client-supplied forwarding
// fields are dropped so a client cannot spoof them. Accept-Encoding is
// forwarded so the origin may compress; the cache key covers every field line
// and weight, so variants stay separate.
//
// When `staleEntry` is non-null its validator is added, turning the fetch into
// a revalidation an unchanged resource answers with a bodyless 304.
//
// The returned views borrow from `request*` and from `staleEntry`, both of
// which must outlive the fetch.
[[nodiscard]] std::pmr::vector<HttpHeaderView> buildForwardHeaders(
    std::span<const HttpHeaderView> requestHeaders,
    std::string_view clientAddress,
    std::string_view host,
    bool tlsEnabled,
    const CachedResponse* staleEntry,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::edge
