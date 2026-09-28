#include "ruvia/web/detail/http3/Http3CriticalStreamOutput.h"

#include "ruvia/http/Http3ServerRequestAdmission.h"

namespace ruvia::detail {

Http3CriticalStreamOutput::Http3CriticalStreamOutput(
    Http3LocalCriticalStreams prefixes) noexcept
    : prefixes_(prefixes) {}

std::size_t Http3CriticalStreamOutput::index(Kind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

std::span<const char> Http3CriticalStreamOutput::prefix(Kind kind) const noexcept {
    switch (kind) {
        case Kind::kControl:
            return prefixes_.controlPrefix();
        case Kind::kQpackEncoder:
            return prefixes_.qpackEncoderPrefix();
        case Kind::kQpackDecoder:
            return prefixes_.qpackDecoderPrefix();
    }
    return {};
}

std::span<const char> Http3CriticalStreamOutput::next(Kind kind) & noexcept {
    const auto slotIndex = index(kind);
    if (slotIndex >= slots_.size()) {
        return {};
    }
    auto& slot = slots_[slotIndex];
    const auto bytes = prefix(kind);
    if (slot.consumed < bytes.size()) {
        slot.offered = true;
        slot.offeringGoaway = false;
        return bytes.subspan(slot.consumed);
    }
    if (kind == Kind::kControl && goawayQueued_ && goawayConsumed_ < goawaySize_) {
        slot.offered = true;
        slot.offeringGoaway = true;
        return std::span<const char>(goaway_).subspan(goawayConsumed_, goawaySize_ - goawayConsumed_);
    }
    return {};
}

bool Http3CriticalStreamOutput::acknowledge(Kind kind, std::size_t accepted) noexcept {
    const auto slotIndex = index(kind);
    if (slotIndex >= slots_.size() || !slots_[slotIndex].offered) {
        return false;
    }
    auto& slot = slots_[slotIndex];
    const auto bytes = slot.offeringGoaway
                           ? std::span<const char>(goaway_).first(goawaySize_)
                           : prefix(kind);
    auto& consumed = slot.offeringGoaway ? goawayConsumed_ : slot.consumed;
    if (accepted > bytes.size() - consumed) {
        return false;
    }
    consumed += accepted;
    if (consumed == bytes.size()) {
        slot.offered = false;
        slot.offeringGoaway = false;
    }
    return true;
}

bool Http3CriticalStreamOutput::queueGoaway(std::uint64_t identifier) noexcept {
    if (goawayQueued_) {
        return false;
    }
    const auto frameSize = encodeHttp3ServerGoawayFrame(goaway_, identifier);
    if (!frameSize) {
        return false;
    }
    goawaySize_ = *frameSize;
    goawayQueued_ = true;
    return true;
}

bool Http3CriticalStreamOutput::complete(Kind kind) const noexcept {
    const auto slot = index(kind);
    if (slot >= slots_.size() || slots_[slot].consumed != prefix(kind).size()) {
        return false;
    }
    return kind != Kind::kControl || !goawayQueued_ || goawayConsumed_ == goawaySize_;
}

bool Http3CriticalStreamOutput::complete() const noexcept {
    return complete(Kind::kControl) && complete(Kind::kQpackEncoder) &&
           complete(Kind::kQpackDecoder);
}

}  // namespace ruvia::detail
