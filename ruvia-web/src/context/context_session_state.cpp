#include "context/context_session_state.h"

#include <stdexcept>

namespace ruvia::detail {

void context_session_state::bind(const session_middleware* owner_value) {
    if (available_) {
        throw std::logic_error("session middleware is already bound");
    }
    require_mutable();
    owner_ = owner_value;
    available_ = true;
}

void context_session_state::require_mutable() const {
    if (phase_ != phase_type::active) {
        throw std::logic_error("session cannot change after response commit begins");
    }
}

bool context_session_state::begin_commit() {
    if (!available_ || phase_ == phase_type::committed) {
        return false;
    }
    if (phase_ == phase_type::failed) {
        std::rethrow_exception(failure_);
    }
    require_mutable();
    phase_ = phase_type::committing;
    return true;
}

void context_session_state::finish_commit() noexcept {
    phase_ = phase_type::committed;
}

void context_session_state::fail_commit(std::exception_ptr exception) noexcept {
    failure_ = std::move(exception);
    phase_ = phase_type::failed;
}

std::pmr::string context_session_state::copy(std::string_view value) const {
    return std::pmr::string(value, resource_);
}

void context_session_state::observe_presented_id(std::string_view id) {
    require_mutable();
    value_.template emplace<session_unrecognized>(copy(id));
}

void context_session_state::load_recognized(std::string_view data) {
    require_mutable();
    auto* presented = std::get_if<session_unrecognized>(&value_);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    auto data_copy = copy(data);
    auto id = std::move(presented->id_);
    value_.template emplace<session_loaded>(std::move(id), std::move(data_copy));
}

void context_session_state::set(std::string_view data) {
    require_mutable();
    if (data.empty()) {
        clear();
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        auto data_copy = copy(data);
        auto id = std::move(loaded_state->id_);
        value_.template emplace<session_rotate>(std::move(id), std::move(data_copy));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        rotated->data_.assign(data);
        return;
    }
    if (auto* fresh = std::get_if<session_persist_new>(&value_)) {
        fresh->data_.assign(data);
        return;
    }
    if (auto* cleared = std::get_if<session_clear>(&value_)) {
        // clear() then set() is the "drop the old session, start a fresh one"
        // idiom. The presented id was already captured for deletion, so this is a
        // rotation -- falling through to session_persist_new would mint a new id and
        // silently orphan the old server-side blob.
        if (!cleared->old_id_.has_value()) {
            value_.template emplace<session_persist_new>(copy(data));
            return;
        }
        auto data_copy = copy(data);
        auto old_id = std::move(*cleared->old_id_);
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data_copy));
        return;
    }
    value_.template emplace<session_persist_new>(copy(data));
}

void context_session_state::clear() {
    require_mutable();
    // Already clearing: keep the id captured by the first call. Re-emplacing would
    // drop it, and the middleware only deletes the server-side blob when it is
    // present -- a second clear() would silently downgrade logout to cookie-only.
    if (std::get_if<session_clear>(&value_) != nullptr) {
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        std::optional<std::pmr::string> id(std::move(loaded_state->id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        std::optional<std::pmr::string> id(std::move(rotated->old_id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    value_.template emplace<session_clear>(std::nullopt);
}

void context_session_state::regenerate() {
    require_mutable();
    const auto regenerate_existing = [this](auto& state_value) {
        auto old_id = std::move(state_value.id_);
        auto data = std::move(state_value.data_);
        if (data.empty()) {
            value_.template emplace<session_clear>(
                std::optional<std::pmr::string>(std::move(old_id)));
        } else {
            value_.template emplace<session_rotate>(std::move(old_id), std::move(data));
        }
    };
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        regenerate_existing(*loaded_state);
        return;
    }
    if (std::holds_alternative<session_clear>(value_) ||
        std::holds_alternative<session_rotate>(value_) ||
        std::holds_alternative<session_persist_new>(value_)) {
        return;
    }
    const auto current_data = data();
    if (!current_data.empty()) {
        value_.template emplace<session_persist_new>(copy(current_data));
    }
}

std::string_view context_session_state::data() const& noexcept {
    if (const auto* state = std::get_if<session_loaded>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_persist_new>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_rotate>(&value_)) {
        return state->data_;
    }
    return {};
}

}  // namespace ruvia::detail
