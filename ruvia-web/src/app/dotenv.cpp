#include "ruvia/web/dotenv.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"

#include "app/env_state.h"

namespace ruvia {
namespace {

template <typename variables_type>
[[nodiscard]] auto find_variable_slot(variables_type& variables, std::string_view name) noexcept {
    return std::ranges::lower_bound(
        variables, name, std::ranges::less{}, [](const detail::env_variable& variable) noexcept {
            return std::string_view(variable.name_);
        });
}

}  // namespace

env::env()
    : state_(detail::construct_pmr_object<detail::env_state>(detail::app_resource())) {}

env::~env() = default;

void env::state_deleter_type::operator()(detail::env_state* state_value) const noexcept {
    detail::destroy_pmr_object(state_value, detail::app_resource());
}

std::optional<std::string_view> env::get(std::string_view name) const& noexcept {
    const auto& variables = state_->variables_;
    const auto it = find_variable_slot(variables, name);
    if (it == variables.end() || std::string_view(it->name_) != name) {
        return std::nullopt;
    }

    return std::string_view(it->value_);
}

std::optional<bool> env::parse_bool_value(std::string_view value) noexcept {
    if (value == "1") {
        return true;
    }
    if (value == "0") {
        return false;
    }

    // ASCII-only case fold via the shared owner: the boolean tokens are ASCII, and
    // std::tolower is locale-dependent (a non-"C" LC_CTYPE set by the host app could
    // fold bytes unexpectedly).
    if (http_ascii_equals_ignore_case(value, "true") ||
        http_ascii_equals_ignore_case(value, "yes") ||
        http_ascii_equals_ignore_case(value, "on")) {
        return true;
    }
    if (http_ascii_equals_ignore_case(value, "false") ||
        http_ascii_equals_ignore_case(value, "no") ||
        http_ascii_equals_ignore_case(value, "off")) {
        return false;
    }

    return std::nullopt;
}

bool env::loaded() const noexcept {
    return state_->loaded_;
}

std::size_t env::size() const noexcept {
    return state_->variables_.size();
}

dotenv_result detail::load_env_from_executable_directory(env& env_value, dotenv_options options) {
    return detail::load_env_from_file(env_value, detail::dotenv_executable_directory() / ".env", options);
}

dotenv_result detail::load_env_from_file(
    env& env_value, const std::filesystem::path& path, dotenv_options options) {
    if (options.existing_variables_ != dotenv_existing_variable_policy::preserve &&
        options.existing_variables_ != dotenv_existing_variable_policy::override) {
        throw std::invalid_argument("dotenv existing variable policy is invalid");
    }
    if (options.missing_file_ != dotenv_missing_file_policy::ignore &&
        options.missing_file_ != dotenv_missing_file_policy::require) {
        throw std::invalid_argument("dotenv missing file policy is invalid");
    }
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            throw std::runtime_error("failed to read dotenv file: " + path.string());
        }
        if (options.missing_file_ == dotenv_missing_file_policy::require) {
            throw std::runtime_error("dotenv file not found: " + path.string());
        }
        return detail::dotenv_result_access::make(false);
    }

    const auto entries = detail::read_dotenv_entries(input, path);
    auto result_value = detail::dotenv_result_access::make(true);
    auto& state_value = detail::env_access::state(env_value);

    for (const auto& entry : entries) {
        auto& variables = state_value.variables_;
        auto it = find_variable_slot(variables, entry.name_);
        if (it != variables.end() && std::string_view(it->name_) == std::string_view(entry.name_)) {
            if (options.existing_variables_ == dotenv_existing_variable_policy::preserve) {
                detail::dotenv_result_access::increment_variables_skipped(result_value);
                continue;
            }

            it->value_ = entry.value_;
            detail::dotenv_result_access::increment_variables_set(result_value);
            continue;
        }

        detail::env_variable variable;
        variable.name_.assign(entry.name_.data(), entry.name_.size());
        variable.value_.assign(entry.value_.data(), entry.value_.size());
        variables.insert(it, std::move(variable));
        detail::dotenv_result_access::increment_variables_set(result_value);
    }

    state_value.loaded_ = true;
    return result_value;
}

}  // namespace ruvia
