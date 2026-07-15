#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia::detail {

struct SessionUntouched final {};

struct SessionUnrecognized final {
    std::pmr::string id;
};

struct SessionLoaded final {
    std::pmr::string id;
    std::pmr::string data;
};

struct SessionPersistNew final {
    std::pmr::string data;
};

struct SessionPersistExisting final {
    std::pmr::string id;
    std::pmr::string data;
};

struct SessionRotate final {
    std::pmr::string oldId;
    std::pmr::string data;
};

struct SessionClear final {
    std::optional<std::pmr::string> oldId;
};

class ContextSessionState final {
public:
    explicit ContextSessionState(std::pmr::memory_resource* resource) noexcept
        : resource_(resource) {}

    void observePresentedId(std::string_view id);
    void loadRecognized(std::string_view data);
    void set(std::string_view data);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view data() const noexcept;

    [[nodiscard]] const SessionUntouched* untouched() const noexcept {
        return std::get_if<SessionUntouched>(&value_);
    }
    [[nodiscard]] const SessionUnrecognized* unrecognized() const noexcept {
        return std::get_if<SessionUnrecognized>(&value_);
    }
    [[nodiscard]] const SessionLoaded* loaded() const noexcept {
        return std::get_if<SessionLoaded>(&value_);
    }
    [[nodiscard]] const SessionPersistNew* persistNew() const noexcept {
        return std::get_if<SessionPersistNew>(&value_);
    }
    [[nodiscard]] const SessionPersistExisting* persistExisting() const noexcept {
        return std::get_if<SessionPersistExisting>(&value_);
    }
    [[nodiscard]] const SessionRotate* rotate() const noexcept {
        return std::get_if<SessionRotate>(&value_);
    }
    [[nodiscard]] const SessionClear* cleared() const noexcept {
        return std::get_if<SessionClear>(&value_);
    }

private:
    [[nodiscard]] std::pmr::string copy(std::string_view value) const;

    std::pmr::memory_resource* resource_;
    std::variant<
        SessionUntouched,
        SessionUnrecognized,
        SessionLoaded,
        SessionPersistNew,
        SessionPersistExisting,
        SessionRotate,
        SessionClear> value_;
};

}  // namespace ruvia::detail
