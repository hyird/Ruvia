#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/native_path.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_response_file.h"
#include "ruvia/web/static_files.h"

#include "http/static_root_config_storage.h"

namespace ruvia::detail {

struct static_root_precompression_options final {
    bool gzip_{false};
    bool brotli_{false};
    bool zstd_{false};
    std::size_t min_bytes_{1024};
    std::size_t max_bytes_{std::size_t{256} * 1024};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return gzip_ || brotli_ || zstd_;
    }
};

struct static_root_memory_variant final {
    explicit static_root_memory_variant(std::pmr::memory_resource* resource)
        : bytes_(resource),
          etag_(resource),
          last_modified_(resource) {}

    http_content_coding content_coding_{http_content_coding::identity};
    std::pmr::string bytes_;
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::pmr::string etag_;
    std::pmr::string last_modified_;
};

struct static_root_entry final {
    explicit static_root_entry(std::pmr::memory_resource* resource)
        : relative_path_(resource),
          file_path_(resource),
          content_type_(resource),
          etag_(resource),
          last_modified_(resource),
          memory_variants_(resource) {}

    std::pmr::string relative_path_;
    ruvia::native_path_string_type file_path_;
    std::pmr::string content_type_;
    std::uint64_t size_{0};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::pmr::string etag_;
    std::pmr::string last_modified_;
    bool directly_servable_{true};
    std::pmr::vector<static_root_memory_variant> memory_variants_;
};

struct static_root_state final {
    ruvia::native_path_string_type root_;
    static_root_config_storage config_;
    std::pmr::vector<static_root_entry> entries_;
    std::pmr::vector<std::pmr::string> directories_;
    // Refresh request leases are charged to this worker-owned snapshot, not to
    // the server globally. The refresh loop can therefore reclaim unrelated
    // retired snapshots while a long request still holds an older one.
    // Application-owned immutable roots outlive all workers and never touch
    // this counter from their concurrent request paths.
    std::size_t active_bindings_{0};
    std::uint64_t fingerprint_{0};

    static_root_state(std::pmr::memory_resource* resource, static_root_config_storage configured_policy)
        : root_(resource),
          config_(std::move(configured_policy)),
          entries_(resource),
          directories_(resource) {}
};

class static_root_memory_variant_view final {
public:
    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

private:
    friend class static_root_entry_view;

    static_root_memory_variant_view(http_content_coding content_coding, std::string_view bytes_value,
        std::uint64_t modified_token, std::time_t modified_seconds, std::string_view etag,
        std::string_view last_modified) noexcept
        : content_coding_(content_coding),
          bytes_(bytes_value),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          etag_(etag),
          last_modified_(last_modified) {}

    http_content_coding content_coding_;
    std::string_view bytes_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    std::string_view etag_;
    std::string_view last_modified_;
};

class static_root_entry_view final {
public:
    [[nodiscard]] const ruvia::native_path_char_type* file_path() const noexcept {
        return file_path_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view get_cache_control() const noexcept {
        return cache_control_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] static_range_request_policy range_requests() const noexcept {
        return range_requests_;
    }

    [[nodiscard]] static_response_validator_policy response_validators() const noexcept {
        return response_validators_;
    }

    [[nodiscard]] std::optional<static_root_memory_variant_view> memory_variant(
        http_content_coding coding) const noexcept {
        if (memory_variants_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& variant : *memory_variants_) {
            if (variant.content_coding_ == coding) {
                return static_root_memory_variant_view(variant.content_coding_, variant.bytes_,
                    variant.modified_token_, variant.modified_seconds_, variant.etag_,
                    variant.last_modified_);
            }
        }
        return std::nullopt;
    }

private:
    friend class static_root_access;

    static_root_entry_view(const ruvia::native_path_char_type* file_path, std::string_view content_type_value,
        std::string_view cache_control_value, std::string_view etag, std::string_view last_modified,
        std::uint64_t size, http_response_file_identity identity, std::uint64_t modified_token,
        std::time_t modified_seconds, static_range_request_policy range_requests,
        static_response_validator_policy response_validators, bool directly_servable,
        const std::pmr::vector<static_root_memory_variant>* memory_variants) noexcept
        : file_path_(file_path),
          content_type_(content_type_value),
          cache_control_(cache_control_value),
          etag_(etag),
          last_modified_(last_modified),
          size_(size),
          identity_(identity),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          range_requests_(range_requests),
          response_validators_(response_validators),
          directly_servable_(directly_servable),
          memory_variants_(memory_variants) {}

    const ruvia::native_path_char_type* file_path_;
    std::string_view content_type_;
    std::string_view cache_control_;
    std::string_view etag_;
    std::string_view last_modified_;
    std::uint64_t size_;
    http_response_file_identity identity_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    static_range_request_policy range_requests_;
    static_response_validator_policy response_validators_;
    bool directly_servable_;
    const std::pmr::vector<static_root_memory_variant>* memory_variants_;
};

class static_root_access final {
public:
    [[nodiscard]] static std::unique_ptr<static_root, pmr_object_deleter<static_root>> make(
        std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
        const static_root_config_storage& config);
    [[nodiscard]] static std::unique_ptr<static_root, pmr_object_deleter<static_root>> make(
        std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
        static_root_config_storage&& config);
    [[nodiscard]] static std::unique_ptr<static_root, pmr_object_deleter<static_root>> clone(
        std::pmr::memory_resource* object_resource, const static_root& source);
    [[nodiscard]] static static_root_config_storage copy_config(
        const static_root& root, std::pmr::memory_resource* resource);
    [[nodiscard]] static std::string_view index_file(const static_root& root) noexcept;
    [[nodiscard]] static bool has_directory_index(const static_root& root) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find_variant(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static bool is_indexed_directory(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static std::uint64_t fingerprint(const static_root& root) noexcept;
    [[nodiscard]] static bool same_snapshot(
        const static_root& left, const static_root& right) noexcept;
    static void acquire_binding(const static_root& root) noexcept;
    static void release_binding(const static_root& root) noexcept;
    [[nodiscard]] static bool has_active_bindings(const static_root& root) noexcept;
    static void install_precompressed_variants(static_root& root, const static_root* previous,
        const static_root_precompression_options& options);

private:
    [[nodiscard]] static std::unique_ptr<static_root, pmr_object_deleter<static_root>> construct(
        std::pmr::memory_resource* object_resource, static_root::prepared_construction_type prepared);
};

}  // namespace ruvia::detail
