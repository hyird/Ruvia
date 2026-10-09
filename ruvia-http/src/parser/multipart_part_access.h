#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/multipart_parser.h"

namespace ruvia::detail {

struct multipart_part_access final {
    // name/filename arrive quote-trimmed; decode their RFC 7230 §3.2.6 quoted-pairs
    // into part-owned storage (they may differ from the raw bytes). content_type/body
    // stay borrowed views into the request body.
    [[nodiscard]] static multipart_part make(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, std::pmr::memory_resource* resource) {
        return make(name, filename, content_type_value, body, !filename.empty(), resource);
    }

    [[nodiscard]] static multipart_part make(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, bool filename_present,
        std::pmr::memory_resource* resource) {
        std::pmr::string decoded_name(resource);
        http_append_decoded_quoted_pairs(decoded_name, name);
        std::pmr::string decoded_filename(resource);
        http_append_decoded_quoted_pairs(decoded_filename, filename);
        return multipart_part(
            std::move(decoded_name), std::move(decoded_filename), content_type_value, body, filename_present);
    }

    template <http_temporary_owning_char_string content_type>
    static multipart_part make(std::string_view, std::string_view, content_type&&, std::string_view,
        std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make(std::string_view, std::string_view, std::string_view, body_type&&,
        std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_part make(std::string_view, std::string_view, content_type&&, std::string_view,
        bool, std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make(std::string_view, std::string_view, std::string_view, body_type&&, bool,
        std::pmr::memory_resource*) = delete;

    [[nodiscard]] static multipart_part make_decoded(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, std::pmr::memory_resource* resource) {
        return make_decoded(name, filename, content_type_value, body, !filename.empty(), resource);
    }

    [[nodiscard]] static multipart_part make_decoded(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, bool filename_present,
        std::pmr::memory_resource* resource) {
        return multipart_part(std::pmr::string(name, resource), std::pmr::string(filename, resource),
            content_type_value, body, filename_present);
    }

    template <http_temporary_owning_char_string content_type>
    static multipart_part make_decoded(std::string_view, std::string_view, content_type&&,
        std::string_view, std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(std::string_view, std::string_view, std::string_view, body_type&&,
        std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_part make_decoded(std::string_view, std::string_view, content_type&&,
        std::string_view, bool, std::pmr::memory_resource*) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(std::string_view, std::string_view, std::string_view, body_type&&,
        bool, std::pmr::memory_resource*) = delete;
};

}  // namespace ruvia::detail
