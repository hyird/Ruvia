#include "ruvia/web/dotenv.h"

#include <algorithm>
#include <fstream>
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

#include <istream>
#include <optional>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_utf8.h"

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
    : state_(std::make_unique<detail::env_state>()) {}

env::~env() = default;

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
    std::wstring buffer(260, L'\0');
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
    std::vector<char> buffer(1024);
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

std::string detail::dotenv_path_message(std::string_view prefix, const std::filesystem::path& path) {
    const auto name = path.u8string();
    std::string result_value(prefix);
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

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r';
}

[[nodiscard]] std::string_view trim_left(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim_right(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    return trim_right(trim_left(value));
}

[[nodiscard]] bool is_valid_key(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }

    // Explicit ASCII checks rather than std::isalpha/isalnum: an environment
    // variable name is ASCII by definition, whereas the <cctype> predicates are
    // locale-dependent (a non-"C" LC_CTYPE set by the host app could admit high
    // bytes into a key). This also matches how the rest of the codebase validates
    // identifiers (is_valid_session_id, detail::ensure_config_host, ...).
    const auto is_ascii_alpha = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!(is_ascii_alpha(key.front()) || key.front() == '_')) {
        return false;
    }

    return std::ranges::all_of(key.substr(1), [&is_ascii_alpha](char value) {
        return is_ascii_alpha(value) || (value >= '0' && value <= '9') || value == '_';
    });
}

[[nodiscard]] std::string location_message(
    const std::filesystem::path& path, std::size_t line_number, std::string_view message) {
    auto result_value = dotenv_path_message("invalid dotenv entry in ", path);
    result_value += ':';
    result_value += std::to_string(line_number);
    result_value += ": ";
    result_value += message;
    return result_value;
}

[[nodiscard]] std::size_t dotenv_utf8_sequence_length(std::string_view value, std::size_t index,
    const std::filesystem::path& path, std::size_t line_number) {
    const auto sequence = ::ruvia::scan_utf8_sequence(value, index);
    if (!sequence.well_formed_) {
        throw std::invalid_argument(
            location_message(path, line_number, "value is not valid UTF-8").c_str());
    }
    return sequence.length_;
}

// The complete set of escapes inside double quotes. As in POSIX sh double
// quotes, a backslash before any other character is not an escape: the
// backslash and that character are both kept literally.
[[nodiscard]] std::optional<char> decode_double_quoted_escape(char escaped) noexcept {
    switch (escaped) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case '"':
            return '"';
        case '\\':
            return '\\';
        default:
            return std::nullopt;
    }
}

[[nodiscard]] std::string parse_double_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    std::string result;
    std::size_t literal_start = 1;
    bool has_escape = false;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char current = value[index];
        if (static_cast<unsigned char>(current) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
            continue;
        }
        if (current == '"') {
            consumed = index + 1;
            if (!has_escape) {
                return std::string(value.substr(1, index - 1));
            }
            result.append(value.substr(literal_start, index - literal_start));
            return result;
        }
        if (current == '\\') {
            if (index + 1 == value.size()) {
                throw std::invalid_argument(
                    location_message(path, line_number, "unfinished escape sequence").c_str());
            }
            has_escape = true;
            result.append(value.substr(literal_start, index - literal_start));
            const char escaped = value[++index];
            if (const auto decoded = decode_double_quoted_escape(escaped); decoded.has_value()) {
                result.push_back(*decoded);
            } else {
                result.push_back('\\');
                const auto length = static_cast<unsigned char>(escaped) >= 0x80
                                        ? dotenv_utf8_sequence_length(value, index, path, line_number)
                                        : 1;
                result.append(value.substr(index, length));
                index += length - 1;
            }
            literal_start = index + 1;
        }
    }
    throw std::invalid_argument(
        location_message(path, line_number, "unterminated double-quoted value").c_str());
}

[[nodiscard]] std::string parse_single_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    for (std::size_t index = 1; index < value.size(); ++index) {
        if (value[index] == '\'') {
            consumed = index + 1;
            return std::string(value.substr(1, index - 1));
        }
        if (static_cast<unsigned char>(value[index]) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
        }
    }
    throw std::invalid_argument(
        location_message(path, line_number, "unterminated single-quoted value").c_str());
}

void validate_quoted_remainder(
    std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (!value.empty() && value.front() != '#') {
        throw std::invalid_argument(
            location_message(path, line_number, "unexpected characters after quoted value").c_str());
    }
}

[[nodiscard]] std::string parse_unquoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number) {
    std::size_t end = value.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '#' && (index == 0 || is_space(value[index - 1]))) {
            end = index;
            break;
        }
        if (static_cast<unsigned char>(value[index]) >= 0x80) {
            index += dotenv_utf8_sequence_length(value, index, path, line_number) - 1;
        }
    }

    return std::string(trim(value.substr(0, end)));
}

[[nodiscard]] std::string parse_value(
    std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (value.empty()) {
        return {};
    }

    if (value.front() == '"') {
        std::size_t consumed = 0;
        auto parsed_value = parse_double_quoted_value(value, path, line_number, consumed);
        validate_quoted_remainder(value.substr(consumed), path, line_number);
        return parsed_value;
    }

    if (value.front() == '\'') {
        std::size_t consumed = 0;
        auto parsed_value = parse_single_quoted_value(value, path, line_number, consumed);
        validate_quoted_remainder(value.substr(consumed), path, line_number);
        return parsed_value;
    }

    return parse_unquoted_value(value, path, line_number);
}

void strip_utf8_bom(std::string_view& line) noexcept {
    constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == bom[0] &&
        static_cast<unsigned char>(line[1]) == bom[1] &&
        static_cast<unsigned char>(line[2]) == bom[2]) {
        line.remove_prefix(3);
    }
}

[[nodiscard]] env_variable parse_entry(
    std::string_view line, const std::filesystem::path& path, std::size_t line_number) {
    line = trim(line);
    if (line.starts_with("export") && line.size() > 6 && is_space(line[6])) {
        line = trim(line.substr(6));
    }

    const auto separator = line.find('=');
    if (separator == std::string_view::npos) {
        throw std::invalid_argument(location_message(path, line_number, "missing '='").c_str());
    }

    const auto key = trim(line.substr(0, separator));
    if (!is_valid_key(key)) {
        throw std::invalid_argument(
            location_message(path, line_number, "invalid variable name").c_str());
    }

    auto parsed_value = parse_value(line.substr(separator + 1), path, line_number);
    env_variable entry;
    entry.name_.assign(key.data(), key.size());
    entry.value_ = std::move(parsed_value);
    return entry;
}

}  // namespace

std::vector<env_variable> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path) {
    std::vector<env_variable> entries;
    // Indices into entries ordered by name: duplicate detection stays
    // O(log N) per line and never borrows a name that a reallocation of
    // entries could move (small-string storage relocates with the element).
    std::vector<std::size_t> entries_by_name;
    const auto entry_name = [&entries](std::size_t index) noexcept {
        return std::string_view(entries[index].name_);
    };
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;

        std::string_view view(line);
        if (line_number == 1) {
            strip_utf8_bom(view);
        }

        view = trim(view);
        if (view.empty() || view.front() == '#') {
            continue;
        }

        auto entry = parse_entry(view, path, line_number);
        // One file assigning a name twice is ambiguous configuration; the
        // existing-variable policy only arbitrates against other sources.
        const auto slot = std::ranges::lower_bound(
            entries_by_name, std::string_view(entry.name_), std::ranges::less{}, entry_name);
        if (slot != entries_by_name.end() && entry_name(*slot) == std::string_view(entry.name_)) {
            throw std::invalid_argument(
                location_message(path, line_number, "duplicate variable name").c_str());
        }
        entries_by_name.insert(slot, entries.size());
        entries.push_back(std::move(entry));
    }

    if (input.bad()) {
        throw std::runtime_error(dotenv_path_message("failed to read dotenv file: ", path).c_str());
    }

    return entries;
}

}  // namespace ruvia::detail
