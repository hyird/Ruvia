#pragma once

#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/http/http_content_codec.h"

namespace ruvia::detail {

struct http_content_decode_result_access final {
    [[nodiscard]] static http_content_decode_result decoded(std::pmr::string bytes_value) noexcept {
        return http_content_decode_result(http_decoded_content(std::move(bytes_value)));
    }

    [[nodiscard]] static http_content_decode_result failure(http_content_decode_error error) noexcept {
        return http_content_decode_result(http_content_decode_failure(error));
    }
};

struct http_content_encode_result_access final {
    [[nodiscard]] static http_content_encode_result encoded(std::pmr::string bytes_value) noexcept {
        return http_content_encode_result(http_encoded_content(std::move(bytes_value)));
    }

    [[nodiscard]] static http_content_encode_result failure(http_content_encode_error error) noexcept {
        return http_content_encode_result(http_content_encode_failure(error));
    }
};

}  // namespace ruvia::detail
