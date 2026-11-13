#pragma once

#include <exception>
#include <utility>

// A reusable codec slot: native compression libraries keep pointers into
// their stream, so the codec lives in one stable heap allocation.
// `codec_type` provides initialized() and reset().

namespace ruvia::detail {

// The slot's codec, created on first use. A codec that failed to initialize is
// not kept, so the next use retries; nullptr reports that failure.
template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_codec(codec_type*& slot, args_type&&... args) {
    if (slot == nullptr) {
        auto* created = new codec_type(std::forward<args_type>(args)...);
        if (!created->initialized()) {
            delete created;
            return nullptr;
        }
        slot = created;
    }
    return slot;
}

// Returns an acquired slot's codec to its initial state when the scope ends
// normally. A codec left by an exception, or one that cannot reset, is
// dropped and recreated by the next use: failed native state is never reused.
template <typename codec_type>
class codec_reset_guard final {
public:
    explicit codec_reset_guard(codec_type*& slot) noexcept
        : slot_(slot),
          uncaught_exceptions_(std::uncaught_exceptions()) {}
    codec_reset_guard(const codec_reset_guard&) = delete;
    codec_reset_guard& operator=(const codec_reset_guard&) = delete;

    ~codec_reset_guard() {
        if (std::uncaught_exceptions() != uncaught_exceptions_ || !slot_->reset()) {
            delete std::exchange(slot_, nullptr);
        }
    }

private:
    codec_type*& slot_;
    int uncaught_exceptions_;
};

}  // namespace ruvia::detail
