#pragma once

#include <memory_resource>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"

namespace ruvia::detail {
enum class StaticFileSelectionMode : std::uint8_t {
    kIdentityOnly,
    kPrecompressed,
};

}  // namespace ruvia::detail

namespace ruvia {

// Which bytes a static route actually serves: the identity file, a
// precompressed sidecar, or a refresh-built in-memory variant the client
// accepts.
class StaticFileRepresentation final {
public:
    StaticFileRepresentation(
        detail::StaticRootEntryView entry, HttpContentCoding contentCoding) noexcept
        : entry_(entry),
          contentCoding_(contentCoding) {}

    StaticFileRepresentation(detail::StaticRootEntryView entry,
        detail::StaticRootMemoryVariantView memoryVariant, HttpContentCoding contentCoding) noexcept
        : entry_(entry),
          memoryVariant_(memoryVariant),
          contentCoding_(contentCoding) {}

    [[nodiscard]] const detail::StaticRootEntryView& entry() const noexcept {
        return entry_;
    }

    [[nodiscard]] const detail::StaticRootMemoryVariantView* memoryVariant() const noexcept {
        return memoryVariant_.has_value() ? &*memoryVariant_ : nullptr;
    }

    [[nodiscard]] HttpContentCoding contentCoding() const noexcept {
        return contentCoding_;
    }

private:
    detail::StaticRootEntryView entry_;
    std::optional<detail::StaticRootMemoryVariantView> memoryVariant_;
    HttpContentCoding contentCoding_;
};

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index -- highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. Index lookups only (no per-request filesystem stat).
[[nodiscard]] std::optional<StaticFileRepresentation> selectStaticFileRepresentation(
    const StaticRoot& root, std::string_view relative, const HttpRequest& request,
    std::pmr::memory_resource* resource, detail::StaticRootEntryView identity,
    detail::StaticFileSelectionMode mode = detail::StaticFileSelectionMode::kIdentityOnly);

}  // namespace ruvia
