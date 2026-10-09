#pragma once

#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia {
class session_middleware;
}

namespace ruvia::detail {

struct session_untouched final {};

struct session_unrecognized final {
    std::pmr::string id_;
};

struct session_loaded final {
    std::pmr::string id_;
    std::pmr::string data_;
};

struct session_persist_new final {
    std::pmr::string data_;
};

struct session_rotate final {
    std::pmr::string old_id_;
    std::pmr::string data_;
};

struct session_clear final {
    std::optional<std::pmr::string> old_id_;
};

class context_session_state final {
public:
    explicit context_session_state(std::pmr::memory_resource* resource) noexcept
        : resource_(resource) {}

    void bind(const session_middleware* owner = nullptr);
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] const session_middleware* owner() const noexcept {
        return owner_;
    }
    // The first commit freezes all mutation before any asynchronous storage I/O.
    // A failed commit is terminal: its external writes cannot safely be replayed.
    [[nodiscard]] bool begin_commit();
    void finish_commit() noexcept;
    void fail_commit(std::exception_ptr exception) noexcept;
    void observe_presented_id(std::string_view id);
    void load_recognized(std::string_view data);
    void set(std::string_view data);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view data() const& noexcept;
    [[nodiscard]] std::string_view data() const&& = delete;

    [[nodiscard]] const session_untouched* untouched() const& noexcept {
        return std::get_if<session_untouched>(&value_);
    }
    [[nodiscard]] const session_untouched* untouched() const&& = delete;

    [[nodiscard]] const session_unrecognized* unrecognized() const& noexcept {
        return std::get_if<session_unrecognized>(&value_);
    }
    [[nodiscard]] const session_unrecognized* unrecognized() const&& = delete;

    [[nodiscard]] const session_loaded* loaded() const& noexcept {
        return std::get_if<session_loaded>(&value_);
    }
    [[nodiscard]] const session_loaded* loaded() const&& = delete;

    [[nodiscard]] const session_persist_new* persist_new() const& noexcept {
        return std::get_if<session_persist_new>(&value_);
    }
    [[nodiscard]] const session_persist_new* persist_new() const&& = delete;

    [[nodiscard]] const session_rotate* rotate() const& noexcept {
        return std::get_if<session_rotate>(&value_);
    }
    [[nodiscard]] const session_rotate* rotate() const&& = delete;

    [[nodiscard]] const session_clear* cleared() const& noexcept {
        return std::get_if<session_clear>(&value_);
    }
    [[nodiscard]] const session_clear* cleared() const&& = delete;

private:
    enum class phase_type : unsigned char { active,
        committing,
        committed,
        failed };
    void require_mutable() const;
    [[nodiscard]] std::pmr::string copy(std::string_view value) const;

    std::pmr::memory_resource* resource_;
    bool available_{false};
    const session_middleware* owner_{nullptr};
    phase_type phase_{phase_type::active};
    std::exception_ptr failure_;
    std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new,
        session_rotate, session_clear>
        value_;
};

}  // namespace ruvia::detail
