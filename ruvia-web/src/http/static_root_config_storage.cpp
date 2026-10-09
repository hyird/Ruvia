#include "http/static_root_config_storage.h"

#include <algorithm>
#include <string_view>

#include "http/static_root_options_validation.h"

namespace ruvia::detail {

namespace {

void lowercase_ascii(std::pmr::string& value) noexcept {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
}

}  // namespace

static_root_config_storage::static_root_config_storage(
    const static_root_config_storage& source_value, std::pmr::memory_resource* resource)
    : static_root_config_storage(resource) {
    cache_control_ = source_value.cache_control_;
    index_file_ = source_value.index_file_;
    default_content_type_ = source_value.default_content_type_;
    auto* const stored_resource = cache_control_.get_allocator().resource();
    mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = mime_types_.emplace_back(stored_resource);
        stored.extension_ = mime.extension_;
        stored.content_type_ = mime.content_type_;
    }
    file_type_kind_ = source_value.file_type_kind_;
    file_type_extensions_.reserve(source_value.file_type_extensions_.size());
    for (const auto& extension : source_value.file_type_extensions_) {
        file_type_extensions_.emplace_back(extension);
    }
    range_requests_ = source_value.range_requests_;
    response_validators_ = source_value.response_validators_;
    dotfiles_ = source_value.dotfiles_;
}

static_root_config_storage make_static_root_config_storage(
    const static_root_options& source_value, std::pmr::memory_resource* resource) {
    validate_static_root_options(source_value);
    return store_validated_static_root_config(source_value, resource);
}

static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value, std::pmr::memory_resource* resource) {
    static_root_config_storage result(resource);
    result.cache_control_ = source_value.cache_control_;
    result.index_file_ = source_value.index_file_;
    result.default_content_type_ = source_value.default_content_type_;
    auto* const stored_resource = result.cache_control_.get_allocator().resource();
    result.mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = result.mime_types_.emplace_back(stored_resource);
        if (!mime.extension_.starts_with('.')) {
            stored.extension_.push_back('.');
        }
        stored.extension_.append(mime.extension_);
        lowercase_ascii(stored.extension_);
        stored.content_type_ = mime.content_type_;
    }
    std::ranges::sort(result.mime_types_,
        [](const static_root_mime_type_storage& left, const static_root_mime_type_storage& right) {
            return left.extension_ < right.extension_;
        });

    result.file_type_kind_ = source_value.file_types_.kind_;
    if (result.file_type_kind_ == static_file_type_policy::kind_type::only) {
        result.file_type_extensions_.reserve(source_value.file_types_.extensions_.size());
        for (std::string_view extension : source_value.file_types_.extensions_) {
            if (extension.starts_with('.')) {
                extension.remove_prefix(1);
            }
            auto& stored = result.file_type_extensions_.emplace_back(extension);
            lowercase_ascii(stored);
        }
        std::ranges::sort(result.file_type_extensions_);
        result.file_type_extensions_.erase(std::ranges::unique(result.file_type_extensions_).begin(),
            result.file_type_extensions_.end());
    }
    result.range_requests_ = source_value.range_requests_;
    result.response_validators_ = source_value.response_validators_;
    result.dotfiles_ = source_value.dotfiles_;
    return result;
}

}  // namespace ruvia::detail
