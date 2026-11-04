#include "http/static_file_variant.h"

#include <array>
#include <cstddef>
#include <cstring>

#include "ruvia/http/http_accept_encoding.h"

#include "http/static_file_metadata.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::static_file_version entry_version(const detail::static_root_entry_view& entry_value) noexcept {
    return {entry_value.modified_seconds(), entry_value.modified_token()};
}

}  // namespace

// A sidecar older than the identity entry is ignored: presence alone cannot
// prove that its decoded bytes still describe the current resource.

std::optional<static_file_representation> select_static_file_representation(const static_root& root,
    std::string_view relative, const http_response_coding_selection& selection,
    std::pmr::memory_resource* resource, detail::static_root_entry_view identity) {
    if (!selection.acceptable()) {
        return std::nullopt;
    }
    const bool has_sidecars = detail::static_root_access::has_sidecars(root);
    if ((!has_sidecars && !identity.has_memory_variants()) ||
        (!selection.accepts(http_content_coding::brotli) &&
            !selection.accepts(http_content_coding::zstd) &&
            !selection.accepts(http_content_coding::gzip))) {
        if (selection.identity_accepted()) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        return std::nullopt;
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
    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_path;
    const bool use_inline_path = has_sidecars && relative.size() + 4U <= inline_path.size();
    if (use_inline_path && !relative.empty()) {
        std::memcpy(inline_path.data(), relative.data(), relative.size());
    }
    std::pmr::string variant_path(resource);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& candidate = candidates[i];
        if (!selection.accepts(candidate.content_coding_)) {
            continue;
        }
        if (has_sidecars) {
            const auto suffix = detail::static_precompression_sidecars[i].suffix_;
            const std::size_t total_size = relative.size() + suffix.size();
            std::string_view lookup_path;
            if (use_inline_path) {
                std::memcpy(inline_path.data() + relative.size(), suffix.data(), suffix.size());
                lookup_path = std::string_view(inline_path.data(), total_size);
            } else {
                variant_path.reserve(total_size);
                variant_path.assign(relative.data(), relative.size());
                variant_path.append(suffix.data(), suffix.size());
                lookup_path = variant_path;
            }
            if (const auto entry_value = detail::static_root_access::find_variant(root, lookup_path);
                entry_value.has_value() &&
                entry_version(*entry_value).at_least_as_new_as(entry_version(identity))) {
                candidate.entry_ = *entry_value;
                available.include(candidate.content_coding_);
                continue;
            }
        }
        if (auto memory_variant = identity.memory_variant(candidate.content_coding_);
            memory_variant.has_value()) {
            candidate.memory_variant_ = *memory_variant;
            available.include(candidate.content_coding_);
        }
    }

    if (const auto chosen = selection.select(available); chosen.acceptable()) {
        if (chosen.coding() == http_content_coding::identity) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        for (const auto& candidate : candidates) {
            if (candidate.content_coding_ == chosen.coding() && candidate.entry_.has_value()) {
                return static_file_representation(*candidate.entry_, candidate.content_coding_);
            }
            if (candidate.content_coding_ == chosen.coding() &&
                candidate.memory_variant_.has_value()) {
                return static_file_representation(
                    identity, *candidate.memory_variant_, candidate.content_coding_);
            }
        }
    }

    return std::nullopt;
}

}  // namespace ruvia
