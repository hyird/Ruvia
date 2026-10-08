#pragma once

#include <charconv>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/web/Dotenv.h"

namespace example {

// Startup-only lookup shared by the examples. Process variables take precedence
// over an optional App::env() loaded from .env beside the executable. A default
// ruvia::Env is empty: it does not import the process environment automatically.
class environment final {
public:
    explicit environment(const ruvia::Env* dotenv = nullptr) noexcept
        : dotenv_(dotenv) {}

    std::optional<std::string_view> get(std::string_view name) const {
        const std::string key(name);
        if (const auto* value = std::getenv(key.c_str())) {
            return std::string_view(value);
        }
        return dotenv_ ? dotenv_->get(name) : std::nullopt;
    }

    template <typename value_type>
        requires std::is_arithmetic_v<value_type>
    std::optional<value_type> get(std::string_view name) const {
        const auto text = get(name);
        if (!text) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<value_type, bool>) {
            if (*text == "true" || *text == "1") {
                return true;
            }
            if (*text == "false" || *text == "0") {
                return false;
            }
        } else {
            value_type value{};
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
            if (error == std::errc{} && end == text->data() + text->size()) {
                return value;
            }
        }
        throw std::invalid_argument("invalid example setting: " + std::string(name));
    }

private:
    const ruvia::Env* dotenv_;
};

}  // namespace example
