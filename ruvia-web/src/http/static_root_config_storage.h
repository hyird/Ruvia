#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/static_files.h"

namespace ruvia::detail {

struct static_root_mime_type_storage final {
    friend bool operator==(const static_root_mime_type_storage&, const static_root_mime_type_storage&) = default;

    std::string extension_;
    std::string content_type_;
};

// Validated, normalized, owning static-root policy.
struct static_root_config_storage final {
    static_root_config_storage() = default;
    static_root_config_storage(const static_root_config_storage&) = default;
    static_root_config_storage(static_root_config_storage&&) noexcept = default;
    static_root_config_storage& operator=(const static_root_config_storage&) = delete;
    static_root_config_storage& operator=(static_root_config_storage&&) noexcept = default;

    friend bool operator==(const static_root_config_storage&, const static_root_config_storage&) = default;

    std::string cache_control_;
    std::string index_file_;
    std::string default_content_type_{"application/octet-stream"};
    std::vector<static_root_mime_type_storage> mime_types_;
    static_file_type_policy::kind file_type_kind_{static_file_type_policy::kind::defaults};
    std::vector<std::string> file_type_extensions_;
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
};

[[nodiscard]] static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value);

[[nodiscard]] bool is_valid_static_file_extension(std::string_view extension) noexcept;

[[nodiscard]] bool file_type_allowed(
    std::string_view extension, const static_root_config_storage& config);

[[nodiscard]] std::string content_type_for(std::string_view extension,
    const static_root_config_storage& config);
}  // namespace ruvia::detail
