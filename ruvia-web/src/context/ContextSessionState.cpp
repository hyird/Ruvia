#include "ruvia/web/detail/http/context/ContextSessionState.h"

#include <stdexcept>

namespace ruvia::detail {

std::pmr::string ContextSessionState::copy(std::string_view value) const {
    return std::pmr::string(value, resource_);
}

void ContextSessionState::observePresentedId(std::string_view id) {
    value_.template emplace<SessionUnrecognized>(copy(id));
}

void ContextSessionState::loadRecognized(std::string_view data) {
    auto* presented = std::get_if<SessionUnrecognized>(&value_);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    auto dataCopy = copy(data);
    auto id = std::move(presented->id);
    value_.template emplace<SessionLoaded>(std::move(id), std::move(dataCopy));
}

void ContextSessionState::set(std::string_view data) {
    if (data.empty()) {
        clear();
        return;
    }
    if (auto* loadedState = std::get_if<SessionLoaded>(&value_)) {
        auto dataCopy = copy(data);
        auto id = std::move(loadedState->id);
        value_.template emplace<SessionPersistExisting>(std::move(id), std::move(dataCopy));
        return;
    }
    if (auto* existing = std::get_if<SessionPersistExisting>(&value_)) {
        existing->data.assign(data);
        return;
    }
    if (auto* rotated = std::get_if<SessionRotate>(&value_)) {
        rotated->data.assign(data);
        return;
    }
    if (auto* fresh = std::get_if<SessionPersistNew>(&value_)) {
        fresh->data.assign(data);
        return;
    }
    if (auto* cleared = std::get_if<SessionClear>(&value_)) {
        // clear() then set() is the "drop the old session, start a fresh one"
        // idiom. The presented id was already captured for deletion, so this is a
        // rotation -- falling through to SessionPersistNew would mint a new id and
        // silently orphan the old server-side blob.
        if (!cleared->oldId.has_value()) {
            value_.template emplace<SessionPersistNew>(copy(data));
            return;
        }
        auto dataCopy = copy(data);
        auto oldId = std::move(*cleared->oldId);
        value_.template emplace<SessionRotate>(std::move(oldId), std::move(dataCopy));
        return;
    }
    value_.template emplace<SessionPersistNew>(copy(data));
}

void ContextSessionState::clear() {
    // Already clearing: keep the id captured by the first call. Re-emplacing would
    // drop it, and the middleware only deletes the server-side blob when it is
    // present -- a second clear() would silently downgrade logout to cookie-only.
    if (std::get_if<SessionClear>(&value_) != nullptr) {
        return;
    }
    if (auto* loadedState = std::get_if<SessionLoaded>(&value_)) {
        std::optional<std::pmr::string> id(std::move(loadedState->id));
        value_.template emplace<SessionClear>(std::move(id));
        return;
    }
    if (auto* existing = std::get_if<SessionPersistExisting>(&value_)) {
        std::optional<std::pmr::string> id(std::move(existing->id));
        value_.template emplace<SessionClear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<SessionRotate>(&value_)) {
        std::optional<std::pmr::string> id(std::move(rotated->oldId));
        value_.template emplace<SessionClear>(std::move(id));
        return;
    }
    value_.template emplace<SessionClear>(std::nullopt);
}

void ContextSessionState::regenerate() {
    if (auto* loadedState = std::get_if<SessionLoaded>(&value_)) {
        auto oldId = std::move(loadedState->id);
        auto data = std::move(loadedState->data);
        if (data.empty()) {
            value_.template emplace<SessionClear>(
                std::optional<std::pmr::string>(std::move(oldId)));
            return;
        }
        value_.template emplace<SessionRotate>(std::move(oldId), std::move(data));
        return;
    }
    if (auto* existing = std::get_if<SessionPersistExisting>(&value_)) {
        auto oldId = std::move(existing->id);
        auto data = std::move(existing->data);
        if (data.empty()) {
            value_.template emplace<SessionClear>(
                std::optional<std::pmr::string>(std::move(oldId)));
            return;
        }
        value_.template emplace<SessionRotate>(std::move(oldId), std::move(data));
        return;
    }
    if (std::get_if<SessionClear>(&value_) != nullptr ||
        std::get_if<SessionRotate>(&value_) != nullptr) {
        return;
    }
    if (auto* fresh = std::get_if<SessionPersistNew>(&value_)) {
        (void)fresh;
        return;
    }
    const auto currentData = data();
    if (!currentData.empty()) {
        value_.template emplace<SessionPersistNew>(copy(currentData));
    }
}

std::string_view ContextSessionState::data() const& noexcept {
    if (const auto* state = std::get_if<SessionLoaded>(&value_)) {
        return state->data;
    }
    if (const auto* state = std::get_if<SessionPersistNew>(&value_)) {
        return state->data;
    }
    if (const auto* state = std::get_if<SessionPersistExisting>(&value_)) {
        return state->data;
    }
    if (const auto* state = std::get_if<SessionRotate>(&value_)) {
        return state->data;
    }
    return {};
}

}  // namespace ruvia::detail
