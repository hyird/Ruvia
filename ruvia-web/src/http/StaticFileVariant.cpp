#include "ruvia/web/detail/http/static/StaticFileVariant.h"

#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"

namespace ruvia {
namespace {

[[nodiscard]] bool precompressedVariantIsAtLeastAsNew(const detail::StaticRootEntryView& identity, const detail::StaticRootEntryView& variant) noexcept {
    if (variant.modifiedSeconds() != identity.modifiedSeconds()) {
        return variant.modifiedSeconds() > identity.modifiedSeconds();
    }
    return variant.modifiedToken() >= identity.modifiedToken();
}

}  // namespace

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index — highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. A sidecar older than the identity entry is ignored:
// presence alone cannot prove that its decoded bytes still describe the current
// resource. Index lookups only (no per-request filesystem stat).

std::optional<StaticFileRepresentation> selectStaticFileRepresentation(const StaticRoot& root, std::string_view relative, const HttpRequest& request, std::pmr::memory_resource* resource, detail::StaticRootEntryView identity, detail::StaticFileSelectionMode mode) {
    if (mode == detail::StaticFileSelectionMode::kIdentityOnly) {
        return StaticFileRepresentation(identity, HttpContentCoding::kIdentity);
    }

    detail::HttpResponseCodingQualities qualities;
    for (const auto& header : request.headers()) {
        if (detail::httpAsciiEqualsIgnoreCase(header.name(), "Accept-Encoding")) {
            qualities.update(header.value());
        }
    }

    struct Candidate final {
        std::string_view suffix;
        HttpContentCoding contentCoding;
        std::optional<detail::StaticRootEntryView> entry;
        std::optional<detail::StaticRootMemoryVariantView> memoryVariant;
    };
    Candidate candidates[] = {
        {".br", HttpContentCoding::kBrotli, std::nullopt, std::nullopt},
        {".zst", HttpContentCoding::kZstd, std::nullopt, std::nullopt},
        {".gz", HttpContentCoding::kGzip, std::nullopt, std::nullopt},
    };

    auto available = detail::HttpResponseCodingCandidates::identityOnly();
    for (auto& candidate : candidates) {
        if (!qualities.accepts(candidate.contentCoding)) {
            continue;
        }
        std::pmr::string variantPath(resource);
        variantPath.reserve(relative.size() + candidate.suffix.size());
        variantPath.assign(relative.data(), relative.size());
        variantPath.append(candidate.suffix.data(), candidate.suffix.size());
        if (const auto entry = detail::StaticRootAccess::findVariant(root, variantPath); entry.has_value()) {
            if (!precompressedVariantIsAtLeastAsNew(identity, *entry)) {
                if (auto memoryVariant = identity.memoryVariant(candidate.contentCoding); memoryVariant.has_value()) {
                    candidate.memoryVariant = *memoryVariant;
                    available.include(candidate.contentCoding);
                }
                continue;
            }
            candidate.entry = *entry;
            available.include(candidate.contentCoding);
            continue;
        }
        if (auto memoryVariant = identity.memoryVariant(candidate.contentCoding); memoryVariant.has_value()) {
            candidate.memoryVariant = *memoryVariant;
            available.include(candidate.contentCoding);
        }
    }

    const auto selectionResult = detail::HttpResponseCodingSelection::select(qualities, available);
    if (const auto* selection = selectionResult.selected()) {
        if (selection->coding() == HttpContentCoding::kIdentity) {
            return StaticFileRepresentation(identity, HttpContentCoding::kIdentity);
        }
        for (const auto& candidate : candidates) {
            if (candidate.contentCoding == selection->coding() && candidate.entry.has_value()) {
                return StaticFileRepresentation(*candidate.entry, candidate.contentCoding);
            }
            if (candidate.contentCoding == selection->coding() && candidate.memoryVariant.has_value()) {
                return StaticFileRepresentation(identity, *candidate.memoryVariant, candidate.contentCoding);
            }
        }
    }

    return std::nullopt;
}

}  // namespace ruvia
