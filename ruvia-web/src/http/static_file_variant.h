#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/static_files.h"

#include "http/static_root_index.h"

namespace ruvia::detail {
enum class static_file_selection_mode : std::uint8_t {
    identity_only,
    precompressed,
};

}  // namespace ruvia::detail

namespace ruvia {

// Which bytes a static route actually serves: the identity file, a
// precompressed sidecar, or a refresh-built in-memory variant the client
// accepts.
class static_file_representation final {
public:
    static_file_representation(
        detail::static_root_entry_view entry_value, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          content_coding_(content_coding) {}

    static_file_representation(detail::static_root_entry_view entry_value,
        detail::static_root_memory_variant_view memory_variant, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          memory_variant_(memory_variant),
          content_coding_(content_coding) {}

    [[nodiscard]] const detail::static_root_entry_view& entry() const noexcept {
        return entry_;
    }

    [[nodiscard]] const detail::static_root_memory_variant_view* memory_variant() const noexcept {
        return memory_variant_.has_value() ? &*memory_variant_ : nullptr;
    }

    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

private:
    detail::static_root_entry_view entry_;
    std::optional<detail::static_root_memory_variant_view> memory_variant_;
    http_content_coding content_coding_;
};

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) the client
// accepts and that exists in the index -- highest Accept-Encoding q-value wins,
// ties resolve br > zstd > gzip. The served bytes are the variant's, so its
// size/etag/modified describe the wire representation; the caller keeps the
// original Content-Type. Index lookups only (no per-request filesystem stat).
[[nodiscard]] std::optional<static_file_representation> select_static_file_representation(
    const static_root& root, std::string_view relative, const http_request& request,
    std::pmr::memory_resource* resource, detail::static_root_entry_view identity,
    detail::static_file_selection_mode mode = detail::static_file_selection_mode::identity_only);

}  // namespace ruvia
