#pragma once

#include <optional>
#include <string>
#include <string_view>

// How a request becomes the key its response is stored under. Two levels: the
// variant prefix identifies the URI, and the full key adds what makes one
// stored representation of that URI different from another. Purging by URI
// purges the prefix, which is why the prefix is a prefix.

namespace ruvia::edge {

// Everything that identifies the URI: method, mapping host, target. The
// terminal delimiter makes this an exact prefix -- purging `/a` must never
// invalidate `/ab`. The host is case-folded because URI hosts are
// case-insensitive.
[[nodiscard]] std::string cacheVariantPrefix(
    std::string_view method,
    std::string_view frontHost,
    std::string_view target);

// The key one stored representation lives under: the variant prefix, then the
// complete request authority, then the client's Accept-Encoding verbatim.
//
// The authority is the whole Host field, not the mapping host: routing ignores
// a port deliberately, but two target URIs with different ports are not the
// same cache key. Accept-Encoding is kept whole -- dropping weights, repeated
// lines, or the absent-versus-empty distinction would let a shared cache serve
// a representation selected for a different request.
[[nodiscard]] std::string cacheKeyFor(
    std::string_view variantPrefix,
    std::string_view host,
    const std::optional<std::string>& acceptEncoding);

}  // namespace ruvia::edge
