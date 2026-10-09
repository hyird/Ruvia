#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_media_type.h"
#include "ruvia/web/static_files.h"

#include "http/static_file_types.h"

namespace ruvia::detail {

[[nodiscard]] inline bool static_file_extensions_equivalent(
    std::string_view left, std::string_view right) noexcept {
    if (left.starts_with('.')) {
        left.remove_prefix(1);
    }
    if (right.starts_with('.')) {
        right.remove_prefix(1);
    }
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto left_character = left[i];
        auto right_character = right[i];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character + ('a' - 'A'));
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character + ('a' - 'A'));
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool static_root_serves_dotfiles(static_dotfile_policy policy) {
    switch (policy) {
        case static_dotfile_policy::deny:
            return false;
        case static_dotfile_policy::serve:
            return true;
        default:
            throw std::invalid_argument("invalid static dotfile policy");
    }
}

inline void validate_static_root_options(const static_root_options& options) {
    (void)static_root_serves_dotfiles(options.dotfiles_);
    switch (options.range_requests_) {
        case static_range_request_policy::ignore:
        case static_range_request_policy::honor:
            break;
        default:
            throw std::invalid_argument("invalid static range request policy");
    }
    switch (options.response_validators_) {
        case static_response_validator_policy::omit:
        case static_response_validator_policy::emit:
            break;
        default:
            throw std::invalid_argument("invalid static response validator policy");
    }
    if (!ruvia::is_valid_http_header_value(options.cache_control_) ||
        (!options.default_content_type_.empty() &&
            !ruvia::is_valid_http_content_type_field_value(options.default_content_type_))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (std::size_t i = 0; i < options.mime_types_.size(); ++i) {
        const auto& mime = options.mime_types_[i];
        if (!ruvia::detail::is_valid_static_file_extension(mime.extension_) ||
            mime.content_type_.empty() ||
            !ruvia::is_valid_http_content_type_field_value(mime.content_type_)) {
            throw std::invalid_argument("invalid static file mime type");
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (static_file_extensions_equivalent(
                    options.mime_types_[previous].extension_, mime.extension_)) {
                throw std::invalid_argument("duplicate static file mime extension");
            }
        }
    }
    switch (options.file_types_.kind_) {
        case static_file_type_policy::kind_type::defaults:
        case static_file_type_policy::kind_type::all:
            if (!options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type extensions require only mode");
            }
            break;
        case static_file_type_policy::kind_type::only:
            if (options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type allow-list must not be empty");
            }
            for (const auto& extension : options.file_types_.extensions_) {
                if (!is_valid_static_file_extension(extension)) {
                    throw std::invalid_argument("invalid static file type");
                }
            }
            break;
        default:
            throw std::invalid_argument("invalid static file type mode");
    }
    if ((options.index_file_.find('/') != std::string_view::npos) || (options.index_file_.find('\\') != std::string_view::npos) || options.index_file_ == "." ||
        options.index_file_ == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

}  // namespace ruvia::detail
