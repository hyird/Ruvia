#pragma once

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {

// One allocation-free deadline lifecycle. A deadline is either absent, armed
// with its cancellation kind and exact time point, or expired with that same
// kind retained until the awaiting operation consumes the outcome.
template <typename kind_type>
    requires std::is_enum_v<kind_type>
class operation_deadline final {
public:
    using clock = std::chrono::steady_clock;

    void arm(clock::time_point deadline_value, kind_type kind) noexcept {
        state_.template emplace<active>(deadline_value, std::move(kind));
    }

    void reset() noexcept {
        state_.template emplace<inactive>();
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::holds_alternative<expired_state>(state_);
    }

    [[nodiscard]] const kind_type* kind() const& noexcept {
        if (const auto* armed = std::get_if<active>(&state_)) {
            return &armed->kind_;
        }
        if (const auto* expired_value = std::get_if<expired_state>(&state_)) {
            return &expired_value->kind_;
        }
        return nullptr;
    }
    const kind_type* kind() const&& = delete;

    [[nodiscard]] std::optional<kind_type> expire(clock::time_point now) noexcept {
        const auto* armed = std::get_if<active>(&state_);
        if (armed == nullptr || armed->deadline_ > now) {
            return std::nullopt;
        }
        auto kind = armed->kind_;
        state_.template emplace<expired_state>(kind);
        return kind;
    }

    [[nodiscard]] bool clear() noexcept {
        const bool was_expired = expired();
        reset();
        return was_expired;
    }

private:
    struct inactive final {};

    struct active final {
        active(clock::time_point armed_deadline, kind_type armed_kind) noexcept
            : deadline_(armed_deadline),
              kind_(std::move(armed_kind)) {}

        clock::time_point deadline_;
        kind_type kind_;
    };

    struct expired_state final {
        explicit expired_state(kind_type expired_kind) noexcept
            : kind_(std::move(expired_kind)) {}

        kind_type kind_;
    };

    using state = std::variant<inactive, active, expired_state>;

    state state_;
};

}  // namespace ruvia
