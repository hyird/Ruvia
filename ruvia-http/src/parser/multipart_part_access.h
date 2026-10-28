#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/multipart_parser.h"

namespace ruvia::detail {

struct multipart_part_access final {
    using text_storage_type = multipart_part::text_storage_type;

    // Unmodified fields borrow the caller-owned body; unfolded or escaped fields
    // own PMR storage.
    [[nodiscard]] static multipart_part make_decoded(multipart_part::text_storage_type name,
        multipart_part::text_storage_type filename, std::string_view content_type_value,
        std::string_view body, bool filename_present, std::pmr::memory_resource* resource,
        bool own_content_type) {
        multipart_part::content_type_storage_type content_type(content_type_value);
        if (own_content_type) {
            content_type.emplace<std::pmr::string>(content_type_value, resource);
        }
        return multipart_part(
            std::move(name), std::move(filename), std::move(content_type), body, filename_present);
    }

    template <http_temporary_owning_char_string content_type>
    static multipart_part make_decoded(std::string_view, std::string_view, content_type&&,
        std::string_view, bool, std::pmr::memory_resource*, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_part make_decoded(std::string_view, std::string_view, std::string_view, body_type&&,
        bool, std::pmr::memory_resource*, bool) = delete;
};

}  // namespace ruvia::detail
