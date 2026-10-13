#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/multipart_parser.h"

namespace ruvia::detail {

struct multipart_part_access final {
    // name/filename are already decoded and are copied into part-owned storage.
    // The body stays borrowed; the content type is owned only when
    // `own_content_type` reports that unfolding produced it.
    [[nodiscard]] static multipart_part make_decoded(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, bool filename_present,
        std::pmr::memory_resource* resource, bool own_content_type) {
        multipart_part::content_type_storage_type content_type(content_type_value);
        if (own_content_type) {
            content_type.emplace<std::pmr::string>(content_type_value, resource);
        }
        return multipart_part(std::pmr::string(name, resource), std::pmr::string(filename, resource),
            std::move(content_type), body, filename_present);
    }

    template <http_temporary_owning_char_string content_type>
    static multipart_part make_decoded(std::string_view, std::string_view, content_type&&,
        std::string_view, bool, std::pmr::memory_resource*, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(std::string_view, std::string_view, std::string_view, body_type&&,
        bool, std::pmr::memory_resource*, bool) = delete;
};

}  // namespace ruvia::detail
