#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/web/detail/json/json_string.h"

namespace ruvia::detail {

inline void append_model_binary(std::pmr::string& output, const bytes& value) {
    const auto encoded_size = http_base64_encoded_size(value.size());
    const auto start = output.size();
    output.resize(start + encoded_size + 2);
    output[start] = '"';
    http_base64_encode(output.data() + start + 1, value.view());
    output[start + encoded_size + 1] = '"';
}

[[nodiscard]] inline std::optional<bytes> decode_model_binary(
    std::string_view encoded, std::pmr::memory_resource* resource) {
    auto* const output_resource = pmr_resource_or_default(resource);
    std::pmr::vector<std::uint8_t> decoded(http_base64_decoded_size_bound(encoded.size()), output_resource);
    const auto decoded_size = http_base64_decode(
        encoded, decoded, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded_size) {
        return std::nullopt;
    }
    decoded.resize(*decoded_size);
    bytes result_value({.resource_ = output_resource});
    result_value.assign_owned(std::move(decoded));
    return result_value;
}

}  // namespace ruvia::detail
