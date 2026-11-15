#include "http_capsule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>

#include "http_structured_field.h"
#include "quic_varint.h"

namespace ruvia {

namespace {
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
// Largest UDP payload: 65535 minus the 8-byte UDP header.
constexpr std::size_t max_udp_payload_size = 65527;
}  // namespace
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::plan_datagram_capsule(std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    if (payload_value.size() > detail::quic_varint_max - context_prefix.size()) {
        return http_datagram_error::payload_too_large;
    }
    http_datagram_write_plan plan;
    plan.payload_ = payload_value;
    const auto header = encode_http_capsule_header(plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
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
    const auto id = detail::decode_quic_varint(input);
    if (!id) {
        return std::nullopt;
    }
    return http_udp_datagram_view{id->value_, input.subspan(id->encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return detail::encode_quic_varint(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > detail::quic_varint_max || length > detail::quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = detail::quic_varint_size(type);
    const auto length_size = detail::quic_varint_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)detail::encode_quic_varint(output, type);
    (void)detail::encode_quic_varint(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_options config) :
    config_(config) {
    if (config.max_capsule_length_ > detail::quic_varint_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
void http_capsule_decoder::reset() noexcept {
    header_size_ = 0;
    type_size_ = 0;
    context_id_size_ = 0;
    type_ = 0;
    remaining_ = 0;
    mode_ = payload_mode::deliver;
    payload_ = false;
    state_ = decoder_state::active;
}
http_capsule_feed_result http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, false, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, true, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one, http_capsule_retention retention) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (state_ == decoder_state::failed) {
        return http_capsule_feed_result(0, http_capsule_decode_failure(error_));
    }
    if (state_ == decoder_state::finished) {
        return http_capsule_feed_result(0, decode_complete{});
    }
    struct feeding_guard final {
        bool& value_;
        explicit feeding_guard(bool& value) noexcept :
            value_(value) {
            value_ = true;
        }
        ~feeding_guard() {
            value_ = false;
        }
    } feeding_scope(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    const auto fail = [&](http_capsule_decode_error error) noexcept {
        error_ = error;
        state_ = decoder_state::failed;
        return http_capsule_feed_result(initial_size - input.size(), http_capsule_decode_failure(error));
    };
    const auto limit_reached = [&]() noexcept {
        return fail(http_capsule_decode_error::limit_exceeded);
    };
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    const auto type = detail::decode_quic_varint(std::span<const char>(header_).first(header_size_));
                    if (!type) {
                        continue;
                    }
                    type_ = type->value_;
                    type_size_ = type->encoded_bytes_;
                }
                const auto length = detail::decode_quic_varint(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if (!length) {
                    continue;
                }
                remaining_ = length->value_;
                header_size_ = 0;
                type_size_ = 0;
                const bool kept_type = retention == http_capsule_retention::all || type_ == http_datagram_capsule_type;
                if (!kept_type) {
                    // RFC 9297 section 3.2: drop and skip over it, at any length.
                    mode_ = payload_mode::skip;
                } else if (retention == http_capsule_retention::udp_payloads && remaining_ != 0) {
                    // RFC 9298 section 4: an unknown Context ID drops the
                    // datagram, so the limit waits for the Context ID.
                    mode_ = payload_mode::context_id;
                    context_id_size_ = 0;
                } else if (remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                } else {
                    mode_ = payload_mode::deliver;
                }
                payload_ = remaining_ != 0;
                if (!payload_ && kept_type) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else if (mode_ == payload_mode::skip) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
            } else if (mode_ == payload_mode::context_id) {
                context_id_[context_id_size_++] = input.front();
                input = input.subspan(1);
                --remaining_;
                const auto context_id = detail::decode_quic_varint(std::span<const char>(context_id_).first(context_id_size_));
                if (!context_id && remaining_ != 0) {
                    continue;
                }
                payload_ = remaining_ != 0;
                if (context_id && context_id->value_ != 0) {
                    mode_ = payload_mode::skip;
                    continue;
                }
                // Context ID 0, or a value too short to hold its Context ID.
                if (context_id_size_ + remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                }
                mode_ = payload_mode::deliver;
                if (callback_value) {
                    callback_value(context_value, {type_, std::span<const char>(context_id_).first(context_id_size_), !payload_});
                }
                if (!payload_) {
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
        const auto consumed = initial_size - input.size();
        if (fin && input.empty()) {
            if (payload_ || header_size_ != 0) {
                return fail(http_capsule_decode_error::truncated);
            }
            state_ = decoder_state::finished;
        }
        if (one && completed) {
            return http_capsule_feed_result(consumed, http_capsule_boundary{});
        }
        if (state_ == decoder_state::finished) {
            return http_capsule_feed_result(consumed, decode_complete{});
        }
        return http_capsule_feed_result(consumed, decode_need_input{});
    } catch (...) {
        error_ = http_capsule_decode_error::truncated;
        state_ = decoder_state::failed;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(http_datagram_mode mode, std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (mode == http_datagram_mode::raw) {
        return plan_datagram_capsule(payload_value, {});
    }
    if (payload_value.size() > max_udp_payload_size) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<std::span<const char>>, http_datagram_error> http_datagram_session::receive_datagram(http_datagram_mode mode, std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    if (mode == http_datagram_mode::raw) {
        return std::optional<std::span<const char>>(input);
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > max_udp_payload_size) {
        return http_datagram_error::payload_too_large;
    }
    return std::optional<std::span<const char>>(udp->payload_);
}

}  // namespace ruvia
