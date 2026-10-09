#include <algorithm>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>

#include "app/env_state.h"

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
    // identifiers (is_valid_session_id, ruvia::is_valid_config_host, ...).
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

[[nodiscard]] std::pmr::string location_message(
    const std::filesystem::path& path, std::size_t line_number, std::string_view message) {
    std::pmr::string result_value("invalid dotenv entry in ", app_resource());
    result_value += path.string();
    result_value += ':';
    result_value += std::to_string(line_number);
    result_value += ": ";
    result_value += message;
    return result_value;
}

[[nodiscard]] std::pmr::string parse_double_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    std::pmr::string result(app_resource());
    result.reserve(value.size());

    for (std::size_t index = 1; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '"') {
            consumed = index + 1;
            return result;
        }

        if (current != '\\') {
            result.push_back(current);
            continue;
        }

        if (index + 1 == value.size()) {
            throw std::invalid_argument(
                location_message(path, line_number, "unfinished escape sequence").c_str());
        }

        const char escaped = value[++index];
        switch (escaped) {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            default:
                result.push_back(escaped);
                break;
        }
    }

    throw std::invalid_argument(
        location_message(path, line_number, "unterminated double-quoted value").c_str());
}

[[nodiscard]] std::pmr::string parse_single_quoted_value(std::string_view value,
    const std::filesystem::path& path, std::size_t line_number, std::size_t& consumed) {
    const auto close = value.find('\'', 1);
    if (close == std::string_view::npos) {
        throw std::invalid_argument(
            location_message(path, line_number, "unterminated single-quoted value").c_str());
    }

    consumed = close + 1;
    return std::pmr::string(value.substr(1, close - 1), app_resource());
}

void validate_quoted_remainder(
    std::string_view value, const std::filesystem::path& path, std::size_t line_number) {
    value = trim_left(value);
    if (!value.empty() && value.front() != '#') {
        throw std::invalid_argument(
            location_message(path, line_number, "unexpected characters after quoted value").c_str());
    }
}

[[nodiscard]] std::pmr::string parse_unquoted_value(std::string_view value) {
    std::size_t end = value.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '#' && (index == 0 || is_space(value[index - 1]))) {
            end = index;
            break;
        }
    }

    return std::pmr::string(trim(value.substr(0, end)), app_resource());
}

[[nodiscard]] std::pmr::string parse_value(
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

    return parse_unquoted_value(value);
}

void strip_utf8_bom(std::string_view& line) noexcept {
    constexpr unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == bom[0] &&
        static_cast<unsigned char>(line[1]) == bom[1] &&
        static_cast<unsigned char>(line[2]) == bom[2]) {
        line.remove_prefix(3);
    }
}

[[nodiscard]] dotenv_entry parse_entry(
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

    dotenv_entry entry;
    entry.name_.assign(key.data(), key.size());
    entry.value_ = parse_value(line.substr(separator + 1), path, line_number);
    return entry;
}

}  // namespace

std::pmr::vector<dotenv_entry> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path) {
    std::pmr::vector<dotenv_entry> entries(app_resource());
    std::pmr::string line(app_resource());
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

        entries.push_back(parse_entry(view, path, line_number));
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read dotenv file: " + path.string());
    }

    return entries;
}

std::pmr::vector<dotenv_entry> read_dotenv_entries(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            throw std::runtime_error("failed to read dotenv file: " + path.string());
        }
        return {};
    }
    return read_dotenv_entries(input, path);
}

}  // namespace ruvia::detail
