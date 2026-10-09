#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {

inline constexpr std::size_t http3_frame_header_max_bytes = 2 * http3_var_int_max_bytes;

enum class http3_frame_type : std::uint64_t {
    data = 0x0,
    headers = 0x1,
    cancel_push = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    goaway = 0x7,
    max_push_id = 0xd
};

struct http3_frame_header final {
    std::uint64_t type_{0};
    std::uint64_t length_{0};
    std::size_t encoded_bytes_{0};
};

struct http3_frame_view final {
    std::uint64_t type_{0};
    std::span<const char> payload_{};
    std::size_t encoded_bytes_{0};
};

[[nodiscard]] inline constexpr bool is_http3_grease_frame_type(std::uint64_t type) noexcept {
    return type >= 0x21 && (type - 0x21) % 0x1f == 0;
}

[[nodiscard]] inline constexpr std::variant<http3_frame_header, http3_codec_error> decode_http3_frame_header(
    std::span<const char> input) noexcept {
    const auto type = decode_http3_var_int(input);
    if ((type.index() != 0)) {
        return std::get<1>(type);
    }
    const auto length = decode_http3_var_int(input.subspan(std::get<0>(type).encoded_bytes_));
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    return http3_frame_header{.type_ = std::get<0>(type).value_,
        .length_ = std::get<0>(length).value_,
        .encoded_bytes_ = std::get<0>(type).encoded_bytes_ + std::get<0>(length).encoded_bytes_};
}

[[nodiscard]] inline constexpr std::variant<std::size_t, http3_codec_error> encode_http3_frame_header(
    std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > http3_var_int_max || length > http3_var_int_max) {
        return http3_codec_error::value_out_of_range;
    }
    const auto required = http3_var_int_encoded_size(type) + http3_var_int_encoded_size(length);
    if (output.size() < required) {
        return http3_codec_error::output_too_small;
    }
    const auto type_size = encode_http3_var_int(output, type);
    const auto length_size = encode_http3_var_int(output.subspan(std::get<0>(type_size)), length);
    return std::get<0>(type_size) + std::get<0>(length_size);
}

[[nodiscard]] inline constexpr std::variant<http3_frame_view, http3_codec_error> decode_http3_frame(
    std::span<const char> input) noexcept {
    const auto header_value = decode_http3_frame_header(input);
    if ((header_value.index() != 0)) {
        return std::get<1>(header_value);
    }
    if (std::get<0>(header_value).length_ > input.size() - std::get<0>(header_value).encoded_bytes_) {
        return http3_codec_error::need_more_data;
    }
    const auto payload_length = static_cast<std::size_t>(std::get<0>(header_value).length_);
    return http3_frame_view{.type_ = std::get<0>(header_value).type_,
        .payload_ = input.subspan(std::get<0>(header_value).encoded_bytes_, payload_length),
        .encoded_bytes_ = std::get<0>(header_value).encoded_bytes_ + payload_length};
}

[[nodiscard]] inline constexpr std::variant<std::size_t, http3_codec_error> encode_http3_frame(
    std::span<char> output, std::uint64_t type, std::span<const char> payload_value) noexcept {
    if (type > http3_var_int_max) {
        return http3_codec_error::value_out_of_range;
    }
    if (payload_value.size() > http3_var_int_max) {
        return http3_codec_error::value_out_of_range;
    }
    const auto header_size = http3_var_int_encoded_size(type) + http3_var_int_encoded_size(payload_value.size());
    if (output.size() < header_size || output.size() - header_size < payload_value.size()) {
        return http3_codec_error::output_too_small;
    }
    const auto written = encode_http3_frame_header(output, type, payload_value.size());
    if ((written.index() != 0)) {
        return std::get<1>(written);
    }
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        output[std::get<0>(written) + i] = payload_value[i];
    }
    return std::get<0>(written) + payload_value.size();
}

}  // namespace ruvia
