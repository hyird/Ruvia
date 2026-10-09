#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/cookies.h"

namespace ruvia {

class http_response;

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// unchanged until write() completes, so owning-string and cookie_options
// temporaries are rejected at construction. Construction throws std::length_error if the wire
// length cannot be represented by std::size_t.
class set_cookie_plan final {
public:
    set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options);

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    set_cookie_plan(name_type&&, value_type&&, const cookie_options&) = delete;

    set_cookie_plan(std::string_view, std::string_view, cookie_options&&) = delete;
    set_cookie_plan(std::string_view, std::string_view, const cookie_options&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view wire_prefix() const noexcept {
        return prefix_text_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view domain() const noexcept {
        return domain_;
    }

    // output must cover size() bytes and not overlap the borrowed inputs.
    void write(char* output) const;

private:
    friend class http_response;
    struct written_fields final {
        std::string_view wire_name_;
        std::string_view path_;
        std::string_view domain_;
    };
    [[nodiscard]] written_fields write_fields(char* output) const noexcept;
    std::array<char, 32> expires_buffer_{};
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::string_view prefix_text_;
    std::string_view priority_text_;
    std::string_view same_site_text_;
    std::uint64_t max_age_value_{0};
    std::size_t expires_size_{0};
    std::size_t max_age_size_{0};
    std::size_t size_{0};
    bool has_max_age_{false};
    bool http_only_{false};
    bool secure_{false};
    bool partitioned_{false};
};

}  // namespace ruvia
