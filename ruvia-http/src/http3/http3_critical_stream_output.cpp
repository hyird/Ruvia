#include "ruvia/http/http3_critical_stream_output.h"

#include <variant>

#include "ruvia/http/http3_server_request_admission.h"

namespace ruvia {

http3_critical_stream_output::http3_critical_stream_output(
    http3_local_critical_streams prefixes) noexcept
    : prefixes_(prefixes) {}

std::size_t http3_critical_stream_output::index(stream_kind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

std::span<const char> http3_critical_stream_output::prefix(stream_kind kind) const noexcept {
    switch (kind) {
        case stream_kind::control:
            return prefixes_.control_prefix();
        case stream_kind::qpack_encoder:
            return prefixes_.qpack_encoder_prefix();
        case stream_kind::qpack_decoder:
            return prefixes_.qpack_decoder_prefix();
    }
    return {};
}

std::span<const char> http3_critical_stream_output::next(stream_kind kind) & noexcept {
    const auto slot_index = index(kind);
    if (slot_index >= slots_.size()) {
        return {};
    }
    auto& slot = slots_[slot_index];
    const auto bytes_value = prefix(kind);
    if (slot.consumed_ < bytes_value.size()) {
        slot.offered_ = true;
        slot.offering_goaway_ = false;
        return bytes_value.subspan(slot.consumed_);
    }
    if (kind == stream_kind::control && goaway_queued_ && goaway_consumed_ < goaway_size_) {
        slot.offered_ = true;
        slot.offering_goaway_ = true;
        return std::span<const char>(goaway_).subspan(goaway_consumed_, goaway_size_ - goaway_consumed_);
    }
    return {};
}

bool http3_critical_stream_output::acknowledge(stream_kind kind, std::size_t accepted) noexcept {
    const auto slot_index = index(kind);
    if (slot_index >= slots_.size() || !slots_[slot_index].offered_) {
        return false;
    }
    auto& slot = slots_[slot_index];
    const auto bytes_value = slot.offering_goaway_
                                 ? std::span<const char>(goaway_).first(goaway_size_)
                                 : prefix(kind);
    auto& consumed = slot.offering_goaway_ ? goaway_consumed_ : slot.consumed_;
    if (accepted > bytes_value.size() - consumed) {
        return false;
    }
    consumed += accepted;
    if (consumed == bytes_value.size()) {
        slot.offered_ = false;
        slot.offering_goaway_ = false;
    }
    return true;
}

bool http3_critical_stream_output::queue_goaway(std::uint64_t identifier) noexcept {
    if (goaway_queued_) {
        return false;
    }
    const auto frame_size = encode_http3_server_goaway_frame(goaway_, identifier);
    if ((frame_size.index() != 0)) {
        return false;
    }
    goaway_size_ = std::get<0>(frame_size);
    goaway_queued_ = true;
    return true;
}

bool http3_critical_stream_output::complete(stream_kind kind) const noexcept {
    const auto slot = index(kind);
    if (slot >= slots_.size() || slots_[slot].consumed_ != prefix(kind).size()) {
        return false;
    }
    return kind != stream_kind::control || !goaway_queued_ || goaway_consumed_ == goaway_size_;
}

bool http3_critical_stream_output::complete() const noexcept {
    return complete(stream_kind::control) && complete(stream_kind::qpack_encoder) &&
           complete(stream_kind::qpack_decoder);
}

}  // namespace ruvia
