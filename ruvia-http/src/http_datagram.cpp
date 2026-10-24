#include "ruvia/http/http_datagram.h"

#include <algorithm>
#include <stdexcept>

#include "field/http_structured_fields.h"

namespace ruvia {
namespace {
constexpr std::uint64_t capsule_var_int_max = (std::uint64_t{1} << 62) - 1;
struct capsule_var_int final {
    std::uint64_t value_;
    std::size_t encoded_bytes_;
};
constexpr std::size_t capsule_var_int_size(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6) ? 1 : value < (std::uint64_t{1} << 14) ? 2
                                             : value < (std::uint64_t{1} << 30)   ? 4
                                                                                  : 8;
}
std::variant<capsule_var_int, http_capsule_codec_error> decode_capsule_var_int(std::span<const char> input) noexcept {
    if (input.empty()) {
        return http_capsule_codec_error::need_more_data;
    }
    const auto first = static_cast<unsigned char>(input.front());
    const std::size_t size = std::size_t{1} << (first >> 6);
    if (input.size() < size) {
        return http_capsule_codec_error::need_more_data;
    }
    std::uint64_t value = first & 0x3f;
    for (std::size_t i = 1; i < size; ++i) {
        value = (value << 8) | static_cast<unsigned char>(input[i]);
    }
    return capsule_var_int{value, size};
}
std::variant<std::size_t, http_capsule_codec_error> encode_capsule_var_int(std::span<char> output, std::uint64_t value) noexcept {
    if (value > capsule_var_int_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto size = capsule_var_int_size(value);
    if (output.size() < size) {
        return http_capsule_codec_error::output_too_small;
    }
    for (std::size_t i = size; i != 0; --i) {
        output[i - 1] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
    const unsigned tag = size == 1 ? 0 : size == 2 ? 0x40
                                     : size == 4   ? 0x80
                                                   : 0xc0;
    output[0] = static_cast<char>(static_cast<unsigned char>(output[0]) | tag);
    return size;
}
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> plan_datagram_capsule(
    std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    http_datagram_write_plan plan{.payload_ = payload_value};
    const auto header = encode_http_capsule_header(
        plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
}  // namespace
std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::nullopt;
    }
    // RFC 8941 4.2: only SP surrounds a top-level Item; OWS (with HTAB)
    // belongs between List and Dictionary members.
    parser.spaces();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return std::nullopt;
    }
    return item.boolean_;
}

std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = decode_capsule_var_int(input);
    if (id.index() != 0) {
        return std::nullopt;
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return encode_capsule_var_int(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > capsule_var_int_max || length > capsule_var_int_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = capsule_var_int_size(type);
    const auto length_size = capsule_var_int_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)encode_capsule_var_int(output, type);
    (void)encode_capsule_var_int(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config)
    : config_(config) {
    if (config.max_capsule_length_ > capsule_var_int_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
http_capsule_status http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value) {
    return feed_impl(input, fin, callback_value, context_value, false).status_;
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value) {
    return feed_impl(input, fin, callback_value, context_value, true);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != http_capsule_status::need_more_data) {
        return {.status_ = status_};
    }
    struct guard {
        bool& value_;
        guard(bool& v)
            : value_(v) {
            value_ = true;
        }
        ~guard() {
            value_ = false;
        }
    } guard(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    auto type = decode_capsule_var_int(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = decode_capsule_var_int(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if ((length.index() != 0)) {
                    continue;
                }
                if (std::get<0>(length).value_ > config_.max_capsule_length_) {
                    status_ = http_capsule_status::limit;
                    return {.status_ = status_, .consumed_bytes_ = initial_size - input.size()};
                }
                remaining_ = std::get<0>(length).value_;
                payload_ = remaining_ != 0;
                header_size_ = 0;
                type_size_ = 0;
                if (!payload_) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback_value) {
                    callback_value(context_value, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || header_size_ ? http_capsule_status::truncated : http_capsule_status::end;
        }
        return {.status_ = status_, .consumed_bytes_ = initial_size - input.size(), .capsule_complete_ = completed};
    } catch (...) {
        status_ = http_capsule_status::truncated;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    return plan_datagram_capsule(payload_value, {});
}
std::optional<std::span<const char>> http_datagram_session::receive_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return *udp;
}
}  // namespace ruvia
