#include "ruvia/http/http3_critical_stream_output.h"

#include "ruvia/http/Http3ServerRequestAdmission.h"

namespace ruvia {

http3_critical_stream_output::http3_critical_stream_output(
    Http3LocalCriticalStreams prefixes) noexcept
    : prefixes_(prefixes) {}

std::size_t http3_critical_stream_output::index(stream_kind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

std::span<const char> http3_critical_stream_output::prefix(stream_kind kind) const noexcept {
    switch (kind) {
        case stream_kind::control:
            return prefixes_.controlPrefix();
        case stream_kind::qpack_encoder:
            return prefixes_.qpackEncoderPrefix();
        case stream_kind::qpack_decoder:
            return prefixes_.qpackDecoderPrefix();
    }
    return {};
}

std::span<const char> http3_critical_stream_output::next(stream_kind kind) & noexcept {
    const auto slot_index = index(kind);
    if (slot_index >= slots_.size()) {
        return {};
    }
    auto& slot = slots_[slot_index];
    const auto bytes = prefix(kind);
    if (slot.consumed < bytes.size()) {
        slot.offered = true;
        slot.offering_goaway = false;
        return bytes.subspan(slot.consumed);
    }
    if (kind == stream_kind::control && goaway_queued_ && goaway_consumed_ < goaway_size_) {
        slot.offered = true;
        slot.offering_goaway = true;
        return std::span<const char>(goaway_).subspan(goaway_consumed_, goaway_size_ - goaway_consumed_);
    }
    return {};
}

bool http3_critical_stream_output::acknowledge(stream_kind kind, std::size_t accepted) noexcept {
    const auto slot_index = index(kind);
    if (slot_index >= slots_.size() || !slots_[slot_index].offered) {
        return false;
    }
    auto& slot = slots_[slot_index];
    const auto bytes = slot.offering_goaway
                           ? std::span<const char>(goaway_).first(goaway_size_)
                           : prefix(kind);
    auto& consumed = slot.offering_goaway ? goaway_consumed_ : slot.consumed;
    if (accepted > bytes.size() - consumed) {
        return false;
    }
    consumed += accepted;
    if (consumed == bytes.size()) {
        slot.offered = false;
        slot.offering_goaway = false;
    }
    return true;
}

bool http3_critical_stream_output::queue_goaway(std::uint64_t identifier) noexcept {
    if (goaway_queued_) {
        return false;
    }
    const auto frame_size = encodeHttp3ServerGoawayFrame(goaway_, identifier);
    if (!frame_size) {
        return false;
    }
    goaway_size_ = *frame_size;
    goaway_queued_ = true;
    return true;
}

bool http3_critical_stream_output::complete(stream_kind kind) const noexcept {
    const auto slot = index(kind);
    if (slot >= slots_.size() || slots_[slot].consumed != prefix(kind).size()) {
        return false;
    }
    return kind != stream_kind::control || !goaway_queued_ || goaway_consumed_ == goaway_size_;
}

bool http3_critical_stream_output::complete() const noexcept {
    return complete(stream_kind::control) && complete(stream_kind::qpack_encoder) &&
           complete(stream_kind::qpack_decoder);
}

}  // namespace ruvia
