#include "ruvia/http/http3_message_body.h"

#include <limits>

namespace ruvia {

http3_message_body::http3_message_body(std::optional<std::uint64_t> content_length, bool payload_allowed) noexcept
    : content_length_(content_length),
      payload_allowed_(payload_allowed) {}

http3_message_body_result http3_message_body::feed(std::uint64_t data_length, bool fin) noexcept {
    if (state_ == state_type::complete) {
        return http3_message_body_result::already_complete;
    }
    if (state_ == state_type::failed) {
        return http3_message_body_result::already_failed;
    }

    const auto fail = [this](http3_message_body_result result_value) {
        state_ = state_type::failed;
        return result_value;
    };

    if (!payload_allowed_ && data_length != 0) {
        return fail(http3_message_body_result::payload_not_allowed);
    }
    if (data_length > std::numeric_limits<std::uint64_t>::max() - received_length_) {
        return fail(http3_message_body_result::length_overflow);
    }

    const auto total_length = received_length_ + data_length;
    if (content_length_ && total_length > *content_length_) {
        return fail(http3_message_body_result::content_length_exceeded);
    }

    received_length_ = total_length;
    if (!fin) {
        return http3_message_body_result::accepted;
    }
    if (payload_allowed_ && content_length_ && received_length_ != *content_length_) {
        return fail(http3_message_body_result::content_length_mismatch);
    }

    state_ = state_type::complete;
    return http3_message_body_result::complete;
}

http3_message_body::state_type http3_message_body::state() const noexcept {
    return state_;
}

std::uint64_t http3_message_body::received_length() const noexcept {
    return received_length_;
}

}  // namespace ruvia
