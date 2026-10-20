#include "http/static_file_variant.h"

#include <array>
#include <cstddef>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_ascii.h"

#include "http/static_file_metadata.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::static_file_version entry_version(const detail::static_root_entry_view& entry_value) noexcept {
    return {entry_value.modified_seconds(), entry_value.modified_token()};
}

}  // namespace

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index — highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. A sidecar older than the identity entry is ignored:
// presence alone cannot prove that its decoded bytes still describe the current
// resource. Index lookups only (no per-request filesystem stat).

std::optional<static_file_representation> select_static_file_representation(const static_root& root,
    std::string_view relative, const http_request& request, std::pmr::memory_resource* resource,
    detail::static_root_entry_view identity, detail::static_file_selection_mode mode) {
    if (mode == detail::static_file_selection_mode::identity_only) {
        return static_file_representation(identity, http_content_coding::identity);
    }

    http_response_coding_qualities qualities;
    if (request.header("Accept-Encoding").has_value()) {
        const auto headers = request.headers();
        for (std::size_t i = 0; i < headers.size(); ++i) {
            if (http_ascii_equals_ignore_case(headers[i].name(), "Accept-Encoding")) {
                qualities.update(headers[i].value());
            }
        }
    }

    struct candidate final {
        http_content_coding content_coding_;
        std::optional<detail::static_root_entry_view> entry_;
        std::optional<detail::static_root_memory_variant_view> memory_variant_;
    };
    std::array<candidate, detail::static_precompression_sidecars.size()> candidates{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        candidates[i].content_coding_ = detail::static_precompression_sidecars[i].content_coding_;
    }

    auto available = http_response_coding_candidates::identity_only();
    std::pmr::string variant_path(resource);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& candidate = candidates[i];
        if (!qualities.accepts(candidate.content_coding_)) {
            continue;
        }
        const auto suffix = detail::static_precompression_sidecars[i].suffix_;
        variant_path.reserve(relative.size() + suffix.size());
        variant_path.assign(relative.data(), relative.size());
        variant_path.append(suffix.data(), suffix.size());
        if (const auto entry_value = detail::static_root_access::find_variant(root, variant_path);
            entry_value.has_value()) {
            if (!entry_version(*entry_value).at_least_as_new_as(entry_version(identity))) {
                if (auto memory_variant = identity.memory_variant(candidate.content_coding_);
                    memory_variant.has_value()) {
                    candidate.memory_variant_ = *memory_variant;
                    available.include(candidate.content_coding_);
                }
                continue;
            }
            candidate.entry_ = *entry_value;
            available.include(candidate.content_coding_);
            continue;
        }
        if (auto memory_variant = identity.memory_variant(candidate.content_coding_);
            memory_variant.has_value()) {
            candidate.memory_variant_ = *memory_variant;
            available.include(candidate.content_coding_);
        }
    }

    const auto selection_result = http_response_coding_selection::select(qualities, available);
    if (const auto* selection = selection_result.selected()) {
        if (selection->coding() == http_content_coding::identity) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        for (const auto& candidate : candidates) {
            if (candidate.content_coding_ == selection->coding() && candidate.entry_.has_value()) {
                return static_file_representation(*candidate.entry_, candidate.content_coding_);
            }
            if (candidate.content_coding_ == selection->coding() &&
                candidate.memory_variant_.has_value()) {
                return static_file_representation(
                    identity, *candidate.memory_variant_, candidate.content_coding_);
            }
        }
    }

    return std::nullopt;
}

}  // namespace ruvia
