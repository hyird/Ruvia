#include "ruvia/http/http1_chunked_body_decoder.h"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {

http1_chunk_decode_result http1_chunk_decode_result::make_need_more(std::size_t consumed_bytes) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_need_more(consumed_bytes));
}

http1_chunk_decode_result http1_chunk_decode_result::make_body_chunk(
    std::size_t consumed_bytes, std::string_view bytes_value) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_body_chunk_view(consumed_bytes, bytes_value));
}

http1_chunk_decode_result http1_chunk_decode_result::make_complete(
    std::size_t consumed_bytes, std::string_view trailers) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_complete_view(consumed_bytes, trailers));
}

http1_chunk_decode_result http1_chunk_decode_result::make_failure(
    std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_failure(consumed_bytes, error));
}

http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder_config config)
    : framing_({
          .body_limit_ = config.body_limit_,
          .trailer_section_limit_ = protocol_byte_limit::unlimited(),
          .trailer_role_ = config.trailer_role_ == http1_chunk_trailer_role::response
                               ? detail::chunk_trailer_role::response
                               : detail::chunk_trailer_role::request,
      }) {
    if (config.trailer_role_ != http1_chunk_trailer_role::request &&
        config.trailer_role_ != http1_chunk_trailer_role::response) {
        throw std::invalid_argument("invalid HTTP/1 chunk trailer role");
    }
}

http1_chunked_body_decoder::~http1_chunked_body_decoder() = default;
http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept = default;
http1_chunked_body_decoder& http1_chunked_body_decoder::operator=(http1_chunked_body_decoder&&) noexcept = default;

http1_chunk_decode_result http1_chunked_body_decoder::decode(std::string_view available) {
    return decode(available, std::numeric_limits<std::size_t>::max());
}

http1_chunk_decode_result http1_chunked_body_decoder::decode(
    std::string_view available, std::size_t max_body_bytes) {
    if (max_body_bytes == 0) {
        throw std::invalid_argument("HTTP/1 chunk decode body quota must be greater than zero");
    }
    return std::visit([](const auto& result_value) {
        using result_type = std::remove_cvref_t<decltype(result_value)>;
        if constexpr (std::is_same_v<result_type, detail::chunk_framing_need_more>) {
            return http1_chunk_decode_result::make_need_more(result_value.consumed_bytes_);
        } else if constexpr (std::is_same_v<result_type, detail::chunk_framing_body>) {
            return http1_chunk_decode_result::make_body_chunk(result_value.consumed_bytes_, result_value.bytes_);
        } else if constexpr (std::is_same_v<result_type, detail::chunk_framing_complete>) {
            return http1_chunk_decode_result::make_complete(result_value.consumed_bytes_, result_value.trailers_);
        } else {
            auto error = http1_chunk_decode_error::invalid_framing;
            if (result_value.error_ == detail::chunk_framing_error::body_limit_exceeded) {
                error = http1_chunk_decode_error::body_limit_exceeded;
            } else if (result_value.error_ == detail::chunk_framing_error::framing_limit_exceeded) {
                error = http1_chunk_decode_error::framing_limit_exceeded;
            }
            return http1_chunk_decode_result::make_failure(result_value.consumed_bytes_, error);
        }
    },
        framing_.decode(available, max_body_bytes));
}

}  // namespace ruvia
