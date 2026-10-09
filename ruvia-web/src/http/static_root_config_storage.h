#pragma once

#include <memory_resource>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/static_files.h"

namespace ruvia::detail {

struct static_root_mime_type_storage final {
    explicit static_root_mime_type_storage(std::pmr::memory_resource* resource)
        : extension_(pmr_resource_or_default(resource)),
          content_type_(pmr_resource_or_default(resource)) {}

    std::pmr::string extension_;
    std::pmr::string content_type_;
};

// Validated, normalized, owning static-root policy. application configuration keeps one
// process-level instance; snapshot rebuild jobs copy it onto the process PMR so
// they never borrow worker or request state while running off-thread.
struct static_root_config_storage final {
    explicit static_root_config_storage(std::pmr::memory_resource* resource)
        : cache_control_(pmr_resource_or_default(resource)),
          index_file_(pmr_resource_or_default(resource)),
          default_content_type_("application/octet-stream", pmr_resource_or_default(resource)),
          mime_types_(pmr_resource_or_default(resource)),
          file_type_extensions_(pmr_resource_or_default(resource)) {}

    static_root_config_storage(
        const static_root_config_storage& source_value, std::pmr::memory_resource* resource);
    static_root_config_storage(static_root_config_storage&&) noexcept = default;
    static_root_config_storage& operator=(const static_root_config_storage&) = delete;
    static_root_config_storage& operator=(static_root_config_storage&&) noexcept = default;

    std::pmr::string cache_control_;
    std::pmr::string index_file_;
    std::pmr::string default_content_type_;
    std::pmr::vector<static_root_mime_type_storage> mime_types_;
    static_file_type_policy::kind_type file_type_kind_{static_file_type_policy::kind_type::defaults};
    std::pmr::vector<std::pmr::string> file_type_extensions_;
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
};

[[nodiscard]] static_root_config_storage make_static_root_config_storage(
    const static_root_options& source_value, std::pmr::memory_resource* resource);

// The caller owns whole-config validation and invokes this only after every
// related policy has passed. This split lets application validate document_root_config
// atomically before the first owner-PMR allocation.
[[nodiscard]] static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
