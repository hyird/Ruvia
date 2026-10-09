#pragma once

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/multipart_parser.h"

namespace ruvia::detail {

struct multipart_stream_part_access final {
    [[nodiscard]] static constexpr multipart_stream_part make(std::string_view name,
        std::string_view filename, std::string_view content_type_value, std::string_view body,
        multipart_chunk_phase phase) noexcept {
        return make(name, filename, content_type_value, body, phase, !filename.empty());
    }

    [[nodiscard]] static constexpr multipart_stream_part make(std::string_view name,
        std::string_view filename, std::string_view content_type_value, std::string_view body,
        multipart_chunk_phase phase, bool filename_present) noexcept {
        return multipart_stream_part(name, filename, content_type_value, body, phase, filename_present);
    }

    template <http_temporary_owning_char_string name_type>
    static multipart_stream_part make(
        name_type&&, std::string_view, std::string_view, std::string_view, multipart_chunk_phase) = delete;

    template <http_temporary_owning_char_string filename_type>
    static multipart_stream_part make(std::string_view, filename_type&&, std::string_view,
        std::string_view, multipart_chunk_phase) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_stream_part make(std::string_view, std::string_view, content_type&&,
        std::string_view, multipart_chunk_phase) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_stream_part make(
        std::string_view, std::string_view, std::string_view, body_type&&, multipart_chunk_phase) = delete;

    template <http_temporary_owning_char_string name_type>
    static multipart_stream_part make(name_type&&, std::string_view, std::string_view, std::string_view,
        multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string filename_type>
    static multipart_stream_part make(std::string_view, filename_type&&, std::string_view,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string content_type>
    static multipart_stream_part make(std::string_view, std::string_view, content_type&&,
        std::string_view, multipart_chunk_phase, bool) = delete;

    template <http_temporary_owning_char_string body_type>
    static multipart_stream_part make(std::string_view, std::string_view, std::string_view, body_type&&,
        multipart_chunk_phase, bool) = delete;
};

}  // namespace ruvia::detail
