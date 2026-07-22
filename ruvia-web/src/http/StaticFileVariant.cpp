#include "ruvia/web/detail/http/StaticFileVariant.h"

#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/StaticFileMetadata.h"

namespace ruvia {

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index — highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. Index lookups only (no per-request filesystem stat).

StaticFileRepresentation selectStaticFileRepresentation(
    const StaticRoot& root,
    std::string_view relative,
    const HttpRequest& request,
    std::pmr::memory_resource* resource,
    detail::StaticRootEntryView identity) {
    StaticFileRepresentation selected(
        identity,
        detail::HttpContentCoding::kIdentity);
    detail::HttpResponseCodingQualities qualities;
    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }

    struct Candidate final {
        std::string_view suffix;
        detail::HttpContentCoding contentCoding;
        int score;
    };
    const Candidate candidates[] = {
        {".br",
         detail::HttpContentCoding::kBrotli,
         detail::httpAcceptedEncodingScore(qualities.brotli)},
        {".zst",
         detail::HttpContentCoding::kZstd,
         detail::httpAcceptedEncodingScore(qualities.zstd)},
        {".gz",
         detail::HttpContentCoding::kGzip,
         detail::httpAcceptedEncodingScore(qualities.gzip)},
    };

    int best = detail::httpAcceptedIdentityScore(qualities.identity);
    for (const auto& candidate : candidates) {
        if (candidate.score < best ||
            (candidate.score == best &&
             selected.contentCoding() != detail::HttpContentCoding::kIdentity)) {
            continue;
        }
        std::pmr::string variantPath(resource);
        variantPath.reserve(relative.size() + candidate.suffix.size());
        variantPath.assign(relative.data(), relative.size());
        variantPath.append(candidate.suffix.data(), candidate.suffix.size());
        if (const auto entry =
                detail::StaticRootAccess::findVariant(root, variantPath);
            entry.has_value()) {
            best = candidate.score;
            selected = StaticFileRepresentation(
                *entry,
                candidate.contentCoding);
        }
    }
    return selected;
}

}  // namespace ruvia
