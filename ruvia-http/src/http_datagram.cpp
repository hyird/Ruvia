#include "ruvia/http/http_datagram.h"

#include <algorithm>
#include <stdexcept>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_peer_streams.h"

#include "field/http_structured_fields.h"

namespace ruvia {
std::variant<bool, http3_codec_error> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return http3_codec_error::value_out_of_range;
    }
    parser.ows();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return http3_codec_error::value_out_of_range;
    }
    return item.boolean_;
}

std::variant<http3_datagram_view, http3_codec_error> decode_http3_datagram(std::span<const char> input) noexcept {
    auto id = decode_http3_var_int(input);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    if (std::get<0>(id).value_ > http3_var_int_max / 4) {
        return http3_codec_error::value_out_of_range;
    }
    return http3_datagram_view{std::get<0>(id).value_ * 4, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http3_codec_error> encode_http3_datagram_prefix(std::span<char> output, std::uint64_t stream_id) noexcept {
    if (!is_http3_request_stream_id(stream_id)) {
        return http3_codec_error::value_out_of_range;
    }
    return encode_http3_var_int(output, stream_id / 4);
}
std::variant<http_udp_datagram_view, http3_codec_error> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = decode_http3_var_int(input);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http3_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return encode_http3_var_int(output, context_id);
}
std::variant<std::size_t, http3_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    return encode_http3_frame_header(output, type, length);
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config)
    : config_(config) {
    if (config.max_capsule_length_ > http3_var_int_max) {
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
                    auto type = decode_http3_var_int(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = decode_http3_var_int(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
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
http3_datagram_receive_status plan_http3_datagram_receive(http3_datagram_view datagram,
    http3_datagram_receive_context context_value) noexcept {
    if (!is_http3_request_stream_id(datagram.stream_id_) || !context_value.local_h3_datagram_) {
        return http3_datagram_receive_status::connection_error;
    }
    if (!context_value.stream_exists_ || !context_value.receive_open_) {
        return http3_datagram_receive_status::drop;
    }
    return context_value.supports_datagrams_ ? http3_datagram_receive_status::deliver : http3_datagram_receive_status::stream_error;
}

http_datagram_session::http_datagram_session(http_datagram_session_config config)
    : config_(config) {
    if (config.http3_stream_id_ && !is_http3_request_stream_id(*config.http3_stream_id_)) {
        throw std::invalid_argument("HTTP datagram stream must be a client bidirectional stream");
    }
}
bool http_datagram_session::quic_datagrams_enabled() const noexcept {
    return config_.http3_stream_id_ && config_.local_h3_datagram_ && config_.peer_h3_datagram_ &&
           config_.quic_datagram_ && config_.max_quic_payload_bytes_ > 0;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(
    std::span<const char> payload_value, http_datagram_transport transport) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    http_datagram_write_plan plan{.payload_ = payload_value, .transport_ = transport};
    if (transport == http_datagram_transport::quic) {
        if (!quic_datagrams_enabled()) {
            return http_datagram_error::not_negotiated;
        }
        const auto prefix = encode_http3_datagram_prefix(plan.prefix_, *config_.http3_stream_id_);
        plan.prefix_size_ = std::get<0>(prefix);
        if (plan.prefix_size_ > config_.max_quic_payload_bytes_ || payload_value.size() > config_.max_quic_payload_bytes_ - plan.prefix_size_) {
            return http_datagram_error::payload_too_large;
        }
    } else {
        const auto prefix = encode_http_capsule_header(plan.prefix_, http_datagram_capsule_type, payload_value.size());
        if ((prefix.index() != 0)) {
            return http_datagram_error::payload_too_large;
        }
        plan.prefix_size_ = std::get<0>(prefix);
    }
    return plan;
}
std::variant<std::optional<std::span<const char>>, http_datagram_error> http_datagram_session::receive_datagram(
    std::span<const char> input, http_datagram_transport transport) const noexcept {
    if (transport == http_datagram_transport::quic) {
        if (!config_.http3_stream_id_ || !config_.local_h3_datagram_) {
            return http_datagram_error::not_negotiated;
        }
        const auto decoded = decode_http3_datagram(input);
        if ((decoded.index() != 0)) {
            return http_datagram_error::malformed;
        }
        if (std::get<0>(decoded).stream_id_ != *config_.http3_stream_id_) {
            return http_datagram_error::wrong_stream;
        }
        input = std::get<0>(decoded).payload_;
    }
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}

std::variant<http_udp_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(
    std::span<const char> payload_value, http_datagram_transport transport) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    http_udp_datagram_write_plan plan{.payload_ = payload_value, .transport_ = transport};
    if (transport == http_datagram_transport::quic) {
        if (!quic_datagrams_enabled()) {
            return http_datagram_error::not_negotiated;
        }
        const auto prefix = encode_http3_datagram_prefix(plan.prefix_, *config_.http3_stream_id_);
        plan.prefix_size_ = std::get<0>(prefix);
        plan.prefix_[plan.prefix_size_++] = 0;  // UDP context ID.
        if (plan.prefix_size_ > config_.max_quic_payload_bytes_ || payload_value.size() > config_.max_quic_payload_bytes_ - plan.prefix_size_) {
            return http_datagram_error::payload_too_large;
        }
    } else {
        const auto prefix = encode_http_capsule_header(plan.prefix_, http_datagram_capsule_type, payload_value.size() + 1);
        plan.prefix_size_ = std::get<0>(prefix);
        plan.prefix_[plan.prefix_size_++] = 0;
    }
    return plan;
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(
    std::span<const char> input, http_datagram_transport transport) const noexcept {
    if (transport == http_datagram_transport::quic) {
        if (!config_.http3_stream_id_ || !config_.local_h3_datagram_) {
            return http_datagram_error::not_negotiated;
        }
        const auto datagram = decode_http3_datagram(input);
        if ((datagram.index() != 0)) {
            return http_datagram_error::malformed;
        }
        if (std::get<0>(datagram).stream_id_ != *config_.http3_stream_id_) {
            return http_datagram_error::wrong_stream;
        }
        input = std::get<0>(datagram).payload_;
    }
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if ((udp.index() != 0)) {
        return http_datagram_error::malformed;
    }
    if (std::get<0>(udp).context_id_ != 0) {
        return std::nullopt;
    }
    if (std::get<0>(udp).payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return std::get<0>(udp);
}
}  // namespace ruvia
