#include "ruvia/web/dotenv.h"

#include <algorithm>
#include <fstream>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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
    : state_(construct_pmr_object<detail::env_state>(process_resource())) {}

env::~env() = default;

void env::state_deleter_type::operator()(detail::env_state* state_value) const noexcept {
    destroy_pmr_object(state_value, process_resource());
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

std::filesystem::path detail::dotenv_executable_directory() {
#ifdef _WIN32
    std::pmr::wstring buffer(260, L'\0', process_resource());
    for (;;) {
        const auto length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::pmr::vector<char> buffer(1024, process_resource());
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error("failed to resolve executable path");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(
                std::string_view(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::pmr::string detail::dotenv_path_message(std::string_view prefix, const std::filesystem::path& path) {
    const auto name = path.string<char8_t, std::char_traits<char8_t>, std::pmr::polymorphic_allocator<char8_t>>(
        std::pmr::polymorphic_allocator<char8_t>(process_resource()));
    std::pmr::string result_value(prefix, process_resource());
    result_value.append(reinterpret_cast<const char*>(name.data()), name.size());
    return result_value;
}

env& env::load_dotenv(dotenv_options options) & {
    return load_dotenv(detail::dotenv_executable_directory() / ".env", options);
}

env& env::load_dotenv(const std::filesystem::path& path, dotenv_options options) & {
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
            throw std::runtime_error(detail::dotenv_path_message("failed to read dotenv file: ", path).c_str());
        }
        if (options.missing_file_ == dotenv_missing_file_policy::require) {
            throw std::runtime_error(detail::dotenv_path_message("dotenv file not found: ", path).c_str());
        }
        return *this;
    }

    auto entries = detail::read_dotenv_entries(input, path);
    auto& variables = state_->variables_;
    if (!entries.empty()) {
        variables.reserve(variables.size() + entries.size());
    }

    for (auto& entry : entries) {
        auto it = find_variable_slot(variables, entry.name_);
        if (it != variables.end() && std::string_view(it->name_) == std::string_view(entry.name_)) {
            if (options.existing_variables_ == dotenv_existing_variable_policy::override) {
                it->value_ = std::move(entry.value_);
            }
            continue;
        }

        variables.insert(it, std::move(entry));
    }

    state_->loaded_ = true;
    return *this;
}

}  // namespace ruvia
