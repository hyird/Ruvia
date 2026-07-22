#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/edge/detail/cache/EdgeCache.h"
namespace ruvia::edge {

// Who owns revalidation and ranging on this fetch.
enum class ForwardMode : unsigned char {
    // The edge caches the response, so it issues its own conditionals and
    // serves ranges from the stored body: the client's are dropped.
    kCache,
    // The response is not cached, so the client's conditionals and Range are
    // the origin's to answer and pass through untouched.
    kPassThrough,
};

// The header list the edge sends upstream, built from the client's: hop-by-hop
// fields and Host are dropped (the edge regenerates Host), client conditionals
// and Range are dropped in kCache mode, and client-supplied forwarding fields
// are dropped so a client cannot spoof them. Accept-Encoding is
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
    ForwardMode mode,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::edge
